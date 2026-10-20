#include "credential_store.h"
#include "config.h"
#include <Preferences.h>
#include <string.h>

struct WifiCredentialSlot {
    char ssid[33];
    char password[65];
};

static WifiCredentialSlot slots[MAX_WIFI_PROFILES];
static int slotCount = 0;
static char apiPassword[128] = "";
static int storedRevision = 0;

static void copyText(char* dest, size_t destLen, const char* src) {
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, src, destLen - 1);
    dest[destLen - 1] = '\0';
}

void loadCredentials() {
    Preferences prefs;
    prefs.begin("panic", true);
    slotCount = prefs.getInt("wifi_n", 0);
    if (slotCount < 0) {
        slotCount = 0;
    }
    if (slotCount > MAX_WIFI_PROFILES) {
        slotCount = MAX_WIFI_PROFILES;
    }
    for (int i = 0; i < slotCount; i++) {
        String ssidKey = "ssid" + String(i);
        String pskKey = "psk" + String(i);
        copyText(slots[i].ssid, sizeof(slots[i].ssid), prefs.getString(ssidKey.c_str(), "").c_str());
        copyText(slots[i].password, sizeof(slots[i].password), prefs.getString(pskKey.c_str(), "").c_str());
    }
    copyText(apiPassword, sizeof(apiPassword), prefs.getString("api_pw", "").c_str());
    storedRevision = prefs.getInt("rev", 0);
    prefs.end();
}

void seedCredentialsFromBuild() {
    if (slotCount > 0 || apiPassword[0] != '\0') {
        return;
    }
    if (NETWORK_COUNT <= 0 && strlen(API_PASSWORD) == 0) {
        return;
    }

    int count = NETWORK_COUNT;
    if (count > MAX_WIFI_PROFILES) {
        count = MAX_WIFI_PROFILES;
    }
    char ssids[MAX_WIFI_PROFILES][33];
    char passwords[MAX_WIFI_PROFILES][65];
    for (int i = 0; i < count; i++) {
        copyText(ssids[i], sizeof(ssids[i]), myNetworks[i].ssid);
        copyText(passwords[i], sizeof(passwords[i]), myNetworks[i].password);
    }
    saveCredentials(API_PASSWORD, count, ssids, passwords, 0);
    Serial.printf("[CREDENTIALS]: Seeded NVS with %d Wi-Fi profile(s).\n", count);
}

int credentialCount() {
    return slotCount;
}

const char* credentialSsid(int index) {
    if (index < 0 || index >= slotCount) {
        return "";
    }
    return slots[index].ssid;
}

const char* credentialPassword(int index) {
    if (index < 0 || index >= slotCount) {
        return "";
    }
    return slots[index].password;
}

const char* credentialApiPassword() {
    if (apiPassword[0] != '\0') {
        return apiPassword;
    }
    return API_PASSWORD;
}

int credentialRevision() {
    return storedRevision;
}

bool credentialsDiffer(const char* nextPassword, int wifiCount, const char ssids[][33], const char passwords[][65]) {
    if (nextPassword != nullptr && strcmp(nextPassword, credentialApiPassword()) != 0) {
        return true;
    }
    if (wifiCount != slotCount) {
        return true;
    }
    for (int i = 0; i < wifiCount; i++) {
        if (strcmp(ssids[i], slots[i].ssid) != 0 || strcmp(passwords[i], slots[i].password) != 0) {
            return true;
        }
    }
    return false;
}

void saveCredentials(const char* nextPassword, int wifiCount, const char ssids[][33], const char passwords[][65], int revision) {
    if (wifiCount < 0) {
        wifiCount = 0;
    }
    if (wifiCount > MAX_WIFI_PROFILES) {
        wifiCount = MAX_WIFI_PROFILES;
    }

    Preferences prefs;
    prefs.begin("panic", false);
    prefs.putInt("wifi_n", wifiCount);
    for (int i = 0; i < MAX_WIFI_PROFILES; i++) {
        String ssidKey = "ssid" + String(i);
        String pskKey = "psk" + String(i);
        if (i < wifiCount) {
            copyText(slots[i].ssid, sizeof(slots[i].ssid), ssids[i]);
            copyText(slots[i].password, sizeof(slots[i].password), passwords[i]);
            prefs.putString(ssidKey.c_str(), slots[i].ssid);
            prefs.putString(pskKey.c_str(), slots[i].password);
        } else {
            slots[i].ssid[0] = '\0';
            slots[i].password[0] = '\0';
            prefs.remove(ssidKey.c_str());
            prefs.remove(pskKey.c_str());
        }
    }
    slotCount = wifiCount;
    if (nextPassword != nullptr && nextPassword != apiPassword) {
        copyText(apiPassword, sizeof(apiPassword), nextPassword);
    }
    if (nextPassword != nullptr) {
        prefs.putString("api_pw", apiPassword);
    }
    storedRevision = revision;
    prefs.putInt("rev", revision);
    prefs.end();
}
