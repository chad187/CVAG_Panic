#include <Arduino.h>
#include <string.h>
#include "config.h"
#include "credential_store.h"
#include "arcade_manager.h"
#include "network_manager.h" 
#include "display_manager.h"
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include "backend_ca.h"
#include <ArduinoJson.h>
#define _TASK_STATUS_REQUEST
#include <TaskSchedulerDeclarations.h>

// Pin Configuration
const int BUTTON_PIN = 26;  
const int TEST_ONE_PIN = 35;
const int TEST_ALL_PIN = 0;
const int LED_PIN = 27;
extern Task taskLEDBlink;

// Debounce Tracking Layout
const unsigned long DEBOUNCE_DELAY = 50;

static const int PANIC_QUEUE_LEN = 8;
static volatile unsigned long panicPressMs[PANIC_QUEUE_LEN];
static volatile uint8_t panicPressCount = 0;
static volatile unsigned long lastPanicEdgeMs = 0;
static portMUX_TYPE panicMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR onPanicButtonEdge() {
    unsigned long now = millis();
    if (now - lastPanicEdgeMs < DEBOUNCE_DELAY) {
        return;
    }
    lastPanicEdgeMs = now;
    portENTER_CRITICAL_ISR(&panicMux);
    if (panicPressCount < PANIC_QUEUE_LEN) {
        panicPressMs[panicPressCount++] = now;
    }
    portEXIT_CRITICAL_ISR(&panicMux);
}

static bool takePanicPress(unsigned long& when) {
    portENTER_CRITICAL(&panicMux);
    if (panicPressCount == 0) {
        portEXIT_CRITICAL(&panicMux);
        return false;
    }
    when = panicPressMs[0];
    for (uint8_t i = 1; i < panicPressCount; i++) {
        panicPressMs[i - 1] = panicPressMs[i];
    }
    panicPressCount--;
    portEXIT_CRITICAL(&panicMux);
    return true;
} 

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

// ============================================================================
// MAIN EXECUTION LOGIC
// ============================================================================
struct AlertView {
    const char* action;
    const char* header;
    const char* busyLabel;
    const char* busyValue;
    const char* resultLabel;
    const char* successValue;
    uint16_t busyColor;
    int busySize;
    int resultSize;
    unsigned long successSpeedMs;
    int successBrightness;
    int successCount;
};

static const unsigned long ALERT_IO_TIMEOUT_MS = 3000;
static const int ALERT_ATTEMPTS = 2;

struct WdtPause {
    bool paused;
    WdtPause() : paused(esp_task_wdt_delete(NULL) == ESP_OK) {}
    ~WdtPause() {
        if (paused) {
            esp_task_wdt_add(NULL);
        }
    }
};

static void readHttpResponse(WiFiClientSecure& client, int& httpCode, String& responseBody) {
    unsigned long started = millis();
    bool headersPassed = false;
    httpCode = 0;
    responseBody = "";

    while (millis() - started < ALERT_IO_TIMEOUT_MS) {
        while (client.available()) {
            String line = client.readStringUntil('\n');
            line.trim();
            if (!headersPassed) {
                if (line.startsWith("HTTP/1.")) {
                    int spaceIdx = line.indexOf(' ');
                    if (spaceIdx != -1) {
                        httpCode = line.substring(spaceIdx + 1, spaceIdx + 4).toInt();
                    }
                }
                if (line.length() == 0) {
                    headersPassed = true;
                }
            } else if (responseBody.length() < 512) {
                responseBody += line;
            }
        }
        if (!client.connected() && !client.available()) {
            break;
        }
        delay(10);
    }

    while (client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (!headersPassed) {
            if (line.startsWith("HTTP/1.")) {
                int spaceIdx = line.indexOf(' ');
                if (spaceIdx != -1) {
                    httpCode = line.substring(spaceIdx + 1, spaceIdx + 4).toInt();
                }
            }
            if (line.length() == 0) {
                headersPassed = true;
            }
        } else if (responseBody.length() < 512) {
            responseBody += line;
        }
    }
}

static bool applyAlertResponse(const AlertView& view, int httpCode, const String& responseBody) {
    if (httpCode >= 200 && httpCode < 300) {
        updateMetricsArea(view.resultLabel, view.successValue, TFT_GREEN, view.resultSize);
        triggerLedPattern(view.successSpeedMs, view.successBrightness, view.successCount);
        Serial.println("[SUCCESS]: Alert request accepted.");
        return true;
    }

    if (httpCode != 429 && httpCode != 401 && httpCode != 400) {
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, responseBody);
    if (!error) {
        String status = doc["status"].as<String>();
        if (status == "unauthorized" || httpCode == 401) {
            updateMetricsArea("Auth Error", "BAD PASS", TFT_RED, 2);
            triggerLedPattern(1000, 127, 2);
            Serial.println("[CRITICAL]: Backend rejected our password credential!");
        } else if (httpCode == 429) {
            int secondsLeft = doc["retry_after"] | 0;
            String rateLimitMessage = "Wait " + String((secondsLeft + 59) / 60) + "m";
            triggerLedPattern(1000, 127, 3);
            updateMetricsArea("Rate Limit", rateLimitMessage.c_str(), TFT_ORANGE, 2);
        } else {
            updateMetricsArea(view.resultLabel, "FAIL", TFT_RED, view.resultSize);
            triggerLedPattern(1000, 127, 5);
            Serial.println("[FAILURE]: Alert request was rejected.");
        }
    } else {
        updateMetricsArea(view.resultLabel, "FAIL", TFT_RED, view.resultSize);
        triggerLedPattern(1000, 127, 6);
        Serial.print("[ERROR]: Failed to parse JSON response. Error code: ");
        Serial.println(error.c_str());
    }
    return true;
}

static void postAlert(const AlertView& view) {
    Serial.println("\n==================================================");
    Serial.print("[ACTION]: ");
    Serial.println(view.action);
    Serial.println("==================================================");

    drawHeader(view.header, TFT_DARKGREY);
    updateMetricsArea(view.busyLabel, view.busyValue, view.busyColor, view.busySize);

#if defined(BACKEND_BASE_URL)
    String baseUrl = String(BACKEND_BASE_URL);
    int hostStart = baseUrl.indexOf("://") + 3;
    int hostEnd = baseUrl.indexOf('/', hostStart);
    if (hostStart < 3 || hostEnd <= hostStart) {
        updateMetricsArea(view.resultLabel, "FAIL", TFT_RED, view.resultSize);
        triggerLedPattern(3000, 127, 2);
        Serial.println("[ERROR]: BACKEND_BASE_URL has no host path.");
        return;
    }

    String host = baseUrl.substring(hostStart, hostEnd);
    String path = baseUrl.substring(hostEnd) + "/trigger/";
    String jsonPayload = String("{\"action\":\"") + view.action + "\"}";
    bool accepted = false;

    {
        WdtPause pauseWdt;
        for (int attempt = 0; attempt < ALERT_ATTEMPTS && !accepted; attempt++) {
            if (attempt > 0) {
                updateMetricsArea(view.busyLabel, "RETRY", TFT_YELLOW, view.busySize);
            }

            WiFiClientSecure secureClient;
            secureClient.setCACert(BACKEND_ROOT_CA);
            secureClient.setHandshakeTimeout(3);
            secureClient.setTimeout(3);

            Serial.print("[CLOUD DATA]: Connecting to ");
            Serial.print(host);
            Serial.println(":443...");

            if (!secureClient.connect(host.c_str(), 443, (int32_t)ALERT_IO_TIMEOUT_MS)) {
                Serial.println("[ERROR]: Direct socket connection to server failed!");
                secureClient.stop();
                continue;
            }

            secureClient.print("POST " + path + " HTTP/1.1\r\n");
            secureClient.print("Host: " + host + "\r\n");
            secureClient.print("Connection: close\r\n");
            secureClient.print("Content-Type: application/json\r\n");
            const char* apiPassword = credentialApiPassword();
            if (apiPassword[0] == '\0') {
                Serial.println("[ERROR]: No API password is stored.");
                secureClient.stop();
                continue;
            }
            secureClient.print("Authorization: Bearer " + String(apiPassword) + "\r\n");
            secureClient.print("Content-Length: " + String(jsonPayload.length()) + "\r\n\r\n");
            secureClient.print(jsonPayload);

            int httpCode = 0;
            String responseBody;
            readHttpResponse(secureClient, httpCode, responseBody);
            secureClient.stop();

            Serial.print("[RESPONSE]: HTTP Code: ");
            Serial.println(httpCode);
            if (responseBody.length() > 0) {
                Serial.print("[CLOUD DATA]: Received Payload: ");
                Serial.println(responseBody);
            }

            if (applyAlertResponse(view, httpCode, responseBody)) {
                accepted = true;
            }
        }
    }

    if (!accepted) {
        updateMetricsArea(view.resultLabel, "FAIL", TFT_RED, view.resultSize);
        triggerLedPattern(3000, 127, 2);
    }
#else
    Serial.println("[ERROR]: BACKEND_BASE_URL is missing.");
    updateMetricsArea(view.resultLabel, "FAIL", TFT_RED, view.resultSize);
#endif
}

static const AlertView BROADCAST_ALERT = {
    "broadcast", "BROADCAST", "Syncing...", "FETCHING", "Broadcast", "SUCCESS",
    TFT_YELLOW, 2, 3, 250, 255, 4
};

static const AlertView TEST_ONE_ALERT = {
    "test_one", "TEST ONE", "Testing...", "TEST", "Test", "OK",
    TFT_BLUE, 2, 3, 1000, 127, 6
};

static const AlertView TEST_ALL_ALERT = {
    "test_all", "TEST ALL", "Testing...", "TEST ALL", "Test All", "SUCCESS",
    TFT_BLUE, 2, 3, 250, 255, 4
};

void executeButtonAction() {
    postAlert(BROADCAST_ALERT);
}

void executeTestButtonAction() {
    postAlert(TEST_ONE_ALERT);
}

void executeTestAllButtonAction() {
    postAlert(TEST_ALL_ALERT);
}


void initArcadeHardware() {
    pinMode(BUTTON_PIN, INPUT_PULLUP); 
    pinMode(TEST_ONE_PIN, INPUT_PULLUP);
    pinMode(TEST_ALL_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onPanicButtonEdge, FALLING);
    ledcSetup(0, 5000, 8);         
    ledcAttachPin(LED_PIN, 0);      
}

bool handleArcadeLogic() {
    if (!clockHasComeOn()) {
        unsigned long ignoredPress = 0;
        while (takePanicPress(ignoredPress)) {
        }
        bool testOneLevel = digitalRead(TEST_ONE_PIN);
        bool testAllLevel = digitalRead(TEST_ALL_PIN);
        lastTestButtonState = testOneLevel;
        currentTestButtonState = testOneLevel;
        lastTestAllButtonState = testAllLevel;
        currentTestAllButtonState = testAllLevel;
        doubleTapPending = false;
        return false;
    }

    if (doubleTapPending && (millis() - doubleTapArmTime) > DOUBLE_TAP_WINDOW_MS) {
        Serial.println("[HARDWARE]: Double-tap confirmation timed out.");
        doubleTapPending = false;
        drawHeader("STANDBY", TFT_BLUE);
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

    lastTestButtonState = altReading;
    lastTestAllButtonState = testAllReading;

    unsigned long queuedPress = 0;
    if (takePanicPress(queuedPress)) {
        (void)queuedPress;
        dimmed = false;
        setDimming(100);
        taskLEDBlink.disable();
        setButtonLED(255);
        clearScreen();
        isClockLayoutDrawn = false;
        onDebounceComplete();
        return true;
    }
    return false;
}

void setButtonLED(int brightness) {
    ledcWrite(0, constrain(brightness, 0, 255)); 
}

void onDebounceComplete() {
    Serial.println("\n[HARDWARE]: Button press stabilized and validated.");

    if (doubleTapPending && (millis() - doubleTapArmTime) <= DOUBLE_TAP_WINDOW_MS) {
        Serial.println("[HARDWARE]: Second tap confirmed. Executing action.");
        doubleTapPending = false;
        executeButtonAction();
    } else {
        doubleTapPending = true;
        doubleTapArmTime = millis();
        Serial.println("[HARDWARE]: First tap received. Waiting for second tap.");
        drawHeader("CONFIRM", TFT_ORANGE);
        updateMetricsArea("Tap Again", "TO SEND", TFT_ORANGE, 2);
    }
}
