// src/network_manager.h
#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

void initialWIFI();
void syncClockWithNTP();
bool checkForFirmwareUpdates();
void performOTAUpdate();
void checkLiveConnection();

#endif