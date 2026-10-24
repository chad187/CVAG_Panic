#ifndef BACKEND_TARGET_H
#define BACKEND_TARGET_H

#include <Arduino.h>
#include "config.h"

struct BackendTarget {
    bool tls;
    String host;
    uint16_t port;
    String path;
};

inline bool parseBackendTarget(const char* suffix, BackendTarget& out) {
    String baseUrl = String(BACKEND_BASE_URL);
    out.tls = baseUrl.startsWith("https://");
    int scheme = baseUrl.indexOf("://");
    if (scheme < 0) {
        return false;
    }
    int hostStart = scheme + 3;
    int pathStart = baseUrl.indexOf('/', hostStart);
    if (pathStart <= hostStart) {
        return false;
    }

    String hostPort = baseUrl.substring(hostStart, pathStart);
    int colon = hostPort.indexOf(':');
    if (colon > 0) {
        out.host = hostPort.substring(0, colon);
        out.port = hostPort.substring(colon + 1).toInt();
    } else {
        out.host = hostPort;
        out.port = out.tls ? 443 : 80;
    }
    if (out.host.length() == 0 || out.port == 0) {
        return false;
    }
    out.path = baseUrl.substring(pathStart) + suffix;
    return true;
}

inline String backendHostHeader(const BackendTarget& target) {
    bool defaultPort = (target.tls && target.port == 443) || (!target.tls && target.port == 80);
    if (defaultPort) {
        return target.host;
    }
    return target.host + ":" + String(target.port);
}

#endif
