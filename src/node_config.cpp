#include "node_config.h"
#include "backend_ca.h"
#include "config.h"
#include "credential_store.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include "backend_target.h"
#include <esp_task_wdt.h>

static unsigned long lastConfigPullMs = 0;
static const unsigned long CONFIG_INTERVAL_MS = 3600000;
static const int CONFIG_TIMEOUT_MS = 8000;

struct WdtPause {
    bool paused;
    WdtPause() : paused(esp_task_wdt_delete(NULL) == ESP_OK) {}
    ~WdtPause() {
        if (paused) {
            esp_task_wdt_add(NULL);
        }
    }
};

static bool applyPayload(JsonDocument& doc) {
    const char* nextPassword = doc["api_password"] | "";
    JsonArray wifi = doc["wifi"].as<JsonArray>();
    int revision = doc["revision"] | credentialRevision();

    char ssids[MAX_WIFI_PROFILES][33];
    char passwords[MAX_WIFI_PROFILES][65];
    int count = 0;
    if (!wifi.isNull()) {
        for (JsonObject profile : wifi) {
            if (count >= MAX_WIFI_PROFILES) {
                break;
            }
            const char* ssid = profile["ssid"] | "";
            const char* password = profile["password"] | "";
            if (ssid[0] == '\0') {
                continue;
            }
            strncpy(ssids[count], ssid, sizeof(ssids[count]) - 1);
            ssids[count][sizeof(ssids[count]) - 1] = '\0';
            strncpy(passwords[count], password, sizeof(passwords[count]) - 1);
            passwords[count][sizeof(passwords[count]) - 1] = '\0';
            count++;
        }
    }

    bool passwordChanged = nextPassword[0] != '\0' && strcmp(nextPassword, credentialApiPassword()) != 0;
    bool wifiChanged = count > 0 && credentialsDiffer(credentialApiPassword(), count, ssids, passwords);
    bool revisionChanged = revision != credentialRevision();
    if (!passwordChanged && !wifiChanged && !revisionChanged) {
        return false;
    }

    const char* passwordToStore = passwordChanged ? nextPassword : credentialApiPassword();
    if (count == 0) {
        count = credentialCount();
        for (int i = 0; i < count; i++) {
            strncpy(ssids[i], credentialSsid(i), sizeof(ssids[i]) - 1);
            ssids[i][sizeof(ssids[i]) - 1] = '\0';
            strncpy(passwords[i], credentialPassword(i), sizeof(passwords[i]) - 1);
            passwords[i][sizeof(passwords[i]) - 1] = '\0';
        }
    }
    saveCredentials(passwordToStore, count, ssids, passwords, revision);
    if (passwordChanged) {
        Serial.println("[CONFIG]: API password updated.");
    }
    return true;
}

static void logStoredWifi() {
    Serial.printf("[CONFIG]: Stored Wi-Fi profiles: %d\n", credentialCount());
    for (int i = 0; i < credentialCount(); i++) {
        Serial.printf("[CONFIG]:  %d %s\n", i + 1, credentialSsid(i));
    }
}

void pullNodeConfig() {
    lastConfigPullMs = millis();
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    const char* token = credentialApiPassword();
    if (token[0] == '\0') {
        Serial.println("[CONFIG]: No API password stored. Skipping credential check.");
        return;
    }

    BackendTarget target;
    if (!parseBackendTarget("/node/config/", target)) {
        Serial.println("[CONFIG]: BACKEND_BASE_URL has no host path.");
        return;
    }

    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    Client* client = &plainClient;
    if (target.tls) {
        secureClient.setCACert(BACKEND_ROOT_CA);
        secureClient.setHandshakeTimeout(8);
        secureClient.setTimeout(8);
        client = &secureClient;
    }

    bool connectedToHost = false;
    {
        WdtPause pauseWdt;
        connectedToHost = target.tls
            ? secureClient.connect(target.host.c_str(), target.port, CONFIG_TIMEOUT_MS)
            : plainClient.connect(target.host.c_str(), target.port, CONFIG_TIMEOUT_MS);
    }
    if (!connectedToHost) {
        Serial.printf("[CONFIG]: Credential endpoint connection failed (%s:%u).\n", target.host.c_str(), target.port);
        client->stop();
        return;
    }

    String revision = String(credentialRevision());
    String hostHeader = backendHostHeader(target);
    client->print("GET " + target.path + " HTTP/1.1\r\n");
    client->print("Host: " + hostHeader + "\r\n");
    client->print("Connection: close\r\n");
    client->print("Authorization: Bearer " + String(token) + "\r\n");
    client->print("X-Node-Id: " + WiFi.macAddress() + "\r\n");
    client->print("X-Config-Revision: " + revision + "\r\n");
    client->print("X-Firmware-Version: " + String(CURRENT_VERSION) + "\r\n\r\n");

    unsigned long started = millis();
    bool headersPassed = false;
    int httpCode = 0;
    String body;
    while (millis() - started < CONFIG_TIMEOUT_MS) {
        while (client->available()) {
            String line = client->readStringUntil('\n');
            line.trim();
            if (!headersPassed) {
                if (line.startsWith("HTTP/1.")) {
                    int spaceIdx = line.indexOf(' ');
                    if (spaceIdx != -1) {
                        httpCode = line.substring(spaceIdx + 1, spaceIdx + 4).toInt();
                    }
                }
                if (line.length() == 0) {
                    headersPassed = true;
                }
            } else if (body.length() < 2048) {
                body += line;
            }
        }
        if (!client->connected() && !client->available()) {
            break;
        }
        delay(10);
        esp_task_wdt_reset();
    }
    client->stop();

    if (httpCode < 200 || httpCode >= 300) {
        Serial.printf("[CONFIG]: Credential endpoint returned %d\n", httpCode);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        Serial.println("[CONFIG]: Credential payload was not JSON.");
        return;
    }
    if (applyPayload(doc)) {
        Serial.printf("[CONFIG]: Saved credential revision %d for %s\n", credentialRevision(), WiFi.macAddress().c_str());
    } else {
        Serial.println("[CONFIG]: Credentials already match the portal.");
    }
    logStoredWifi();
}

void serviceNodeConfig() {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    if (lastConfigPullMs != 0 && millis() - lastConfigPullMs < CONFIG_INTERVAL_MS) {
        return;
    }
    lastConfigPullMs = millis();
    pullNodeConfig();
}
