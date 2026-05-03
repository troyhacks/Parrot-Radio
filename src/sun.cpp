#include "sun.h"
#include "config.h"
#include <time.h>
#include <Sunset.h>

// Static storage for latest calculation
static SolarTimes lastResult = {0};
static char sunriseStr[16] = "--:--";
static char sunsetStr[16] = "--:--";

// Sunset library instance
static SunSet sunCalc;

// Format a time as "6:45 AM" or "8:30 PM"
static void formatTime(int hour, int minute, char* out, size_t len) {
  if (hour < 0 || minute < 0) {
    snprintf(out, len, "--:--");
    return;
  }
  if (hour == 0) {
    snprintf(out, len, "12:%02d AM", minute);
  } else if (hour < 12) {
    snprintf(out, len, "%d:%02d AM", hour, minute);
  } else if (hour == 12) {
    snprintf(out, len, "12:%02d PM", minute);
  } else {
    snprintf(out, len, "%d:%02d PM", hour - 12, minute);
  }
}

// Convert minutes-past-midnight to hour:minute
static void minsToTime(int mins, int& hour, int& minute) {
  if (mins < 0) {
    hour = -1;
    minute = -1;
    return;
  }
  hour = mins / 60;
  minute = mins % 60;
}

// Number to English words (for TTS pronunciation)
static const char* s_ones[] = {"zero", "one", "two", "three", "four", "five ", "six", "seven", "eight", "nine",
                                "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
static const char* s_tens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};

static String intToWords(int n) {
  if (n == 0) return "zero";
  String s;
  if (n < 0) { s = "minus "; n = -n; }
  if (n >= 100) { s += s_ones[n/100]; s += " hundred "; n %= 100; }
  if (n >= 20) {
    if (n >= 50 && n < 60) {
      s += "five ";
      s += s_ones[n % 10];
    } else {
      s += s_tens[n/10];
      if (n % 10) { s += " "; s += s_ones[n % 10]; }
    }
  }
  else if (n >= 1) s += s_ones[n];
  return s;
}

// Format time as words for TTS (e.g., "six oh seven AM")
static String formatTimeHMWords(int hour, int minute) {
  if (hour < 0 || minute < 0) return "unknown";
  String s;
  int h12 = hour % 12;
  if (h12 == 0) h12 = 12;
  s = intToWords(h12);
  if (minute == 0) {
    // Exact hour
  } else if (minute < 10) {
    s += " oh " + intToWords(minute);
  } else {
    s += " " + intToWords(minute);
  }
  s += hour < 12 ? " AM" : " PM";
  return s;
}

SolarTimes calculateSunTimes(float latitude, float longitude, int year, int month, int day) {
  SolarTimes result = {0};

  // Get timezone offset in hours
  // gmtOffsetSeconds is +14400 for EDT (UTC-4), meaning local = UTC + 4
  // But Sunset library adds this to UTC to get local, so we need to negate
  // to convert UTC→local: local = UTC + (-gmtOffsetSeconds/3600)
  float tzOffsetHours = -gmtOffsetSeconds / 3600.0f;

  // Initialize sun calculator with position and timezone
  sunCalc.setPosition(latitude, longitude, tzOffsetHours);
  sunCalc.setCurrentDate(year, month, day);

  // Official sunrise/sunset (zenith = 90.833°)
  int riseMins = sunCalc.calcSunrise();
  int setMins = sunCalc.calcSunset();

  minsToTime(riseMins, result.sunriseHour, result.sunriseMinute);
  minsToTime(setMins, result.sunsetHour, result.sunsetMinute);

  // Civil twilight (6° below horizon = 96° zenith)
  int civilRiseMins = sunCalc.calcCivilSunrise();
  int civilSetMins = sunCalc.calcCivilSunset();
  minsToTime(civilRiseMins, result.sunriseCivilHour, result.sunriseCivilMinute);
  minsToTime(civilSetMins, result.sunsetCivilHour, result.sunsetCivilMinute);

  // Nautical twilight (12° below = 102° zenith)
  int nautRiseMins = sunCalc.calcNauticalSunrise();
  int nautSetMins = sunCalc.calcNauticalSunset();
  minsToTime(nautRiseMins, result.sunriseNauticalHour, result.sunriseNauticalMinute);
  minsToTime(nautSetMins, result.sunsetNauticalHour, result.sunsetNauticalMinute);

  // Astronomical twilight (18° below = 108° zenith)
  int astroRiseMins = sunCalc.calcAstronomicalSunrise();
  int astroSetMins = sunCalc.calcAstronomicalSunset();
  minsToTime(astroRiseMins, result.sunriseAstronomicalHour, result.sunriseAstronomicalMinute);
  minsToTime(astroSetMins, result.sunsetAstronomicalHour, result.sunsetAstronomicalMinute);

  // Golden hour - sun at 6° above horizon = 84° zenith
  // The library doesn't have golden hour, so we use sunset - 1 hour approximation
  // or we can compute it manually
  result.goldenHourEveningEndHour = -1;
  result.goldenHourEveningEndMinute = -1;
  result.goldenHourMorningStartHour = -1;
  result.goldenHourMorningStartMinute = -1;

  result.valid = (riseMins >= 0 && setMins >= 0);
  lastResult = result;

  // Format strings for official sunrise/sunset
  if (result.valid) {
    formatTime(result.sunriseHour, result.sunriseMinute, sunriseStr, sizeof(sunriseStr));
    formatTime(result.sunsetHour, result.sunsetMinute, sunsetStr, sizeof(sunsetStr));
  }

  return result;
}

const char* getSunriseString() {
  return sunriseStr;
}

const char* getSunsetString() {
  return sunsetStr;
}

String getSunriseWords() {
  if (!lastResult.valid) return "unknown";
  return formatTimeHMWords(lastResult.sunriseHour, lastResult.sunriseMinute);
}

String getSunsetWords() {
  if (!lastResult.valid) return "unknown";
  return formatTimeHMWords(lastResult.sunsetHour, lastResult.sunsetMinute);
}

String getAstronomicalDuskWords() {
  if (!lastResult.valid) return "unknown";
  return formatTimeHMWords(lastResult.sunsetAstronomicalHour, lastResult.sunsetAstronomicalMinute);
}

bool isDaytime() {
  if (!lastResult.valid) return false;

  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  int hour = t->tm_hour;
  int minute = t->tm_min;

  int currentMins = hour * 60 + minute;
  int sunriseMins = lastResult.sunriseHour * 60 + lastResult.sunriseMinute;
  int sunsetMins = lastResult.sunsetHour * 60 + lastResult.sunsetMinute;

  return (currentMins >= sunriseMins && currentMins < sunsetMins);
}

String getNextGoldenHourWords() {
  if (!lastResult.valid) return "";

  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  int currentMins = t->tm_hour * 60 + t->tm_min;

  int morningMins = lastResult.goldenHourMorningStartHour * 60 + lastResult.goldenHourMorningStartMinute;
  int eveningMins = lastResult.goldenHourEveningEndHour * 60 + lastResult.goldenHourEveningEndMinute;

  // Determine which golden hour is next
  int ghH = -1, ghM = -1;
  String ghType;

  if (currentMins < morningMins) {
    ghType = "morning golden hour at ";
    ghH = lastResult.goldenHourMorningStartHour;
    ghM = lastResult.goldenHourMorningStartMinute;
  } else if (currentMins < eveningMins) {
    ghType = "evening golden hour at ";
    ghH = lastResult.goldenHourEveningEndHour;
    ghM = lastResult.goldenHourEveningEndMinute;
  } else {
    // Both passed today, return empty (or could wrap to tomorrow)
    return "";
  }

  if (ghH < 0) return "";
  return ghType + formatTimeHMWords(ghH, ghM);
}
