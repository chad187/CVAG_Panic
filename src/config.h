// src/config.h
#ifndef CONFIG_H
#define CONFIG_H

#define CURRENT_VERSION "1.0.0"

// Global Time Server Parameters
#define NTP_SERVER "pool.ntp.org"
#define TZ_INFO "PST8PDT,M3.2.0,M11.1.0" // US Pacific Time

// Fallback if private_env.ini isn't loaded
#ifndef WIFI_PROFILES
    #define WIFI_PROFILES {"NO_WIFI_CONFIGURED", "NO_PASSWORD"}
#endif

extern bool connected;
extern bool dimmed;
extern bool isClockLayoutDrawn;

struct WifiCredential {
    const char* ssid;
    const char* password;
};

const WifiCredential myNetworks[] = {
    WIFI_PROFILES
};

const int NETWORK_COUNT = sizeof(myNetworks) / sizeof(myNetworks[0]);

// The raw URL pointing to your repository's version tracking manifest file
#define CONFIG_URL "https://raw.githubusercontent.com/YOUR_GITHUB_USERNAME/YOUR_REPO_NAME/main/version.json"

// The direct asset link where your compiled firmware binary will sit inside GitHub Releases
#define FIRMWARE_URL "https://github.com/YOUR_GITHUB_USERNAME/YOUR_REPO_NAME/releases/latest/download/firmware.bin"

#endif