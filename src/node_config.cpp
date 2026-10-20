#include "node_config.h"
#include "backend_ca.h"
#include "config.h"
#include "credential_store.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
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
    return true;
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

    String baseUrl = String(BACKEND_BASE_URL);
    int hostStart = baseUrl.indexOf("://") + 3;
    int hostEnd = baseUrl.indexOf('/', hostStart);
    if (hostStart < 3 || hostEnd <= hostStart) {
        Serial.println("[CONFIG]: BACKEND_BASE_URL has no host path.");
        return;
    }
    String host = baseUrl.substring(hostStart, hostEnd);
    String path = baseUrl.substring(hostEnd) + "/node/config/";

    WiFiClientSecure client;
    client.setCACert(BACKEND_ROOT_CA);
    client.setHandshakeTimeout(8);
    client.setTimeout(8);

    bool connectedToHost = false;
    {
        WdtPause pauseWdt;
        connectedToHost = client.connect(host.c_str(), 443, CONFIG_TIMEOUT_MS);
    }
    if (!connectedToHost) {
        Serial.println("[CONFIG]: Credential endpoint connection failed.");
        client.stop();
        return;
    }

    String revision = String(credentialRevision());
    client.print("GET " + path + " HTTP/1.1\r\n");
    client.print("Host: " + host + "\r\n");
    client.print("Connection: close\r\n");
    client.print("Authorization: Bearer " + String(token) + "\r\n");
    client.print("X-Node-Id: " + WiFi.macAddress() + "\r\n");
    client.print("X-Config-Revision: " + revision + "\r\n\r\n");

    unsigned long started = millis();
    bool headersPassed = false;
    int httpCode = 0;
    String body;
    while (millis() - started < CONFIG_TIMEOUT_MS) {
        while (client.available()) {
            String line = client.readStringUntil('\n');
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
        if (!client.connected() && !client.available()) {
            break;
        }
        delay(10);
        esp_task_wdt_reset();
    }
    client.stop();

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
