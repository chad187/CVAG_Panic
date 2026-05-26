// src/main.cpp
#include <Arduino.h>
#include <TaskScheduler.h>
#include "config.h"
#include "display_manager.h"
#include "network_manager.h"

Scheduler runner; 
bool isClockLayoutDrawn = false; 

// --- NEW STATE TRACKING FOR MAINTENANCE ---
bool hasRunDailyMaintenance = false;
bool connected = false;

void clockCallback(); 
void maintenanceCallback(); // Forward declaration for our maintenance runner
void connectionWatchdogCallback();

// Tasks: 
// 1. Clock runs every 1000ms forever
Task taskUpdateClock(1000, TASK_FOREVER, &clockCallback);
// 2. Maintenance runs every 5 minutees (300000ms) forever
Task taskMaintenance(300000, TASK_FOREVER, &maintenanceCallback);
Task taskNetworkWatchdog(10000, TASK_FOREVER, &connectionWatchdogCallback);

//3. check for a live internet connection

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
    Serial.println("[SCHEDULER]: Watchdog task triggered!");
    checkLiveConnection(); // Run the silent hardware validation check
}

void setup() {
    Serial.begin(115200);
    delay(500);
    
    initDisplay();
    drawHeader("STARTING UP...", TFT_DARKGREY); 
    
    initialWIFI();
    
    runner.init();
    
    // Register both tasks into the loop infrastructure engine
    runner.addTask(taskUpdateClock);
    runner.addTask(taskMaintenance);
    runner.addTask(taskNetworkWatchdog);
    
    // Start both tasks humming
    taskUpdateClock.enable();
    taskMaintenance.enable();
    taskNetworkWatchdog.enable();
    
    Serial.println("System initialized. Multi-tasking maintenance kernel active.");
}

void loop() {
    // Both tasks are tracked and executed asynchronously right here without interfering with each other
    runner.execute();
}