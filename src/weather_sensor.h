#pragma once
#include <Arduino.h>

struct BME280Data {
  float temperature;
  float humidity;
  float pressure;
  bool valid;
};

void initWeatherSensor();
void readWeatherSensor(BME280Data* data);
bool weatherSensorFound();
