// src/network_manager.cpp
#include "network_manager.h"
#include <time.h>
#include <string.h>
#include "config.h"
#include "display_manager.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <ping/ping_sock.h>

WiFiMulti wifiMulti;

static const char* activeBlacklistedSSID = nullptr;
static int consecutiveWanFailures = 0;
static unsigned long wanMissStartedMs = 0;
static const int WAN_MISSES_BEFORE_DROP = 2;
static const unsigned long WAN_DROP_AFTER_MS = 20000;
static const uint32_t PING_TIMEOUT_MS = 1000;

static bool firmwareConfirmed = false;
static bool imagePendingVerify = false;
static unsigned long firmwareBootMs = 0;
static unsigned long lastConfirmAttemptMs = 0;
static const unsigned long FIRMWARE_VERIFY_WINDOW_MS = 300000;
static const unsigned long CONFIRM_RETRY_MS = 60000;

static volatile bool pingFinished = false;
static volatile bool pingReached = false;

static void onPingSuccess(esp_ping_handle_t, void*) {
    pingReached = true;
}

static void onPingTimeout(esp_ping_handle_t, void*) {
}

static void onPingEnd(esp_ping_handle_t, void*) {
    pingFinished = true;
}

static bool pingWanOnce() {
    pingFinished = false;
    pingReached = false;

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = 1;
    config.interval_ms = PING_TIMEOUT_MS;
    config.timeout_ms = PING_TIMEOUT_MS;
    IP4_ADDR(ip_2_ip4(&config.target_addr), 1, 1, 1, 1);
    config.target_addr.type = IPADDR_TYPE_V4;

    esp_ping_callbacks_t callbacks = {};
    callbacks.on_ping_success = onPingSuccess;
    callbacks.on_ping_timeout = onPingTimeout;
    callbacks.on_ping_end = onPingEnd;

    esp_ping_handle_t session = nullptr;
    if (esp_ping_new_session(&config, &callbacks, &session) != ESP_OK) {
        return false;
    }

    esp_task_wdt_reset();
    if (esp_ping_start(session) != ESP_OK) {
        esp_ping_delete_session(session);
        return false;
    }

    unsigned long started = millis();
    while (!pingFinished && millis() - started < PING_TIMEOUT_MS + 300) {
        delay(20);
        esp_task_wdt_reset();
    }

    esp_ping_stop(session);
    esp_ping_delete_session(session);
    return pingReached;
}

static void noteWanResult(bool reachable) {
    if (reachable) {
        consecutiveWanFailures = 0;
        wanMissStartedMs = 0;
        return;
    }
    if (wanMissStartedMs == 0) {
        wanMissStartedMs = millis();
    }
    consecutiveWanFailures++;
    Serial.printf("[WATCHDOG]: WAN miss %d, %lu ms since the first miss\n",
                  consecutiveWanFailures, millis() - wanMissStartedMs);
}

static bool wanFailedEnough() {
    if (consecutiveWanFailures < WAN_MISSES_BEFORE_DROP || wanMissStartedMs == 0) {
        return false;
    }
    return millis() - wanMissStartedMs >= WAN_DROP_AFTER_MS;
}

void syncClockWithNTP() {
    Serial.println("Synchronizing clock with NTP server to counter drift...");
    configTzTime(TZ_INFO, NTP_SERVER);
}

static String pendingFirmwareUrl;

static bool readVersionPart(const char*& cursor, int& value) {
    if (*cursor == '.') {
        cursor++;
    }
    if (*cursor == '\0') {
        value = 0;
        return true;
    }
    if (*cursor < '0' || *cursor > '9') {
        return false;
    }
    long parsed = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        parsed = parsed * 10 + (*cursor - '0');
        if (parsed > 1000000) {
            return false;
        }
        cursor++;
    }
    if (*cursor != '\0' && *cursor != '.') {
        return false;
    }
    value = (int)parsed;
    return true;
}

// 1 when remote is strictly newer, 0 when it is equal or older, -1 when either string is not numeric.
static int compareVersions(const char* remote, const char* local) {
    if (remote == nullptr || local == nullptr) {
        return -1;
    }
    if (*remote == 'v' || *remote == 'V') {
        remote++;
    }
    if (*local == 'v' || *local == 'V') {
        local++;
    }

    for (int part = 0; part < 8; part++) {
        int remotePart = 0;
        int localPart = 0;
        if (!readVersionPart(remote, remotePart) || !readVersionPart(local, localPart)) {
            return -1;
        }
        if (remotePart != localPart) {
            return remotePart > localPart ? 1 : 0;
        }
        if (*remote == '\0' && *local == '\0') {
            return 0;
        }
    }
    return -1;
}

FirmwareCheckResult checkForFirmwareUpdates() {
    pendingFirmwareUrl = "";
    Serial.println("Checking GitHub repository for compiled firmware binaries...");

    HTTPClient http;
    // Initialize secure socket layer communication channel
    http.begin(CONFIG_URL); 
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Crucial for GitHub file hosting paths
    http.setTimeout(8000);

    esp_task_wdt_reset();
    int httpCode = http.GET();
    FirmwareCheckResult result = FIRMWARE_CHECK_FAILED;

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

                int versionOrder = compareVersions(latestVersion, CURRENT_VERSION);
                if (versionOrder > 0) {
                    const char* firmwareUrl = doc["firmware"];
                    if (firmwareUrl == nullptr || strncmp(firmwareUrl, "https://", 8) != 0) {
                        Serial.println("[OTA]: Newer version has no https firmware URL. Not updating.");
                        result = FIRMWARE_UP_TO_DATE;
                    } else {
                        pendingFirmwareUrl = firmwareUrl;
                        Serial.println(">>> Newer firmware release discovered on GitHub! <<<");
                        Serial.printf("[OTA]: Firmware URL: %s\n", pendingFirmwareUrl.c_str());
                        result = FIRMWARE_UPDATE_AVAILABLE;
                    }
                } else if (versionOrder == 0) {
                    Serial.println("System up to date. Remote version is not newer.");
                    result = FIRMWARE_UP_TO_DATE;
                } else {
                    Serial.println("[OTA]: Version string is not numeric. Refusing to update.");
                    result = FIRMWARE_UP_TO_DATE;
                }
            }
        } else {
            Serial.printf("JSON Deserialization failed: %s\n", error.c_str());
        }
    } else {
        Serial.printf("HTTP GET request failed. Error response code: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end(); // Safely close connections and release socket buffers back to the heap
    return result;
}

void noteFirmwareBoot() {
    firmwareBootMs = millis();
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != nullptr &&
        esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        imagePendingVerify = true;
        Serial.println("[OTA]: Running image is pending verification.");
    }
}

void confirmRunningFirmware() {
    if (firmwareConfirmed) {
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("[OTA]: Mark valid returned %s\n", esp_err_to_name(err));
    if (err == ESP_OK || !imagePendingVerify) {
        firmwareConfirmed = true;
        imagePendingVerify = false;
    }
}

void rollbackIfUnconfirmed() {
    if (!imagePendingVerify || firmwareConfirmed) {
        return;
    }
    if (millis() - firmwareBootMs < FIRMWARE_VERIFY_WINDOW_MS) {
        return;
    }
    Serial.println("[OTA]: Verification window elapsed. Rolling back.");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

static void maybeConfirmAfterLinkUp() {
    if (firmwareConfirmed || !connected) {
        return;
    }
    if (millis() - lastConfirmAttemptMs < CONFIRM_RETRY_MS) {
        return;
    }
    lastConfirmAttemptMs = millis();
    if (checkForFirmwareUpdates() != FIRMWARE_CHECK_FAILED) {
        confirmRunningFirmware();
    }
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
        esp_task_wdt_reset();
        Serial.print(".");
        timeoutCounter++;
    }
    
    // An address is enough to try an alert. One missed ping does not mean offline.
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        Serial.println("\n[BOOT]: Hardware link settled. Testing upstream internet access...");
        bool wanUp = pingWanOnce();
        noteWanResult(wanUp);
        connected = true;
        configTzTime(TZ_INFO, NTP_SERVER);
        
        String activeSSID = WiFi.SSID();
        for (int i = 0; i < NETWORK_COUNT; i++) {
            if (activeSSID.equalsIgnoreCase(String(myNetworks[i].ssid))) {
                currentNetworkIndex = i;
                break;
            }
        }
        
        netState = STATE_VERIFY_WAN;
        setNetworkDisplayHold(false);
        
        String displaySpecs = WiFi.SSID() + " | " + WiFi.localIP().toString();
        updateStatusArea("Connected successfully!", TFT_GREEN);
        updateMetricsArea("Network Details:", displaySpecs, TFT_GREEN, 1);
        Serial.printf("[BOOT SUCCESS]: Link ACTIVE on SSID: %s (Index: %d | IP: %s | WAN ping: %s)\n",
                      WiFi.SSID().c_str(), currentNetworkIndex, WiFi.localIP().toString().c_str(),
                      wanUp ? "ok" : "miss");
        return;
    }
    
    connected = false;
    netState = STATE_TRANSITION;
    setNetworkDisplayHold(true);
    Serial.println("\n[BOOT WARNING]: No Wi-Fi address. Booting into offline mode.");
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
        esp_task_wdt_reset();
        Serial.print(".");
    }
    Serial.println("\n[NTP ERROR]: Synchronization timed out. System clock fallback active.");
    return false;
}

static void beginLinkRecovery(unsigned long now) {
    connected = false;
    consecutiveWanFailures = 0;
    wanMissStartedMs = 0;
    Serial.printf("\n[WATCHDOG]: Dropping link for profile: %s\n", myNetworks[currentNetworkIndex].ssid);

    setNetworkDisplayHold(true);
    updateStatusArea("Link Lost", TFT_RED);
    updateMetricsArea("Network Details:", "RECONNECTING", TFT_RED, 1);

    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(100);

    WiFi.mode(WIFI_STA);
    delay(50);
    WiFi.begin(myNetworks[currentNetworkIndex].ssid, myNetworks[currentNetworkIndex].password);

    connectionTimestamp = now;
    netState = STATE_TRANSITION;
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
                bool wanUp = pingWanOnce();
                noteWanResult(wanUp);
                if (wanUp || !wanFailedEnough()) {
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
                    setNetworkDisplayHold(false);
                    
                    String displaySpecs = WiFi.SSID() + " | " + WiFi.localIP().toString();
                    updateStatusArea("Connected successfully!", TFT_GREEN);
                    updateMetricsArea("Network Details:", displaySpecs, TFT_GREEN, 1);
                    isClockLayoutDrawn = false;
                    Serial.printf("\n[WATCHDOG]: Initial Scan Settled on SSID: %s\n", WiFi.SSID().c_str());
                    maybeConfirmAfterLinkUp();
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
                
                bool wanUp = pingWanOnce();
                noteWanResult(wanUp);
                if (wanUp || !wanFailedEnough()) {
                    Serial.printf("\n[WATCHDOG SUCCESS]: Profile %s has an address. WAN ping %s.\n",
                                  WiFi.SSID().c_str(), wanUp ? "ok" : "not yet failed enough to drop");
                    
                    configTzTime(TZ_INFO, NTP_SERVER); 
                    waitForNtpSync(3000); 
                    
                    connected = true;
                    netState = STATE_VERIFY_WAN;
                    setNetworkDisplayHold(false);
                    
                    String displaySpecs = WiFi.SSID() + " | " + WiFi.localIP().toString();
                    updateStatusArea("Connected successfully!", TFT_GREEN);
                    updateMetricsArea("Network Details:", displaySpecs, TFT_GREEN, 1);
                    isClockLayoutDrawn = false;
                    maybeConfirmAfterLinkUp();
                    return;
                } else {
                    Serial.println("[WATCHDOG WARNING]: Router connected, but WAN missed too many checks. Rotating.");
                }
            } 
            
            // Check if we have spent too much time waiting for this specific profile to negotiate an IP address
            if (currentMillis - connectionTimestamp > AUTH_TIMEOUT_MS || currentStatus == WL_CONNECT_FAILED) {
                // Increment to the next fallback credential block safely
                currentNetworkIndex = (currentNetworkIndex + 1) % NETWORK_COUNT;
                Serial.printf("\n[WATCHDOG]: Profile failed or timed out. Rotating tracking index to slot %d: -> Next target: %s\n", 
                              currentNetworkIndex, myNetworks[currentNetworkIndex].ssid);
                
                // Update screen to explicitly show exactly what network it is fighting to connect to
                consecutiveWanFailures = 0;
                setNetworkDisplayHold(true);
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
            beginLinkRecovery(currentMillis);
            break;
        }

        // =========================================================================
        // STATE 3: STABLE RUNTIME HEARTBEAT VERIFICATION
        // =========================================================================
        case STATE_VERIFY_WAN: {
            if (currentStatus != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
                beginLinkRecovery(currentMillis);
                return;
            }

            bool wanUp = pingWanOnce();
            noteWanResult(wanUp);
            if (wanUp) {
                Serial.printf("[WATCHDOG]: Heartbeat verified. WAN link active on '%s'\n", WiFi.SSID().c_str());
                maybeConfirmAfterLinkUp();
            } else if (wanFailedEnough()) {
                Serial.printf("\n[WARNING]: '%s' failed WAN checks for %lu ms. Reconnecting.\n",
                              WiFi.SSID().c_str(), millis() - wanMissStartedMs);
                beginLinkRecovery(currentMillis);
            } else {
                Serial.printf("[WATCHDOG]: WAN miss kept (%d, need %lu ms) on '%s'\n",
                              consecutiveWanFailures, WAN_DROP_AFTER_MS, WiFi.SSID().c_str());
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

    if (!pendingFirmwareUrl.startsWith("https://")) {
        Serial.println("[OTA]: Refusing download without an https URL from version.json.");
        updateMetricsArea("Flash Status:", "URL Missing", TFT_RED, 1);
        return;
    }

    HTTPClient http;
    http.begin(pendingFirmwareUrl);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(8000);

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