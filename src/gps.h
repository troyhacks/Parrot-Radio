#pragma once

#include <Arduino.h>

// GPS state
struct GPSData {
  bool valid;          // GPS fix acquired
  float latitude;       // Degrees North
  float longitude;      // Degrees East
  float altitude;       // Meters
  int satellites;       // Number of satellites
  float speed;          // Speed in km/h
  float course;         // Course in degrees
  int hour;
  int minute;
  int second;
  int day;
  int month;
  int year;
};

extern GPSData gpsData;
extern bool gpsFound;

// Initialize GPS serial and PPS pin
void initGPS();

// Update GPS data from serial (call in loop)
// Returns true when new valid data received
bool updateGPS();

// Check if GPS has a fix
bool gpsHasFix();

// Use GPS time to sync RTC if not synced via NTP
void syncRTCFromGPS();

// Update timezone from current GPS coordinates (call periodically when GPS has a fix)
void updateTimezoneFromGPS();
