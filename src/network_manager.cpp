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

        // CLEAN V7 FIX: Removed StaticJsonDocument<384>, replaced with modern JsonDocument
        JsonDocument doc; 
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            const char* latestVersion = doc["version"]; // Extract the "version" string value
            
            // CRITICAL PROTECTION: Ensure the key actually exists before running strcmp
            if (latestVersion == nullptr) {
                Serial.println("[OTA ERROR]: Manifest parsed, but 'version' key is missing or invalid.");
            } else {
                Serial.printf("Local Core Version: %s\n", CURRENT_VERSION);
                Serial.printf("GitHub Remote Version: %s\n", latestVersion);

                // Compare strings. If they do not match, we have an update available!
                if (strcmp(latestVersion, CURRENT_VERSION) != 0) {
                    Serial.println(">>> New firmware release discovered on GitHub! <<<");
                    updateAvailable = true;
                } else {
                    Serial.println("System up to date. Version strings match.");
                }
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

// 1. Define a named type for your state machine layout
enum NetworkState { 
    STATE_INITIAL_SCAN, 
    STATE_TRANSITION, 
    STATE_HARDWARE_DROP, 
    STATE_VERIFY_WAN 
};

// 2. Declare your static tracking instance using that new type
static NetworkState netState = STATE_INITIAL_SCAN;
static int currentNetworkIndex = 0;
static unsigned long connectionTimestamp = 0;
const unsigned long AUTH_TIMEOUT_MS = 15000; 

// This function now completely blocks startup until true internet is found or it times out
void initialWIFI() {
    updateStatusArea("Scanning Networks...", TFT_YELLOW);
    Serial.println("\n[BOOT]: Initializing blocking network scan via WiFiMulti...");
    
    // Clear out any stale registration profiles and prime the credentials list
    for (int i = 0; i < NETWORK_COUNT; i++) {
        wifiMulti.addAP(myNetworks[i].ssid, myNetworks[i].password);
    }
    
    int timeoutCounter = 0;
    // 1. Loop until the hardware registers secure a link layer connection (Max 10 seconds)
    while (wifiMulti.run() != WL_CONNECTED && timeoutCounter < 20) {
        delay(500);
        Serial.print(".");
        timeoutCounter++;
    }
    
    // 2. Hardware link is up! Now immediately verify if it has a real WAN route to the internet
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        Serial.println("\n[BOOT]: Hardware link settled. Testing upstream internet access...");
        
        if (Ping.ping(IPAddress(1, 1, 1, 1), 1)) {
            connected = true; // Sets your global tracking variable TRUE before returning!
            configTzTime(TZ_INFO, NTP_SERVER); // Sync clock parameters immediately
            
            // Sync your array tracking index to reality based on where WiFiMulti landed
            String activeSSID = WiFi.SSID();
            for (int i = 0; i < NETWORK_COUNT; i++) {
                if (activeSSID.equalsIgnoreCase(String(myNetworks[i].ssid))) {
                    currentNetworkIndex = i;
                    break;
                }
            }
            
            // Transition our background watchdog state machine straight to stable heartbeat mode
            netState = STATE_VERIFY_WAN; 
            
            String displaySpecs = WiFi.SSID() + " | " + WiFi.localIP().toString();
            updateStatusArea("Connected successfully!", TFT_GREEN);
            updateMetricsArea("Network Details:", displaySpecs, TFT_GREEN, 1);
            Serial.printf("[BOOT SUCCESS]: Internet ACTIVE on SSID: %s (Index: %d | IP: %s)\n", 
                          WiFi.SSID().c_str(), currentNetworkIndex, WiFi.localIP().toString().c_str());
            return;
        }
    }
    
    // Fallback if the hardware connected but the internet was completely dead
    connected = false;
    netState = STATE_TRANSITION; // Tell the background watchdog to immediately start cycling channels
    Serial.println("\n[BOOT WARNING]: Hardware linked, but WAN internet ping failed. Booting into offline mode.");
    updateStatusArea("All Connections Failed", TFT_RED);
    updateMetricsArea("Network Details:", "OFFLINE", TFT_RED, 1);
}

bool waitForNtpSync(uint32_t timeoutMs) {
    Serial.print("[NTP]: Awaiting synchronization packet");
    uint32_t startAttempt = millis();
    struct tm timeinfo;
    
    while (millis() - startAttempt < timeoutMs) {
        // Check if the system clock registers have a valid year yet (anything past 1970 means synced!)
        if (getLocalTime(&timeinfo, 0)) { 
            Serial.println("\n[NTP SUCCESS]: Internal hardware clock synchronized!");
            return true;
        }
        delay(100);
        Serial.print(".");
    }
    Serial.println("\n[NTP ERROR]: Synchronization timed out. System clock fallback active.");
    return false;
}

void checkLiveConnection() {
    wl_status_t currentStatus = WiFi.status();
    unsigned long currentMillis = millis();

    switch (netState) {
        
        // =========================================================================
        // STATE 0: INITIAL BOOT SCAN (Asynchronous entry verification)
        // =========================================================================
        case STATE_INITIAL_SCAN: {
            if (wifiMulti.run() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
                if (Ping.ping(IPAddress(1, 1, 1, 1), 1)) {
                    configTzTime(TZ_INFO, NTP_SERVER); 
                    waitForNtpSync(3000);
                    
                    connected = true;
                    String activeSSID = WiFi.SSID();
                    for (int i = 0; i < NETWORK_COUNT; i++) {
                        if (activeSSID.equalsIgnoreCase(String(myNetworks[i].ssid))) {
                            currentNetworkIndex = i;
                            break;
                        }
                    }
                    
                    netState = STATE_VERIFY_WAN;
                    
                    String displaySpecs = WiFi.SSID() + " | " + WiFi.localIP().toString();
                    updateStatusArea("Connected successfully!", TFT_GREEN);
                    updateMetricsArea("Network Details:", displaySpecs, TFT_GREEN, 1);
                    isClockLayoutDrawn = false;
                    Serial.printf("\n[WATCHDOG]: Initial Scan Settled on SSID: %s\n", WiFi.SSID().c_str());
                }
            }
            
            if (!connected && (currentMillis - connectionTimestamp > AUTH_TIMEOUT_MS)) {
                Serial.println("\n[WATCHDOG]: Initial multi-scan failed to verify WAN. Dropping into active profile rotation...");
                netState = STATE_TRANSITION;
                connectionTimestamp = currentMillis;
                // Prime the first profile change instantly
                WiFi.disconnect(true, true);
                WiFi.mode(WIFI_STA);
                delay(50);
                WiFi.begin(myNetworks[currentNetworkIndex].ssid, myNetworks[currentNetworkIndex].password);
            }
            break;
        }

        // =========================================================================
        // STATE 1: ACTIVE TRANSITION & SCAN CYCLING (Enforces profile rotations)
        // =========================================================================
        case STATE_TRANSITION: {
            Serial.printf("[WATCHDOG-PROBE]: Testing Profile %d [%s] -> Status Code: %d\n", 
                          currentNetworkIndex, myNetworks[currentNetworkIndex].ssid, currentStatus);
            
            // If the hardware link is secured, test for true WAN internet right now
            if (currentStatus == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
                Serial.printf("[WATCHDOG-PROBE]: Hardware link established for '%s'. Verifying WAN router route...\n", WiFi.SSID().c_str());
                
                if (Ping.ping(IPAddress(1, 1, 1, 1), 1)) {
                    Serial.printf("\n[WATCHDOG SUCCESS]: Upstream WAN verified on profile: %s. Handshake complete!\n", WiFi.SSID().c_str());
                    
                    configTzTime(TZ_INFO, NTP_SERVER); 
                    waitForNtpSync(3000); 
                    
                    connected = true;
                    netState = STATE_VERIFY_WAN;
                    
                    String displaySpecs = WiFi.SSID() + " | " + WiFi.localIP().toString();
                    updateStatusArea("Connected successfully!", TFT_GREEN);
                    updateMetricsArea("Network Details:", displaySpecs, TFT_GREEN, 1);
                    isClockLayoutDrawn = false;
                    return;
                } else {
                    Serial.println("[WATCHDOG WARNING]: Router connected, but WAN interface is dead. Forcing early channel rotation.");
                }
            } 
            
            // Check if we have spent too much time waiting for this specific profile to negotiate an IP address
            if (currentMillis - connectionTimestamp > AUTH_TIMEOUT_MS || currentStatus == WL_CONNECT_FAILED) {
                // Increment to the next fallback credential block safely
                currentNetworkIndex = (currentNetworkIndex + 1) % NETWORK_COUNT;
                Serial.printf("\n[WATCHDOG]: Profile failed or timed out. Rotating tracking index to slot %d: -> Next target: %s\n", 
                              currentNetworkIndex, myNetworks[currentNetworkIndex].ssid);
                
                // Update screen to explicitly show exactly what network it is fighting to connect to
                updateStatusArea("Cycling Profile...", TFT_YELLOW);
                updateMetricsArea("Targeting AP:", myNetworks[currentNetworkIndex].ssid, TFT_YELLOW, 1);

                WiFi.disconnect(true, true);
                WiFi.mode(WIFI_OFF);
                delay(100); 
                
                WiFi.mode(WIFI_STA);
                delay(50);
                WiFi.begin(myNetworks[currentNetworkIndex].ssid, myNetworks[currentNetworkIndex].password);
                connectionTimestamp = currentMillis; // Reset timeout clock for the fresh profile target
            }
            break;
        }

        // =========================================================================
        // STATE 2: UNEXPECTED HARDWARE DROP
        // =========================================================================
        case STATE_HARDWARE_DROP: {
            connected = false;
            Serial.printf("\n[WATCHDOG]: Critical drop detected. Forcing hardware stack reset for profile: %s\n", myNetworks[currentNetworkIndex].ssid);
            
            updateStatusArea("Link Lost", TFT_RED);
            updateMetricsArea("Network Details:", "RECONNECTING", TFT_RED, 1);

            WiFi.disconnect(true, true);
            WiFi.mode(WIFI_OFF);
            delay(100);

            WiFi.mode(WIFI_STA);
            delay(50);
            WiFi.begin(myNetworks[currentNetworkIndex].ssid, myNetworks[currentNetworkIndex].password);
            
            connectionTimestamp = currentMillis;
            netState = STATE_TRANSITION;
            break;
        }

        // =========================================================================
        // STATE 3: STABLE RUNTIME HEARTBEAT VERIFICATION
        // =========================================================================
        case STATE_VERIFY_WAN: {
            if (currentStatus != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
                netState = STATE_HARDWARE_DROP;
                return;
            }

            if (Ping.ping(IPAddress(1, 1, 1, 1), 1)) {
                Serial.printf("[WATCHDOG]: Heartbeat verified. WAN link active on '%s'\n", WiFi.SSID().c_str());
            } 
            else {
                Serial.printf("\n[WARNING]: Running connection '%s' lost its upstream WAN gateway internet access.\n", WiFi.SSID().c_str());
                netState = STATE_HARDWARE_DROP; // Divert into recovery mode to rotate channels immediately
            }
            break;
        }
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