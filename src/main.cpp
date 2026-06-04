// src/main.cpp
#include <Arduino.h>
#include <TaskScheduler.h>
#include "config.h"
#include "display_manager.h"
#include "network_manager.h"
#include "arcade_manager.h"

Scheduler runner; 
bool isClockLayoutDrawn = false; 

// --- NEW STATE TRACKING FOR MAINTENANCE ---
bool hasRunDailyMaintenance = false;
bool connected = false;
bool dimmed = false;

void clockCallback(); 
void maintenanceCallback();
void connectionWatchdogCallback();
void dimmingCallback();
void arcadeLogicCallback();
void ledBlinkCallback();

// Tasks: 
// 1. Clock runs every 1000ms forever
Task taskUpdateClock(1000, TASK_FOREVER, &clockCallback);
// 2. Maintenance runs every 5 minutees (300000ms) forever
Task taskMaintenance(300000, TASK_FOREVER, &maintenanceCallback);
Task taskNetworkWatchdog(10000, TASK_FOREVER, &connectionWatchdogCallback);
Task taskDimming(0, TASK_ONCE, &dimmingCallback);
Task taskArcadeWatcher(20, TASK_FOREVER, &arcadeLogicCallback);
Task taskLEDBlink(20, TASK_FOREVER, &ledBlinkCallback);

void dimmingCallback() {
    Serial.println("System entered deep idle. Dimming display and starting LED pulse.");
    setDimming(7);
    
    // Start the high-frequency breathing task now that we are idle
    taskLEDBlink.enable();
}

void arcadeLogicCallback() {
    if(handleArcadeLogic()) {
        // --- WAKE UP SEQUENCE ---
        // Stop the background blinking immediately so it doesn't blink during dispatches
        
        // Handle your clock layout delays
        taskUpdateClock.disable();
        taskUpdateClock.restartDelayed(10000);
    }
}

void ledBlinkCallback() {
    const unsigned long intervalMs = 2000; // Total time for one full pulse wave cycle
    const int maxBrightness = 180;          // Maximum peak brightness limit
    
    unsigned long currentMillis = millis();

    // Calculate our wave position using the current millisecond timeline
    float angle = (float)(currentMillis % intervalMs) / (float)intervalMs * 2.0 * PI;
    float waveIntensity = (sin(angle) + 1.0) / 2.0;

    int dynamicBrightness = (int)(waveIntensity * maxBrightness);
    setButtonLED(dynamicBrightness);
}

void clockCallback() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return; 
    }

    if (!isClockLayoutDrawn) {
        clearScreen();
        drawMainClockLayout(); 
        isClockLayoutDrawn = true;
    }

    updateLiveClock();
}

// This runs automatically in the background every 5 minutes
void maintenanceCallback() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return; 
    }

    if (timeinfo.tm_hour == 0 && !hasRunDailyMaintenance) {
        Serial.println("Midnight window detected. Starting daily system maintenance...");

        if (!connected) {
            Serial.println("System offline at midnight. Forcing a hard network stack rebuild...");
            tryWiFiConnection(); // Force a hard, blocking 10-second re-scan and reconnection attempt
        } else {
            syncClockWithNTP();
            bool updateAvailable = checkForFirmwareUpdates();
        
            if (updateAvailable) {
                // If a mismatch is discovered, completely stop all scheduler clocks 
                // and initiate the critical chip flashing sequence immediately.
                taskUpdateClock.disable();
                taskMaintenance.disable();
                
                performOTAUpdate(); 
            }
        }
        
        hasRunDailyMaintenance = true; 
    }

    if (timeinfo.tm_hour > 0 && hasRunDailyMaintenance) {
        hasRunDailyMaintenance = false;
        Serial.println("System maintenance lock reset. Armed for next midnight window.");
    }

}

void connectionWatchdogCallback() {
    checkLiveConnection(); // Run the silent hardware validation check
}

void setup() {
    Serial.begin(115200);
    delay(500);
    
    initDisplay();
    initArcadeHardware();
    drawHeader("STARTING UP...", TFT_DARKGREY); 
    
    initialWIFI();
    
    runner.init();
    
    // Register both tasks into the loop infrastructure engine
    runner.addTask(taskUpdateClock);
    runner.addTask(taskMaintenance);
    runner.addTask(taskNetworkWatchdog);
    runner.addTask(taskDimming);
    runner.addTask(taskArcadeWatcher);
    runner.addTask(taskLEDBlink);
    
    // Start both tasks humming
    taskUpdateClock.enable();
    taskMaintenance.enable();
    taskNetworkWatchdog.enable();
    taskArcadeWatcher.enable();
    
    Serial.println("System initialized. Multi-tasking maintenance kernel active.");
}

void loop() {
    // Both tasks are tracked and executed asynchronously right here without interfering with each other
    runner.execute();
    if (!dimmed) {
        dimmed = true;
        taskDimming.restartDelayed(10000);
    }
}