// src/network_manager.h
#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

void initialWIFI();
void syncClockWithNTP();

enum FirmwareCheckResult {
    FIRMWARE_CHECK_FAILED,
    FIRMWARE_UP_TO_DATE,
    FIRMWARE_UPDATE_AVAILABLE
};

FirmwareCheckResult checkForFirmwareUpdates();
void performOTAUpdate();
void checkLiveConnection();
void noteFirmwareBoot();
void confirmRunningFirmware();
void rollbackIfUnconfirmed();

#endif