#pragma once

#include <Arduino.h>

struct TZRegion {
  float minLat;
  float maxLat;
  float minLon;
  float maxLon;
  const char* posix;
};

const char* getTimezoneForCoords(float lat, float lon);
