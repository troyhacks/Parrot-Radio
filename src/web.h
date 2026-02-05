#ifndef WEB_H
#define WEB_H

#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// Web server (extern — created in parrot.cpp)
extern WebServer server;
extern DNSServer dnsServer;
extern Preferences preferences;

// WiFi/Web initialization
void initWiFi();

// Web handlers
void handleRoot();
void handleSave();
void handleStatus();
void handleSetTime();
void handlePins();
void handleSavePins();

#endif // WEB_H
