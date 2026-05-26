// src/arcade_manager.cpp
#include <Arduino.h>
#include "config.h"
#include "arcade_manager.h"
#include "network_manager.h" // For checking the "connected" flag
#include "display_manager.h"

// Pin Configuration (Adjust these to match your actual physical LilyGO wiring layout)
const int BUTTON_PIN = 35;  

// For your classic V1.1 board, the onboard screen backlight is driven by GPIO 4.
// If you want to flash the actual screen backlight as a status alert on button press,
// you can route it here! Otherwise, leave it as 33 for your future external LED layout.
const int LED_PIN = 33;
// Internal State Tracker Variables for Non-Blocking Debounce
bool lastButtonState = HIGH;      // Assumes Pull-up resistor wiring layout (HIGH = open)
bool currentButtonState = HIGH;   
unsigned long lastDebounceTime = 0;  
const unsigned long DEBOUNCE_DELAY = 50; // 50ms window to filter out mechanical noise

void initArcadeHardware() {
    // Correct mode for the hardware-pulled GPIO 35 right button
    pinMode(BUTTON_PIN, INPUT); //this might need to be INPUT_PULLUP with gpio pins
    
    // Configure hardware PWM properties for smooth LED fading control
    ledcSetup(0, 5000, 8);         
    ledcAttachPin(LED_PIN, 0);     
    
    setButtonLED(0); 
    Serial.println("Arcade Hardware Layer Initialized on GPIO 35.");
}

bool handleArcadeLogic() {
    bool reading = digitalRead(BUTTON_PIN);

    if (reading != lastButtonState) {
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
        if (reading != currentButtonState) {
            currentButtonState = reading;

            if (currentButtonState == LOW) {
                dimmed = false;
                setDimming(100);
                clearScreen();
                isClockLayoutDrawn = false;
                
                // 1. Remove the manual state overwrite from here!
                onDebounceComplete();
                
                // 2. Assign the tracker at the escape boundary so it's correct for the next loop
                lastButtonState = reading; 
                return true;
            }
        }
    }

    // This handles 99.9% of your cycles smoothly every 20ms
    lastButtonState = reading;
    return false;
}

void setButtonLED(int brightness) {
    // Constrain the incoming value cleanly between 0 (OFF) and 255 (Max Brightness)
    int safeBrightness = constrain(brightness, 0, 255);
    ledcWrite(0, safeBrightness); // Write PWM value to channel 0
}

void onDebounceComplete() {
    Serial.println("\n[HARDWARE]: Button press stabilized and validated.");
    
    // Safety Guard: Only fire if we actually have active WAN internet connectivity
    if (connected) {
        executeButtonAction();
    } else {
        Serial.println("[WARNING]: Button ignored. Device is currently OFFLINE.");
        updateMetricsArea("Broadcast", "FAIL", TFT_RED, 3);
        // Flash the button LED quickly as an error warning indicator
        setButtonLED(255); delay(100); setButtonLED(0); delay(100); setButtonLED(255); delay(100); setButtonLED(0);
    }
}

void executeButtonAction() {
    Serial.println("[ACTION]: Launching primary remote trigger script protocol...");
    updateMetricsArea("Broadcast", "SUCCESS", TFT_GREEN, 3);
    
    // Light up the button at full intensity while processing the execution event
    setButtonLED(255);
    
    // TODO: Put your Selenium script webhook target or target API dispatch calls here!
    
    // Dim the button back down once processing terminates successfully
    setButtonLED(30); 
}