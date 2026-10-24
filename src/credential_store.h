#ifndef CREDENTIAL_STORE_H
#define CREDENTIAL_STORE_H

#include <Arduino.h>

static const int MAX_WIFI_PROFILES = 6;

void loadCredentials();
void seedCredentialsFromBuild();

int credentialCount();
const char* credentialSsid(int index);
const char* credentialPassword(int index);
const char* credentialApiPassword();
int credentialRevision();

bool credentialsDiffer(const char* apiPassword, int wifiCount, const char ssids[][33], const char passwords[][65]);
void saveCredentials(const char* apiPassword, int wifiCount, const char ssids[][33], const char passwords[][65], int revision);

#endif
