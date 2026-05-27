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

// Pin Configuration
const int BUTTON_PIN = 35;  
const int LED_PIN = 33;

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
// PART 1: RESEND EMAIL DISPATCHER
// ============================================================================
int sendResendEmail(const char* recipientList) {
    int successfulSends = 0; // Track successful dispatches

    #ifdef RESEND_API_KEY
        static char emailBuffer[256];
        static char payloadBuffer[256];
        static char authHeader[128];
        
        strncpy(emailBuffer, recipientList, sizeof(emailBuffer) - 1);
        emailBuffer[sizeof(emailBuffer) - 1] = '\0';
        
        char* emailContext = NULL;
        char* targetEmail = strtok_r(emailBuffer, ",", &emailContext);
        
        Serial.println("[QUEUE]: Initializing Resend Email Dispatch Loop...");
        
        WiFiClientSecure secureClient;
        secureClient.setInsecure(); 

        while (targetEmail != NULL) {
            while (*targetEmail == ' ') targetEmail++; 
            
            if (strlen(targetEmail) > 0) {
                snprintf(payloadBuffer, sizeof(payloadBuffer), 
                         "{\"from\":\"CVAGLookout@resend.dev\",\"to\":[\"%s\"],\"subject\":\"ALERT!\",\"text\":\"AN INSPECTOR HAS BEEN SIGHTED! \"}", 
                         targetEmail);
                
                snprintf(authHeader, sizeof(authHeader), "Bearer %s", RESEND_API_KEY);
                
                Serial.print("\n[CORE-NET]: Targeting destination: "); Serial.println(targetEmail);
                
                HTTPClient http;
                if (http.begin(secureClient, "https://api.resend.com/emails")) {
                    http.addHeader("Content-Type", "application/json");
                    http.addHeader("Authorization", authHeader);
                    
                    Serial.println("[CORE-NET]: Sending secure POST payload...");
                    int httpResponseCode = http.POST(payloadBuffer);
                    
                    Serial.print("[SERVER RESPONSE]: HTTP Code: "); Serial.println(httpResponseCode);
                    
                    String responseBody = http.getString();
                    if (responseBody.length() > 0) {
                        Serial.print("[SERVER REPLY]: "); Serial.println(responseBody);
                    }
                    
                    if (httpResponseCode >= 200 && httpResponseCode < 300) {
                        Serial.println("[SUCCESS]: Email successfully delivered to Resend gateway.");
                        successfulSends++; // Increment on real 2xx success codes
                    } else {
                        Serial.println("[ERROR]: Resend rejected this payload configuration.");
                    }
                    
                    http.end();
                } else {
                    Serial.println("[CRITICAL]: Core HTTP architecture could not initialize socket.");
                }
            }
            targetEmail = strtok_r(NULL, ",", &emailContext); 
        }
    #else
        Serial.println("[ERROR]: Resend deployment skipped. RESEND_API_KEY macro is missing.");
    #endif

    return successfulSends; // Send the total count back to the hardware action layer
}

// ============================================================================
// PART 2: TWILIO SMS DISPATCHER (Commented out for testing)
// ============================================================================
int sendTwilioSMS(const char* phoneList) {
    int successfulSends = 0;

    // Verify your configuration macros exist before running
    #if defined(TWILIO_SID) && defined(TWILIO_TOKEN)
        static char phoneBuffer[256];
        static char smsPayloadBuffer[384]; // Room for numbers + message string
        
        strncpy(phoneBuffer, phoneList, sizeof(phoneBuffer) - 1);
        phoneBuffer[sizeof(phoneBuffer) - 1] = '\0';
        
        char* phoneContext = NULL;
        char* targetPhone = strtok_r(phoneBuffer, ",", &phoneContext);
        
        Serial.println("[QUEUE]: Initializing Twilio SMS Dispatch Loop...");
        
        // Build the basic auth credential string programmatically: Basic Base64(SID:TOKEN)
        String rawAuthCredentials = String(TWILIO_SID) + ":" + String(TWILIO_TOKEN);
        String encodedAuth = base64::encode(rawAuthCredentials);
        String authHeader = "Basic " + encodedAuth;

        // Build the precise endpoint path using your Account SID
        String twilioEndpoint = "https://api.twilio.com/2010-04-01/Accounts/" + String(TWILIO_SID) + "/Messages.json";

        WiFiClientSecure secureClient;
        secureClient.setInsecure(); // Skip root certificate checking to keep execution fast

        while (targetPhone != NULL) {
            while (*targetPhone == ' ') targetPhone++; // Clean spaces
            
            if (strlen(targetPhone) > 0) {
                // IMPORTANT: Replace +15551112222 with your active Twilio phone number!
                // We use '+' inside url-encoding as spaces between words to keep it simple without a heavy encoder library
                snprintf(smsPayloadBuffer, sizeof(smsPayloadBuffer), 
                    "To=%s&From=%s&Body=An+inspector+has+been+sighted!", 
                    targetPhone, TWILIO_NUMBER);
                
                Serial.print("\n[CORE-NET]: Targeting SMS destination: "); Serial.println(targetPhone);
                
                HTTPClient http;
                if (http.begin(secureClient, twilioEndpoint)) {
                    // Twilio demands URL Form Encoding content type declarations
                    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
                    http.addHeader("Authorization", authHeader.c_str());
                    
                    Serial.println("[CORE-NET]: Pushing outbound urlencoded form payload...");
                    int httpResponseCode = http.POST(smsPayloadBuffer);
                    
                    Serial.print("[SERVER RESPONSE]: Twilio HTTP Code: "); Serial.println(httpResponseCode);
                    
                    String responseBody = http.getString();
                    if (responseBody.length() > 0) {
                        Serial.print("[SERVER REPLY]: "); Serial.println(responseBody);
                    }
                    
                    if (httpResponseCode >= 200 && httpResponseCode < 300) {
                        Serial.println("[SUCCESS]: SMS payload accepted by Twilio gate cellular networks.");
                        successfulSends++;
                    } else {
                        Serial.println("[ERROR]: Twilio gateway rejected this transmission request.");
                    }
                    
                    http.end();
                } else {
                    Serial.println("[CRITICAL]: Unable to allocate internal HTTP structure socket context.");
                }
            }
            targetPhone = strtok_r(NULL, ",", &phoneContext); 
        }
    #else
        Serial.println("[ERROR]: Twilio deployment skipped. TWILIO_SID or TWILIO_TOKEN macros are missing.");
    #endif

    return successfulSends;
}

// ============================================================================
// MAIN EXECUTION LOGIC
// ============================================================================
void executeButtonAction() {
    Serial.println("\n==================================================");
    Serial.println("[ACTION]: Launching primary remote trigger script protocol...");
    Serial.print("[WIFI STATUS]: Checking radio connection profile... ");
    Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
    Serial.println("==================================================");
    
    setButtonLED(255);
    int totalSuccessfulEmails = 0;
    int totalSuccessfulTexts = 0;

    // 1. Process Emails
    #ifdef EMAILS
        totalSuccessfulEmails = sendResendEmail(EMAILS);
    #endif

    // 2. Process SMS Texts
    #ifdef PHONES
        totalSuccessfulTexts = sendTwilioSMS(PHONES);
    #endif

    // UI SCREEN EVALUATION RULE: Paint GREEN if any alert style was successfully dispatched
    if (totalSuccessfulEmails > 0 || totalSuccessfulTexts > 0) {
        Serial.println("\n[STATUS]: Alert dispatch verified. At least one vector cleared.");
        updateMetricsArea("Broadcast", "SUCCESS", TFT_GREEN, 3);
    } else {
        Serial.println("\n[STATUS]: Comprehensive alert failure. Zero communication packets escaped.");
        updateMetricsArea("Broadcast", "FAIL", TFT_RED, 3);
    }

    setButtonLED(30); 
    Serial.println("\n[SUCCESS]: Trigger routine complete. Core loop active.");
}

void initArcadeHardware() {
    pinMode(BUTTON_PIN, INPUT); 
    ledcSetup(0, 5000, 8);         
    ledcAttachPin(LED_PIN, 0);     
    setButtonLED(0); 
}

bool handleArcadeLogic() {
    bool reading = digitalRead(BUTTON_PIN);
    if (reading != lastButtonState) { lastDebounceTime = millis(); }
    if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
        if (reading != currentButtonState) {
            currentButtonState = reading;
            if (currentButtonState == LOW) {
                extern bool dimmed; dimmed = false;
                extern void setDimming(int percentage); setDimming(100);
                clearScreen();
                extern bool isClockLayoutDrawn; isClockLayoutDrawn = false;
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
    if (connected) { executeButtonAction(); } 
    else { updateMetricsArea("Broadcast", "FAIL", TFT_RED, 3); }
}