#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <TFT_eSPI.h>

extern TFT_eSPI tft;

void initDisplay();
// --- UPDATE THIS TO ACCEPT TEXT AND COLOR BACKGROUND VARIABLES ---
void drawHeader(const char* title, uint16_t bgColor); 

void updateStatusArea(const char* status, uint16_t color);
void updateMetricsArea(const char* label, String value, uint16_t color, int size);
void updateLiveClock();
void clearScreen();
void drawMainClockLayout();
void setDimming(int percentage);

#endif