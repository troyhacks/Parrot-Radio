#pragma once

#include <Arduino.h>

// Solar calculation result
struct SolarTimes {
  int sunriseHour;
  int sunriseMinute;
  int sunsetHour;
  int sunsetMinute;
  int sunriseCivilHour;
  int sunriseCivilMinute;
  int sunsetCivilHour;
  int sunsetCivilMinute;
  int sunriseNauticalHour;
  int sunriseNauticalMinute;
  int sunsetNauticalHour;
  int sunsetNauticalMinute;
  int sunriseAstronomicalHour;
  int sunriseAstronomicalMinute;
  int sunsetAstronomicalHour;
  int sunsetAstronomicalMinute;
  int goldenHourEveningEndHour;
  int goldenHourEveningEndMinute;
  int goldenHourMorningStartHour;
  int goldenHourMorningStartMinute;
  bool valid;
};

// Calculate sunrise and sunset for a given date and location
// Uses the NOAA solar calculator algorithm
SolarTimes calculateSunTimes(float latitude, float longitude, int year, int month, int day);

// Calculate sun times for a specific zenith angle (e.g., 96.0 for civil twilight)
// zenith: angle of sun below horizon (90.833 = official sunrise/sunset)
SolarTimes calculateSunTimesForZenith(float latitude, float longitude, int year, int month, int day, float zenith);

// Get sunrise time as a string (e.g., "6:45 AM")
const char* getSunriseString();

// Get sunset time as a string (e.g., "8:30 PM")
const char* getSunsetString();

// Get sunrise time as words for TTS (e.g., "six oh seven AM")
String getSunriseWords();

// Get sunset time as words for TTS (e.g., "eight twenty one PM")
String getSunsetWords();

// Get astronomical dusk (night sky dusk) as words for TTS
String getAstronomicalDuskWords();

// Check if current time is between sunrise and sunset
bool isDaytime();

// Get next golden hour as words (e.g., "morning golden hour at six thirty PM")
// Returns empty string if no golden hour found today
String getNextGoldenHourWords();
