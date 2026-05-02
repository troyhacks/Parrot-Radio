#include "sun.h"
#include "config.h"
#include <time.h>
#include <math.h>

// Constants
static const float RAD = PI / 180.0f;
static const float DEG = 180.0f / PI;

// Static storage for latest calculation
static SolarTimes lastResult = {0};
static char sunriseStr[16] = "--:--";
static char sunsetStr[16] = "--:--";

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

// Convert UTC broken-down time to local using the cached UTC offset.
// gmtOffsetSeconds is computed once in syncNTP after timezone is set.
static bool utcToLocal(int year, int month, int day, int utcHour, int utcMinute, int& localHour, int& localMinute) {
  if (utcHour < 0 || utcMinute < 0) return false;

  // Convert UTC hour/minute to minutes since midnight UTC
  int utcTotalMins = utcHour * 60 + utcMinute;

  // Convert offset from seconds to minutes
  int offsetMins = (int)(gmtOffsetSeconds / 60);

  // Apply offset to get local minutes since midnight
  int localTotalMins = utcTotalMins + offsetMins;

  // Normalize to 0-1439 range (handle day boundary)
  while (localTotalMins < 0) localTotalMins += 1440;
  while (localTotalMins >= 1440) localTotalMins -= 1440;

  localHour = localTotalMins / 60;
  localMinute = localTotalMins % 60;
  return true;
}

// Calculate day of year (1 = Jan 1)
static int dayOfYear(int year, int month, int day) {
  static const int daysPerMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int doy = day;
  for (int m = 1; m < month; m++) {
    doy += daysPerMonth[m];
  }
  if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
    if (month > 2) doy++;
  }
  return doy;
}

// Calculate sun times for a specific zenith angle
// Returns hour and minute (in UTC) when sun reaches that zenith
// Returns false if sun never reaches this zenith at this location
static bool calculateSunTimeForZenith(float latitude, float longitude, int year, int month, int day,
                                     float zenith, bool forSunrise, int& outHour, int& outMinute) {
  if (latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) {
    return false;
  }

  float latRad = latitude * RAD;
  int doy = dayOfYear(year, month, day);
  float lngHour = longitude / 15.0f;

  // Approximate time
  float t = doy + ((forSunrise ? 6.0f : 18.0f) - lngHour) / 24.0f;

  // Sun's mean anomaly
  float m = (0.9856f * t) - 3.289f;

  // Sun's true longitude
  float l = m + (1.916f * sinf(m * RAD)) + (0.020f * sinf(2.0f * m * RAD)) + 282.634f;
  l = fmodf(l, 360.0f);
  if (l < 0) l += 360.0f;

  // Sun's right ascension
  float ra = atanf(0.91764f * tanf(l * RAD)) * DEG;
  float lQuad = floorf(l / 90.0f) * 90.0f;
  ra += (lQuad - lQuad);  // Adjust quadrant
  ra = fmodf(ra, 360.0f);

  // Convert RA to hours
  float raHour = ra / 15.0f;

  // Sun's declination
  float sinDec = 0.39782f * sinf(l * RAD);
  float cosDec = cosf(asinf(sinDec));

  // Hour angle for this zenith
  float cosH = (cosf(zenith * RAD) - (sinDec * sinf(latRad))) / (cosDec * cosf(latRad));

  if (cosH > 1.0f || cosH < -1.0f) {
    return false;  // Sun never reaches this zenith
  }

  // Hour angle in degrees then hours
  float h = forSunrise ? (360.0f - acosf(cosH) * DEG) : acosf(cosH) * DEG;
  float hHour = h / 15.0f;

  // Local time
  float localT = hHour + raHour - (0.06571f * t) - 6.622f;

  // Convert to UTC
  float utc = localT + lngHour;
  utc = fmodf(utc, 24.0f);
  if (utc < 0) utc += 24.0f;

  outHour = (int)utc;
  outMinute = (int)((utc - outHour) * 60.0f);
  return true;
}

SolarTimes calculateSunTimes(float latitude, float longitude, int year, int month, int day) {
  SolarTimes result = {0};

  // Helper to convert UTC h:m to local and store in result fields
  // Returns true only if conversion succeeds
  auto convertAndStore = [&](int utcH, int utcM, int& outH, int& outM) -> bool {
    int lH = -1, lM = -1;
    if (utcToLocal(year, month, day, utcH, utcM, lH, lM)) {
      outH = lH;
      outM = lM;
      return true;
    }
    outH = -1;
    outM = -1;
    return false;
  };

  // Official sunrise/sunset (zenith = 90.833°)
  int riseH = -1, riseM = -1, setH = -1, setM = -1;
  bool hasRise = calculateSunTimeForZenith(latitude, longitude, year, month, day, 90.833f, true, riseH, riseM);
  bool hasSet = calculateSunTimeForZenith(latitude, longitude, year, month, day, 90.833f, false, setH, setM);

  if (hasRise) convertAndStore(riseH, riseM, result.sunriseHour, result.sunriseMinute);
  if (hasSet) convertAndStore(setH, setM, result.sunsetHour, result.sunsetMinute);

  // Civil twilight (6° below horizon = 96° zenith)
  int cRiseH, cRiseM, cSetH, cSetM;
  if (calculateSunTimeForZenith(latitude, longitude, year, month, day, 96.0f, true, cRiseH, cRiseM))
    convertAndStore(cRiseH, cRiseM, result.sunriseCivilHour, result.sunriseCivilMinute);
  if (calculateSunTimeForZenith(latitude, longitude, year, month, day, 96.0f, false, cSetH, cSetM))
    convertAndStore(cSetH, cSetM, result.sunsetCivilHour, result.sunsetCivilMinute);

  // Nautical twilight (12° below = 102° zenith)
  int nRiseH, nRiseM, nSetH, nSetM;
  if (calculateSunTimeForZenith(latitude, longitude, year, month, day, 102.0f, true, nRiseH, nRiseM))
    convertAndStore(nRiseH, nRiseM, result.sunriseNauticalHour, result.sunriseNauticalMinute);
  if (calculateSunTimeForZenith(latitude, longitude, year, month, day, 102.0f, false, nSetH, nSetM))
    convertAndStore(nSetH, nSetM, result.sunsetNauticalHour, result.sunsetNauticalMinute);

  // Astronomical twilight (18° below = 108° zenith)
  int aRiseH, aRiseM, aSetH, aSetM;
  if (calculateSunTimeForZenith(latitude, longitude, year, month, day, 108.0f, true, aRiseH, aRiseM))
    convertAndStore(aRiseH, aRiseM, result.sunriseAstronomicalHour, result.sunriseAstronomicalMinute);
  if (calculateSunTimeForZenith(latitude, longitude, year, month, day, 108.0f, false, aSetH, aSetM))
    convertAndStore(aSetH, aSetM, result.sunsetAstronomicalHour, result.sunsetAstronomicalMinute);

  // Golden hour (sun at 6° above horizon = 84° zenith)
  int geH, geM, gmH, gmM;
  if (calculateSunTimeForZenith(latitude, longitude, year, month, day, 84.0f, false, geH, geM))
    convertAndStore(geH, geM, result.goldenHourEveningEndHour, result.goldenHourEveningEndMinute);
  if (calculateSunTimeForZenith(latitude, longitude, year, month, day, 84.0f, true, gmH, gmM))
    convertAndStore(gmH, gmM, result.goldenHourMorningStartHour, result.goldenHourMorningStartMinute);

  result.valid = hasRise && hasSet;
  lastResult = result;

  // Format strings for official sunrise/sunset
  if (hasRise) formatTime(result.sunriseHour, result.sunriseMinute, sunriseStr, sizeof(sunriseStr));
  if (hasSet) formatTime(result.sunsetHour, result.sunsetMinute, sunsetStr, sizeof(sunsetStr));

  return result;
}

const char* getSunriseString() {
  return sunriseStr;
}

const char* getSunsetString() {
  return sunsetStr;
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
