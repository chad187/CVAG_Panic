// src/display_manager.cpp
#include <Arduino.h>       // <-- ADD THIS (Brings in TFT_BLACK, TFT_CYAN, etc.)
#include <time.h>          // <-- ADD THIS (Brings in getLocalTime and struct tm)
#include "display_manager.h"
#include "config.h"

TFT_eSPI tft = TFT_eSPI();

static bool networkHoldsDisplay = false;
static bool clockHasComeOnFlag = false;

static int glyphWidth(int textSize) {
    return 6 * textSize;
}

static String fitToWidth(const String& value, int textSize, int maxPixels) {
    int cell = glyphWidth(textSize);
    if (cell <= 0) {
        return value;
    }
    int maxChars = maxPixels / cell;
    if ((int)value.length() <= maxChars) {
        return value;
    }
    if (maxChars <= 1) {
        return value.substring(0, maxChars);
    }
    return value.substring(0, maxChars - 1) + "~";
}

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
    tft.setTextColor(color, TFT_BLACK);
    tft.setTextSize(1);
    tft.drawString(fitToWidth(status, 1, 220), 10, 32);
}

void updateMetricsArea(const char* label, String value, uint16_t color, int size) {
    const int contentTop = 54;
    const int contentBottom = 134;
    const int gap = 6;
    const int maxPixels = 220;

    int labelSize = constrain(size, 1, 3);
    int valueSize = labelSize + 1;
    while (valueSize > 1 && (int)value.length() * glyphWidth(valueSize) > maxPixels) {
        valueSize--;
    }

    String shownLabel = fitToWidth(label, labelSize, maxPixels);
    String shownValue = fitToWidth(value, valueSize, maxPixels);
    int labelH = 8 * labelSize;
    int valueH = 8 * valueSize;
    int blockH = labelH + gap + valueH;
    int available = contentBottom - contentTop;
    int y = contentTop;
    if (blockH < available) {
        y = contentTop + (available - blockH) / 2;
    }

    // Leave the header (y 0-25) and the status line (y 30-50) in place.
    tft.fillRect(0, 52, 240, 83, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(labelSize);
    tft.drawString(shownLabel, 10, y);

    tft.setTextColor(color, TFT_BLACK);
    tft.setTextSize(valueSize);
    tft.drawString(shownValue, 10, y + labelH + gap);
}

void updateLiveClock() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) {
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

void setNetworkDisplayHold(bool hold) {
    networkHoldsDisplay = hold;
}

bool networkDisplayHeld() {
    return networkHoldsDisplay;
}

void noteClockDisplayed() {
    clockHasComeOnFlag = true;
}

bool clockHasComeOn() {
    return clockHasComeOnFlag;
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