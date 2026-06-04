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
#define _TASK_STATUS_REQUEST
#include <TaskSchedulerDeclarations.h>

// Pin Configuration
const int BUTTON_PIN = 26;  
const int LED_PIN = 27;
extern Task taskLEDBlink;

// Debounce Tracking Layout
bool lastButtonState = HIGH;      
bool currentButtonState = HIGH;   
unsigned long lastDebounceTime = 0;  
const unsigned long DEBOUNCE_DELAY = 50; 

// Connection Pooling Engine
#define MAX_ASYNC_REQUESTS 10
AsyncHTTPRequest requestPool[MAX_ASYNC_REQUESTS];
int currentPoolIndex = 0;

int pendingRequestsCount = 0;
bool dispatchHasFailed = false;

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
        
        if (responseCode < 200 || responseCode >= 300) {
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

    #if defined(SHEET_ID) && defined(SHEET_PASSWORD)
        WiFiClientSecure secureClient;
        secureClient.setInsecure(); 

        HTTPClient http;
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        // --- THE SECURITY CHANGE ---
        // Automatically appends '?password=xxxx' right onto the end of your Google Script URL
        String secureUrl = String(SHEET_ID) + "?password=" + String(SHEET_PASSWORD);

        if (http.begin(secureClient, secureUrl)) {
            int httpCode = http.GET();
            
            if (httpCode == 200) {
                String responseBody = http.getString();
                Serial.print("[CLOUD DATA]: Received Payload: "); Serial.println(responseBody);

                JsonDocument doc; 
                DeserializationError error = deserializeJson(doc, responseBody);

                if (!error) {
                    String status = doc["status"].as<String>();
                    
                    if (status == "unauthorized") {
                        updateMetricsArea("Auth Error", "BAD PASS", TFT_RED, 2);
                        triggerLedPattern(1000, 127, 2);
                        Serial.println("[CRITICAL]: Google Script rejected our password credential!");
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
        Serial.println("[ERROR]: SHEET_ID or SHEET_PASSWORD macro configuration missing.");
        updateMetricsArea("Broadcast", "FAIL", TFT_RED, 3);
    #endif

}

void initArcadeHardware() {
    pinMode(BUTTON_PIN, INPUT_PULLUP); 
    ledcSetup(0, 5000, 8);         
    ledcAttachPin(LED_PIN, 0);      
}

bool handleArcadeLogic() {
    bool reading = digitalRead(BUTTON_PIN);
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
    return false;
}

void setButtonLED(int brightness) {
    ledcWrite(0, constrain(brightness, 0, 255)); 
}

void onDebounceComplete() {
    Serial.println("\n[HARDWARE]: Button press stabilized and validated.");
    if (connected) {
        executeButtonAction();
    } 
    else { updateMetricsArea("Broadcast", "FAIL", TFT_RED, 3); }
}
