#include <Arduino.h>
#include <WiFi.h>
#include <AsyncHTTPRequest_Generic.h> 
#include <string.h>
#include "config.h"
#include "arcade_manager.h"
#include "network_manager.h" 
#include "display_manager.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <ArduinoJson.h>
#include <ctype.h>
#define _TASK_STATUS_REQUEST
#include <TaskSchedulerDeclarations.h>

// Pin Configuration
const int BUTTON_PIN = 26;  
const int TEST_ONE_PIN = 35;
const int TEST_ALL_PIN = 0;
const int LED_PIN = 27;
extern Task taskLEDBlink;

// Debounce Tracking Layout
bool lastButtonState = HIGH;      
bool currentButtonState = HIGH;   
unsigned long lastDebounceTime = 0;  
const unsigned long DEBOUNCE_DELAY = 50; 

bool lastTestButtonState = HIGH;
bool currentTestButtonState = HIGH;
unsigned long lastTestDebounceTime = 0;
const unsigned long TEST_DEBOUNCE_DELAY = 50;

bool lastTestAllButtonState = HIGH;
bool currentTestAllButtonState = HIGH;
unsigned long lastTestAllDebounceTime = 0;
const unsigned long TEST_ALL_DEBOUNCE_DELAY = 50;

// Double-tap confirmation flow
bool doubleTapPending = false;
unsigned long doubleTapArmTime = 0;
const unsigned long DOUBLE_TAP_WINDOW_MS = 5000;

// Connection Pooling Engine
#define MAX_ASYNC_REQUESTS 10
AsyncHTTPRequest requestPool[MAX_ASYNC_REQUESTS];
int currentPoolIndex = 0;

int pendingRequestsCount = 0;
bool dispatchHasFailed = false;

String urlEncode(const String& value) {
    String encoded;
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            encoded += hex;
        }
    }
    return encoded;
}

String buildAlertEndpoint(const String& route, const String& password) {
    String endpoint = String(BACKEND_BASE_URL) + "/" + urlEncode(String(YARD_ID));
    endpoint += route;
    if (password.length() > 0) {
        endpoint += "?password=" + urlEncode(password);
    }
    return endpoint;
}

// Convert state integers to human-readable text for logging
const char* getReadyStateText(int state) {
    switch(state) {
        case 0:  return "UNSENT (0)";
        case 1:  return "OPENED / CONNECTING (1)";
        case 2:  return "HEADERS RECEIVED (2)";
        case 3:  return "LOADING / TRANSMITTING (3)";
        case 4:  return "DONE / COMPLETED (4)";
        default: return "UNKNOWN STATE";
    }
}

// 2. Updated background callback handler
void onAsyncRequestComplete(void* optParm, AsyncHTTPRequest* request, int readyState) {
    int poolId = (int)optParm; 
    
    Serial.print("[ASYNC POOL #"); Serial.print(poolId);
    Serial.print("]: State transitioned to -> ");
    Serial.println(getReadyStateText(readyState));

    // Hardcoded '4' handles the completed signal across all versions of the library
    if (readyState == 4) { 
        int responseCode = request->responseHTTPcode();
        Serial.print("[ASYNC CALLBACK #"); Serial.print(poolId);
        Serial.print("]: Process finished. Remote Server HTTP Code: ");
        Serial.println(responseCode);
        
        String responseBody = request->responseText();
        if (responseBody.length() > 0) {
            Serial.print("[SERVER RESPONSE #"); Serial.print(poolId);
            Serial.print("]: "); Serial.println(responseBody);
        }
        
        if (responseCode < 200 || (responseCode >= 300 && (responseCode < 301 || responseCode > 308))) {
            dispatchHasFailed = true;
            Serial.print("[ERROR]: Pool slot #"); Serial.print(poolId);
            Serial.println(" caught an invalid execution signal response.");
        }

        if (pendingRequestsCount > 0) {
            pendingRequestsCount--;
        }

        Serial.print("[TRACKER]: Outstanding packets remaining in air: ");
        Serial.println(pendingRequestsCount);

        if (pendingRequestsCount == 0) {
            if (dispatchHasFailed) {
                Serial.println("[STATUS]: Asynchronous pipeline completed with errors.");
                updateMetricsArea("Broadcast", "FAIL", TFT_RED, 3);
            } else {
                Serial.println("[STATUS]: Asynchronous pipeline completed successfully!");
                updateMetricsArea("Broadcast", "SUCCESS", TFT_GREEN, 3);
            }
        }
    }
}

// Helper to get next pool index with registration parameters logging
AsyncHTTPRequest* getFreshRequestFromPool(int indexIdentifier) {
    if (currentPoolIndex >= MAX_ASYNC_REQUESTS) {
        Serial.println("[POOL WARNING]: Wrapping around memory allocations boundary indexes.");
        currentPoolIndex = 0; 
    }
    
    AsyncHTTPRequest* req = &requestPool[currentPoolIndex++];
    req->abort(); 
    
    // Pass the index as an optional parameter context pointer so callbacks can identify themselves
    req->onReadyStateChange(onAsyncRequestComplete, (void*)indexIdentifier);
    return req;
}

// ============================================================================
// MAIN EXECUTION LOGIC
// ============================================================================
void executeButtonAction() {
    Serial.println("\n==================================================");
    Serial.println("[ACTION]: Launching Secure Cloud Broadcaster...");
    Serial.println("==================================================");
    
    updateMetricsArea("Syncing...", "FETCHING", TFT_YELLOW, 2);

    #if defined(BACKEND_BASE_URL) && defined(API_PASSWORD) && defined(YARD_ID)
        HTTPClient http;
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.addHeader("Content-Type", "application/json");

        String endpoint = buildAlertEndpoint("/alert/broadcast", String(API_PASSWORD));
        JsonDocument requestDoc;
        requestDoc["password"] = String(API_PASSWORD);
        String requestBody;
        serializeJson(requestDoc, requestBody);

        Serial.print("[CLOUD DATA]: POST "); Serial.println(endpoint);
        Serial.print("[CLOUD DATA]: Payload: "); Serial.println(requestBody);

        bool requestStarted = false;
        if (endpoint.startsWith("https://")) {
            WiFiClientSecure secureClient;
            secureClient.setInsecure();
            requestStarted = http.begin(secureClient, endpoint);
        } else {
            requestStarted = http.begin(endpoint);
        }

        if (requestStarted) {
            int httpCode = http.POST(requestBody);
            
            if (httpCode == 200 || httpCode == 429 || (httpCode >= 301 && httpCode <= 308)) {
                String responseBody = http.getString();
                Serial.print("[CLOUD DATA]: Received Payload: "); Serial.println(responseBody);

                JsonDocument doc; 
                DeserializationError error = deserializeJson(doc, responseBody);

                if (!error) {
                    String status = doc["status"].as<String>();
                    
                    if (status == "unauthorized") {
                        updateMetricsArea("Auth Error", "BAD PASS", TFT_RED, 2);
                        triggerLedPattern(1000, 127, 2);
                        Serial.println("[CRITICAL]: Backend rejected our password credential!");
                    } else if (status == "error") {
                        String minsLeft = doc["minutes_remaining"].as<String>();
                        String rateLimitMessage = "Wait " + minsLeft + "m";
                        triggerLedPattern(1000, 127, 3);
                        updateMetricsArea("Rate Limit", rateLimitMessage.c_str(), TFT_ORANGE, 2);
                    } else if (status == "success") {
                        updateMetricsArea("Broadcast", "SUCCESS", TFT_GREEN, 3);
                        triggerLedPattern(250, 255, 4);
                        Serial.println("[SUCCESS]: Network queues successfully triggered.");
                    } else {
                        updateMetricsArea("Broadcast", "FAIL", TFT_RED, 3);
                        triggerLedPattern(1000, 127, 5);
                        Serial.println("[FAILURE]: Failed to trigger network queues.");
                        Serial.println("[DEBUG]: Unrecognized status field in JSON response: " + status);
                    }
                } else {
                    updateMetricsArea("Broadcast", "FAIL", TFT_RED, 3);
                    triggerLedPattern(1000, 127, 6);
                    Serial.print("[ERROR]: Failed to parse JSON response. Error code: ");
                    Serial.println(error.c_str());
                }
            } else {
                updateMetricsArea("Broadcast", "FAIL", TFT_RED, 3);
                triggerLedPattern(1000, 127, 7);
                Serial.print("[ERROR]: HTTP request failed with code: ");
                Serial.println(httpCode);
            }
            http.end();
        }
    #else
        Serial.println("[ERROR]: BACKEND_BASE_URL, YARD_ID or API_PASSWORD macro configuration missing.");
        updateMetricsArea("Broadcast", "FAIL", TFT_RED, 3);
    #endif

}

void executeTestButtonAction() {
    Serial.println("\n==================================================");
    Serial.println("[ACTION]: Launching Test Request...");
    Serial.println("==================================================");

    updateMetricsArea("Testing...", "TEST", TFT_BLUE, 2);

    #if defined(BACKEND_BASE_URL) && defined(API_PASSWORD) && defined(YARD_ID)
        HTTPClient http;
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.addHeader("Content-Type", "application/json");

        String endpoint = buildAlertEndpoint("/alert/testOne", String(API_PASSWORD));
        JsonDocument requestDoc;
        requestDoc["password"] = String(API_PASSWORD);
        requestDoc["mode"] = "test";
        String requestBody;
        serializeJson(requestDoc, requestBody);

        Serial.print("[TEST DATA]: POST "); Serial.println(endpoint);
        Serial.print("[TEST DATA]: Payload: "); Serial.println(requestBody);

        bool requestStarted = false;
        if (endpoint.startsWith("https://")) {
            WiFiClientSecure secureClient;
            secureClient.setInsecure();
            requestStarted = http.begin(secureClient, endpoint);
        } else {
            requestStarted = http.begin(endpoint);
        }

        if (requestStarted) {
            int httpCode = http.POST(requestBody);

            if (httpCode == 200 || (httpCode >= 301 && httpCode <= 308)) {
                String responseBody = http.getString();
                Serial.print("[TEST DATA]: Received Payload: "); Serial.println(responseBody);
                updateMetricsArea("Test", "OK", TFT_GREEN, 3);
                triggerLedPattern(1000, 127, 6);
            } else {
                updateMetricsArea("Test", "FAIL", TFT_RED, 3);
                triggerLedPattern(3000, 127, 2);
                Serial.print("[ERROR]: Test request failed with code: ");
                Serial.println(httpCode);
            }
            http.end();
        }
    #else
        Serial.println("[ERROR]: BACKEND_BASE_URL, YARD_ID or API_PASSWORD macro configuration missing.");
        updateMetricsArea("Test", "FAIL", TFT_RED, 3);
    #endif
}

void executeTestAllButtonAction() {
    Serial.println("\n==================================================");
    Serial.println("[ACTION]: Launching Test All Request...");
    Serial.println("==================================================");

    updateMetricsArea("Testing...", "TEST ALL", TFT_BLUE, 2);

    #if defined(BACKEND_BASE_URL) && defined(API_PASSWORD) && defined(YARD_ID)
        HTTPClient http;
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.addHeader("Content-Type", "application/json");

        String endpoint = buildAlertEndpoint("/alert/testAll", String(API_PASSWORD));
        JsonDocument requestDoc;
        requestDoc["password"] = String(API_PASSWORD);
        requestDoc["mode"] = "testAll";
        String requestBody;
        serializeJson(requestDoc, requestBody);

        Serial.print("[TEST ALL DATA]: POST "); Serial.println(endpoint);
        Serial.print("[TEST ALL DATA]: Payload: "); Serial.println(requestBody);

        bool requestStarted = false;
        if (endpoint.startsWith("https://")) {
            WiFiClientSecure secureClient;
            secureClient.setInsecure();
            requestStarted = http.begin(secureClient, endpoint);
        } else {
            requestStarted = http.begin(endpoint);
        }

        if (requestStarted) {
            int httpCode = http.POST(requestBody);

            if (httpCode == 200 || (httpCode >= 301 && httpCode <= 308)) {
                String responseBody = http.getString();
                Serial.print("[TEST ALL DATA]: Received Payload: "); Serial.println(responseBody);
                updateMetricsArea("Test All", "OK", TFT_GREEN, 3);
                triggerLedPattern(1000, 127, 6);
            } else {
                updateMetricsArea("Test All", "FAIL", TFT_RED, 3);
                triggerLedPattern(3000, 127, 2);
                Serial.print("[ERROR]: Test All request failed with code: ");
                Serial.println(httpCode);
            }
            http.end();
        }
    #else
        Serial.println("[ERROR]: BACKEND_BASE_URL, YARD_ID or API_PASSWORD macro configuration missing.");
        updateMetricsArea("Test All", "FAIL", TFT_RED, 3);
    #endif
}

void initArcadeHardware() {
    pinMode(BUTTON_PIN, INPUT_PULLUP); 
    pinMode(TEST_ONE_PIN, INPUT_PULLUP);
    pinMode(TEST_ALL_PIN, INPUT_PULLUP);
    ledcSetup(0, 5000, 8);         
    ledcAttachPin(LED_PIN, 0);      
}

bool handleArcadeLogic() {
    bool reading = digitalRead(BUTTON_PIN);

    if (doubleTapPending && (millis() - doubleTapArmTime) > DOUBLE_TAP_WINDOW_MS) {
        Serial.println("[HARDWARE]: Double-tap confirmation timed out.");
        doubleTapPending = false;
        updateMetricsArea("Broadcast", "READY", TFT_BLUE, 3);
    }

    bool altReading = digitalRead(TEST_ONE_PIN);

    if (altReading != lastTestButtonState) {
        lastTestDebounceTime = millis();
    }

    if ((millis() - lastTestDebounceTime) > TEST_DEBOUNCE_DELAY) {
        if (altReading != currentTestButtonState) {
            currentTestButtonState = altReading;
            if (currentTestButtonState == LOW) {
                dimmed = false;
                setDimming(100);
                taskLEDBlink.disable(); 
                setButtonLED(0);
                clearScreen();
                isClockLayoutDrawn = false; 
                executeTestButtonAction();
                lastTestButtonState = altReading;
                return true;
            }
        }
    }

    bool testAllReading = digitalRead(TEST_ALL_PIN);

    if (testAllReading != lastTestAllButtonState) {
        lastTestAllDebounceTime = millis();
    }

    if ((millis() - lastTestAllDebounceTime) > TEST_ALL_DEBOUNCE_DELAY) {
        if (testAllReading != currentTestAllButtonState) {
            currentTestAllButtonState = testAllReading;
            if (currentTestAllButtonState == LOW) {
                dimmed = false;
                setDimming(100);
                taskLEDBlink.disable(); 
                setButtonLED(0);
                clearScreen();
                isClockLayoutDrawn = false; 
                executeTestAllButtonAction();
                lastTestAllButtonState = testAllReading;
                return true;
            }
        }
    }

    if (reading != lastButtonState) { lastDebounceTime = millis(); }
    if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
        if (reading != currentButtonState) {
            currentButtonState = reading;
            if (currentButtonState == LOW) {
                dimmed = false;
                setDimming(100);
                taskLEDBlink.disable(); 
                setButtonLED(255);
                clearScreen();
                isClockLayoutDrawn = false;
                onDebounceComplete();
                lastButtonState = reading; 
                return true;
            }
        }
    }
    lastButtonState = reading;
    lastTestButtonState = altReading;
    lastTestAllButtonState = testAllReading;
    return false;
}

void setButtonLED(int brightness) {
    ledcWrite(0, constrain(brightness, 0, 255)); 
}

void onDebounceComplete() {
    Serial.println("\n[HARDWARE]: Button press stabilized and validated.");

    if (!connected) {
        doubleTapPending = false;
        updateMetricsArea("Broadcast", "FAIL", TFT_RED, 3);
        return;
    }

    if (doubleTapPending && (millis() - doubleTapArmTime) <= DOUBLE_TAP_WINDOW_MS) {
        Serial.println("[HARDWARE]: Second tap confirmed. Executing action.");
        doubleTapPending = false;
        executeButtonAction();
    } else {
        doubleTapPending = true;
        doubleTapArmTime = millis();
        Serial.println("[HARDWARE]: First tap received. Waiting for second tap.");
        updateMetricsArea("Tap Again", "TO SEND", TFT_ORANGE, 2);
    }
}
