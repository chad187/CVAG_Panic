// src/display_manager.cpp
#include <Arduino.h>       // <-- ADD THIS (Brings in TFT_BLACK, TFT_CYAN, etc.)
#include <time.h>          // <-- ADD THIS (Brings in getLocalTime and struct tm)
#include "display_manager.h"
#include "config.h"

TFT_eSPI tft = TFT_eSPI();

void initDisplay() {
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    
    // Backlight initialization
    ledcSetup(1, 10000, 8); 
    ledcAttachPin(4, 1);    
    ledcWrite(1, 255); // Full brightness
}

void drawHeader(const char* title, uint16_t bgColor) {
    tft.fillRect(0, 0, 240, 25, bgColor); // Dynamic background fill
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.drawString(title, 8, 4);
}

void updateStatusArea(const char* status, uint16_t color) {
    tft.fillRect(0, 30, 240, 20, TFT_BLACK);
    tft.setTextColor(color);
    tft.setTextSize(1);
    tft.drawString(status, 10, 32);
}

void updateMetricsArea(const char* label, String value, uint16_t color, int size) {
    tft.fillRect(0, 60, 240, 50, TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(size);
    tft.drawString(label, 10, 62);
    
    tft.setTextColor(color);
    tft.setTextSize(size+1);
    tft.drawString(value, 10, 80);
}

void updateLiveClock() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return; 
    }

    char timeString[9];  
    char dateString[12]; 

    strftime(timeString, sizeof(timeString), "%H:%M:%S", &timeinfo);
    strftime(dateString, sizeof(dateString), "%a, %b %d", &timeinfo);

    // Shifted UP to Y=45 (Fits perfectly below the 26px header divider)
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK); 
    tft.setTextSize(1);
    tft.drawCentreString(dateString, 120, 45, 2); 

    // Shifted UP to Y=70 (Perfect center positioning on a 135px tall screen)
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawCentreString(timeString, 120, 70, 4);
    drawMainClockLayout();
}

void clearScreen() {
    tft.fillScreen(TFT_BLACK);
}

void drawMainClockLayout() {
    // Replaces the old static text with a clean green "CONNECTED" banner
    if (connected) {
        drawHeader("CONNECTED v" CURRENT_VERSION, TFT_GREEN);
    }
    else {
        drawHeader("OFFLINE v" CURRENT_VERSION, TFT_RED);
    }
     
    
    // Draw a subtle horizontal dividing line right under your green header space
    tft.drawFastHLine(0, 26, 240, TFT_DARKGREY);
}

void setDimming(int percentage) {
  // Guard the input between 0 and 100
  percentage = constrain(percentage, 0, 100);
  
  // NORMAL LOGIC: 0% percentage = 0 PWM (Black), 100% percentage = 255 PWM (Bright)
  int dutyCycle = map(percentage, 0, 100, 0, 255);
  
  // INVERTED LOGIC: Un-comment this line if 0 makes your screen bright:
  // int dutyCycle = map(percentage, 0, 100, 255, 0); 
  
  ledcWrite(1, dutyCycle);
}