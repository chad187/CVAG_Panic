// src/config.h
#ifndef CONFIG_H
#define CONFIG_H

// Set from version.json by scripts/inject_version.py. That file is the only version to bump.
#ifndef CURRENT_VERSION
#error "CURRENT_VERSION is missing. Build with PlatformIO so version.json is compiled in."
#endif

#ifndef BACKEND_BASE_URL
    #define BACKEND_BASE_URL "https://cvag.rocks/alerts/api"
#endif

#ifndef API_PASSWORD
    #define API_PASSWORD ""
#endif

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
#define CONFIG_URL "https://raw.githubusercontent.com/chad187/CVAG_Panic/release/version.json"

#endif