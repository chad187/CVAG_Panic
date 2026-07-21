// src/arcade_manager.h
#ifndef ARCADE_MANAGER_H
#define ARCADE_MANAGER_H

void initArcadeHardware();
bool handleArcadeLogic();
void setButtonLED(int brightness);
void onDebounceComplete();
void executeButtonAction();
void executeTestButtonAction();
void triggerLedPattern(unsigned long speedMs, int maxBrightness, int count);

#endif