#include "weather.h"
#include "config.h"
#include "tts.h"
#include "radio.h"
#include <WiFi.h>
#include <HTTPClient.h>

// Cached weather report — only fetched once every 15 minutes
static String cachedWeatherReport;
static unsigned long weatherFetchTime = 0;

// Convert Open-Meteo weather code to description
static String weatherCodeToText(int code) {
  if (code == 0) return "clear sky";
  if (code == 1) return "mainly clear";
  if (code == 2) return "partly cloudy";
  if (code == 3) return "overcast";
  if (code == 45 || code == 48) return "foggy";
  if (code >= 51 && code <= 55) return "drizzle";
  if (code >= 56 && code <= 57) return "freezing drizzle";
  if (code >= 61 && code <= 65) return "rain";
  if (code >= 66 && code <= 67) return "freezing rain";
  if (code >= 71 && code <= 75) return "snow";
  if (code == 77) return "snow grains";
  if (code >= 80 && code <= 82) return "rain showers";
  if (code >= 85 && code <= 86) return "snow showers";
  if (code == 95) return "thunderstorm";
  if (code >= 96 && code <= 99) return "thunderstorm with hail";
  return "unknown conditions";
}

// Extract the "current" section from Open-Meteo JSON
static String extractCurrentSection(const String& json) {
  int idx = json.indexOf("\"current\":{");
  if (idx < 0) return "";
  idx += 10;  // Move past "current":{
  int depth = 1;
  int endIdx = idx;
  while (endIdx < (int)json.length() && depth > 0) {
    if (json[endIdx] == '{') depth++;
    if (json[endIdx] == '}') depth--;
    endIdx++;
  }
  return json.substring(idx, endIdx);
}

// Extract a float value from JSON like "temperature_2m":-4.2
static float extractJsonFloat(const String& json, const String& key) {
  String searchKey = "\"" + key + "\":";
  int idx = json.indexOf(searchKey);
  if (idx < 0) return -999;
  idx += searchKey.length();
  int endIdx = idx;
  while (endIdx < (int)json.length() && (isDigit(json[endIdx]) || json[endIdx] == '-' || json[endIdx] == '.')) {
    endIdx++;
  }
  return json.substring(idx, endIdx).toFloat();
}

// Extract an int value from JSON like "weather_code":71
static int extractJsonInt(const String& json, const String& key) {
  String searchKey = "\"" + key + "\":";
  int idx = json.indexOf(searchKey);
  if (idx < 0) return -999;
  idx += searchKey.length();
  int endIdx = idx;
  while (endIdx < (int)json.length() && (isDigit(json[endIdx]) || json[endIdx] == '-')) {
    endIdx++;
  }
  return json.substring(idx, endIdx).toInt();
}

String fetchWeatherReport() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Weather: WiFi not connected");
    return "no wifi";
  }

  // Use cached report if still fresh
  if (cachedWeatherReport.length() > 0 && millis() - weatherFetchTime < WEATHER_CACHE_MS) {
    Serial.println("Weather: using cached report");
    return cachedWeatherReport;
  }

  Serial.println("Fetching weather...");

  String weatherURL = "http://api.open-meteo.com/v1/forecast?latitude=" + String(weatherLat, 4) +
                      "&longitude=" + String(weatherLon, 4) +
                      "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m&temperature_unit=celsius&wind_speed_unit=kmh";
  Serial.printf("Weather URL: %s\n", weatherURL.c_str());

  HTTPClient http;
  http.begin(weatherURL.c_str());
  http.setTimeout(10000);

  int httpCode = http.GET();
  String report;

  if (httpCode == 200) {
    String json = http.getString();
    Serial.printf("Weather raw: %s\n", json.c_str());

    String current = extractCurrentSection(json);
    Serial.printf("Current section: %s\n", current.c_str());

    float temp = extractJsonFloat(current, "temperature_2m");
    float feelsLike = extractJsonFloat(current, "apparent_temperature");
    int humidity = (int)extractJsonFloat(current, "relative_humidity_2m");
    float wind = extractJsonFloat(current, "wind_speed_10m");
    int weatherCode = extractJsonInt(current, "weather_code");

    // eSpeak handles number pronunciation natively
    report = weatherCodeToText(weatherCode);
    report += ", " + String((int)round(temp)) + " degrees";
    report += ", feels like " + String((int)round(feelsLike)) + " degrees";
    report += ", humidity " + String(humidity) + " percent";
    report += ", winds " + String((int)round(wind)) + " kilometers per hour";

    cachedWeatherReport = report;
    weatherFetchTime = millis();
  } else {
    Serial.printf("Weather fetch failed: %d\n", httpCode);
    // If we have a stale cache, use it rather than saying "unavailable"
    if (cachedWeatherReport.length() > 0) {
      Serial.println("Weather: using stale cached report");
      report = cachedWeatherReport;
    } else {
      report = "weather unavailable";
    }
  }

  http.end();
  return report;
}

void speakWeather() {
  String report = fetchWeatherReport();
  Serial.printf("Weather report: %s\n", report.c_str());
  pttOn();
  delay(600);
  speakPreMessage();
  sayText(("Weather report, " + report).c_str());
  speakPostMessage();
  delay(1000);
  pttOff();
}
