#ifndef WEATHER_H
#define WEATHER_H

#include <Arduino.h>

// Weather functions
String fetchWeatherReport();
String getWeatherDisplayString();
void speakWeather();

#endif // WEATHER_H
