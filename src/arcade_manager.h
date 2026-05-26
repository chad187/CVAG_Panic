// src/arcade_manager.h
#ifndef ARCADE_MANAGER_H
#define ARCADE_MANAGER_H

void initArcadeHardware();
void handleArcadeLogic();
void setButtonLED(int brightness);
void onDebounceComplete();
void executeButtonAction();

#endif