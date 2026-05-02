#pragma once
#include <Arduino.h>

struct BME280Data {
  float temperature;
  float humidity;
  float pressure;
  bool valid;
};

extern BME280Data localWeather;  // Latest local weather reading

void initWeatherSensor();
void readWeatherSensor(BME280Data* data);
bool weatherSensorFound();
