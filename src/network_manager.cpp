// src/network_manager.cpp
#include "network_manager.h"
#include <time.h>
#include "config.h"
#include "display_manager.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ESP32Ping.h>

WiFiMulti wifiMulti;

static const char* activeBlacklistedSSID = nullptr;

void syncClockWithNTP() {
    Serial.println("Synchronizing clock with NTP server to counter drift...");
    configTzTime(TZ_INFO, NTP_SERVER);
}

bool checkForFirmwareUpdates() {
    Serial.println("Checking GitHub repository for compiled firmware binaries...");
    
    // Ensure we are fully connected before touching raw sockets
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("OTA Check Aborted: Network currently offline.");
        return false;
    }

    HTTPClient http;
    // Initialize secure socket layer communication channel
    http.begin(CONFIG_URL); 
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Crucial for GitHub file hosting paths

    int httpCode = http.GET();
    bool updateAvailable = false;

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.println("Successfully downloaded manifest payload:");
        Serial.println(payload);

        // Allocate memory for parsing the incoming JSON payload string
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            const char* latestVersion = doc["version"]; // Extract the "version" string value
            
            Serial.printf("Local Core Version: %s\n", CURRENT_VERSION);
            Serial.printf("GitHub Remote Version: %s\n", latestVersion);

            // Compare strings. If they do not match, we have an update available!
            if (strcmp(latestVersion, CURRENT_VERSION) != 0) {
                Serial.println(">>> New firmware release discovered on GitHub! <<<");
                updateAvailable = true;
            } else {
                Serial.println("System up to date. Version strings match.");
            }
        } else {
            Serial.printf("JSON Deserialization failed: %s\n", error.c_str());
        }
    } else {
        Serial.printf("HTTP GET request failed. Error response code: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end(); // Safely close connections and release socket buffers back to the heap
    return updateAvailable;
}

bool tryWiFiConnection() {
    Serial.println("\nStarting network scan via WiFiMulti...");
    
    for (int i = 0; i < NETWORK_COUNT; i++) {
        wifiMulti.addAP(myNetworks[i].ssid, myNetworks[i].password);
    }

    int timeoutCounter = 0;
    while (wifiMulti.run() != WL_CONNECTED && timeoutCounter < 20) {
        delay(500);
        Serial.print(".");
        timeoutCounter++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        connected = true; // Set your global tracking variable
        
        // Sync Time with Internet registers immediately
        configTzTime(TZ_INFO, NTP_SERVER);
        Serial.println("\nWi-Fi connection established. NTP initiated.");
        Serial.printf("IP Assigned: %s\n", WiFi.localIP().toString().c_str());
        
        return true;
    } else {
        connected = false;
        Serial.println("\nAll connection profiles failed to handshake.");
        return false;
    }
}

void initialWIFI() {
    updateStatusArea("Scanning Networks...", TFT_YELLOW);
    
    // Call your pure networking function logic
    bool success = tryWiFiConnection();

    if (success) {
        String displaySpecs = WiFi.SSID() + " | " + WiFi.localIP().toString();
        updateStatusArea("Connected successfully!", TFT_GREEN);
        updateMetricsArea("Network Details:", displaySpecs, TFT_GREEN, 1);
    } else {
        updateStatusArea("All Connections Failed", TFT_RED);
        updateMetricsArea("Network Details:", "OFFLINE", TFT_RED, 1);
    }
}

static int currentNetworkIndex = 0;
static unsigned long connectionTimestamp = 0;
static bool isTransitioning = false;
const unsigned long AUTH_TIMEOUT_MS = 15000; // Give the radio a solid 15s to handshake

void checkLiveConnection() {
    wl_status_t currentStatus = WiFi.status();
    unsigned long currentMillis = millis();

    // =========================================================================
    // STATE 1: ACTIVE TRANSITION (Waiting for Hardware Auth & DHCP)
    // =========================================================================
    if (isTransitioning) {
        // If we successfully locked in a hardware connection AND got an IP address
        if (currentStatus == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
            Serial.printf("\n[WATCHDOG]: Hardware link settled on SSID: %s. Handshake complete!\n", WiFi.SSID().c_str());
            isTransitioning = false; // Disarm transition armor, hand over to WAN tester
            connected = true;
        } 
        // If we are still shifting channels, check if we've timed out
        else if (currentMillis - connectionTimestamp > AUTH_TIMEOUT_MS) {
            currentNetworkIndex = (currentNetworkIndex + 1) % NETWORK_COUNT;
            Serial.printf("\n[WATCHDOG]: Handshake timeout on profile %d. Cycling to: %s\n", 
                          currentNetworkIndex, myNetworks[currentNetworkIndex].ssid);
            
            WiFi.disconnect(true, true);
            WiFi.mode(WIFI_OFF);
            delay(100); 
            
            WiFi.mode(WIFI_STA);
            delay(50);
            WiFi.begin(myNetworks[currentNetworkIndex].ssid, myNetworks[currentNetworkIndex].password);
            
            connectionTimestamp = currentMillis; // Reset the timeout clock for the new profile
        } 
        else {
            Serial.printf("[WATCHDOG]: Awaiting connection lease for '%s'... (%dms elapsed)\n", 
                          myNetworks[currentNetworkIndex].ssid, (int)(currentMillis - connectionTimestamp));
        }
        
        // CRITICAL CRITICAL CRITICAL: Absolute hard exit. 
        // Do not let ANY other network logic or ping tests fire while transitioning!
        return; 
    }

    // =========================================================================
    // STATE 2: UNEXPECTED HARDWARE DROP (Out-of-band link loss)
    // =========================================================================
    if (currentStatus != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
        if (connected) { connected = false; }
        
        Serial.printf("\n[WATCHDOG]: Connection lost. Re-bootstrapping active profile: %s\n", myNetworks[currentNetworkIndex].ssid);
        WiFi.mode(WIFI_STA);
        delay(50);
        WiFi.begin(myNetworks[currentNetworkIndex].ssid, myNetworks[currentNetworkIndex].password);
        
        connectionTimestamp = currentMillis;
        isTransitioning = true;
        return;
    }

    // =========================================================================
    // STATE 3: STABLE CONNECTION -> VERIFY UPSTREAM WAN INTERNET ACCESS
    // =========================================================================
    bool internetAccess = Ping.ping(IPAddress(1, 1, 1, 1), 1);

    if (internetAccess) {
        if (!connected) {
            connected = true;
            
            // Sync our tracker index with wherever WiFiMulti landed on startup
            String activeSSID = WiFi.SSID();
            for (int i = 0; i < NETWORK_COUNT; i++) {
                if (activeSSID.equalsIgnoreCase(String(myNetworks[i].ssid))) {
                    currentNetworkIndex = i;
                    break;
                }
            }
            Serial.printf("\n[WATCHDOG]: Internet ACTIVE on SSID: %s (Index: %d | IP: %s)\n", 
                          WiFi.SSID().c_str(), currentNetworkIndex, WiFi.localIP().toString().c_str());
        }
    } 
    else {
        // UPSTREAM WAN IS DEAD
        if (connected) { connected = false; }
        
        String deadSSID = WiFi.SSID();
        Serial.printf("\n[WARNING]: SSID '%s' has no upstream WAN internet access.\n", deadSSID.c_str());
        
        // Sync our tracking position to reality before shifting
        for (int i = 0; i < NETWORK_COUNT; i++) {
            if (deadSSID.equalsIgnoreCase(String(myNetworks[i].ssid))) {
                currentNetworkIndex = i;
                break;
            }
        }

        // Increment to the next fallback profile cleanly
        currentNetworkIndex = (currentNetworkIndex + 1) % NETWORK_COUNT;
        Serial.printf("[WATCHDOG]: Routing away from dead link -> Fallback profile: %s\n", myNetworks[currentNetworkIndex].ssid);

        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        delay(100);
        
        WiFi.mode(WIFI_STA);
        delay(50);
        WiFi.begin(myNetworks[currentNetworkIndex].ssid, myNetworks[currentNetworkIndex].password);
        
        connectionTimestamp = currentMillis;
        isTransitioning = true; // Lock down transition armor
    }
}

void performOTAUpdate() {
    Serial.println("Initializing OTA firmware installation sequence...");
    
    // 1. Wipe the screen to give the flashing process an exclusive status window
    clearScreen();
    drawHeader("SYSTEM UPGRADE", TFT_RED);
    updateMetricsArea("Flash Status:", "Initializing...", TFT_YELLOW, 1);

    HTTPClient http;
    http.begin(FIRMWARE_URL);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("OTA HTTP connection failed. Code: %d\n", httpCode);
        updateMetricsArea("Flash Status:", "Download Failed", TFT_RED, 1);
        http.end();
        return;
    }

    // 2. Extract total stream payload length from headers
    int contentLength = http.getSize();
    if (contentLength <= 0) {
        Serial.println("OTA Error: Invalid binary payload size string.");
        updateMetricsArea("Flash Status:", "Size Invalid", TFT_RED, 1);
        http.end();
        return;
    }

    // 3. Inform the internal kernel memory management partition layout to prepare for installation
    bool canBegin = Update.begin(contentLength, U_FLASH);
    
    if (canBegin) {
        Serial.printf("Flashing script initialized. Payload size: %d bytes. Streaming...\n", contentLength);
        updateMetricsArea("Flash Status:", "Streaming Binary", TFT_CYAN, 1);

        // Get the active live network stream socket pointer from the chip architecture
        WiFiClient* client = http.getStreamPtr();
        
        // Write the stream bytes directly from network sockets straight to flash registers
        size_t written = Update.writeStream(*client);

        if (written == contentLength) {
            Serial.printf("Successfully flashed %d bytes into app registers.\n", written);
        } else {
            Serial.printf("Flash mismatch error: Fused %d / %d bytes.\n", written, contentLength);
        }

        // 4. Verify structural integrity hashes and close out filesystem write blocks
        if (Update.end()) {
            if (Update.isFinished()) {
                Serial.println("OTA Verification Successful! Rebooting engine now...");
                updateMetricsArea("Flash Status:", "SUCCESS. REBOOTING", TFT_GREEN, 1);
                delay(2000);
                
                // Hard reset execution lines on the core silicon hardware level
                ESP.restart(); 
            } else {
                Serial.println("OTA Error: Flash verification finished but marked incomplete.");
                updateMetricsArea("Flash Status:", "Verify Failed", TFT_RED, 1);
            }
        } else {
            Serial.printf("Flash verification failed. Core Error String: %s\n", Update.errorString());
            updateMetricsArea("Flash Status:", "Verification Err", TFT_RED, 1);
        }
    } else {
        Serial.println("OTA Error: Not enough storage spaces allocated to accept this file size layout.");
        updateMetricsArea("Flash Status:", "Partition Error", TFT_RED, 1);
    }

    http.end();
}