#include "rtc.h"
#include "config.h"
#include <Wire.h>
#include <sys/time.h>
#include <WiFi.h>
#include "gps.h"

#ifndef BOARD_TTWR
// BCD conversion helpers
static uint8_t bcdToDec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

bool ds3231Read(struct tm &t) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  Wire.requestFrom((uint8_t)DS3231_ADDR, (uint8_t)7);
  if (Wire.available() < 7) return false;
  t.tm_sec  = bcdToDec(Wire.read() & 0x7F);
  t.tm_min  = bcdToDec(Wire.read());
  t.tm_hour = bcdToDec(Wire.read() & 0x3F);
  Wire.read();  // day of week (skip, mktime computes it)
  t.tm_mday = bcdToDec(Wire.read());
  t.tm_mon  = bcdToDec(Wire.read() & 0x1F) - 1;  // struct tm months 0-11
  t.tm_year = bcdToDec(Wire.read()) + 100;         // DS3231 stores 0-99 for 2000-2099
  t.tm_isdst = 0;
  return true;
}

void ds3231Write(const struct tm &t) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  Wire.write(decToBcd(t.tm_sec));
  Wire.write(decToBcd(t.tm_min));
  Wire.write(decToBcd(t.tm_hour));
  Wire.write(decToBcd((t.tm_wday) + 1));  // DS3231 DOW is 1-7
  Wire.write(decToBcd(t.tm_mday));
  Wire.write(decToBcd(t.tm_mon + 1));     // DS3231 months 1-12
  Wire.write(decToBcd(t.tm_year % 100));
  Wire.endTransmission();
}
#endif // !BOARD_TTWR

void applyTimezone() {
  if (timezonePosix.length() > 0) {
    setenv("TZ", timezonePosix.c_str(), 1);
    tzset();
    Serial.printf("Timezone set: %s\n", timezonePosix.c_str());
  } else {
    setenv("TZ", "UTC0", 1);
    tzset();
    Serial.println("Timezone: UTC (not configured)");
  }
}

void initRTC() {
#ifdef BOARD_TTWR
  // T-TWR doesn't have a DS3231 - use ESP32 internal RTC and rely on NTP
  Serial.println("T-TWR: DS3231 not available, using internal RTC + NTP");
  rtcFound = false;
#else
  Wire.begin(RTC_SDA, RTC_SCL);
  Wire.beginTransmission(DS3231_ADDR);
  if (Wire.endTransmission() == 0) {
    rtcFound = true;
    Serial.println("DS3231 RTC found");
    struct tm t;
    if (ds3231Read(t)) {
      // DS3231 stores UTC — set system clock (TZ is still UTC at this point)
      time_t epoch = mktime(&t);
      struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
      settimeofday(&tv, NULL);
      Serial.printf("System time set from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
    } else {
      Serial.println("DS3231 read failed (new/unprogrammed module?)");
    }
  } else {
    Serial.println("DS3231 not found on I2C bus");
  }
#endif
}

void syncNTP() {
  if (WiFi.status() != WL_CONNECTED) return;

  // Capture time before NTP sync to measure drift
  time_t beforeSync;
  time(&beforeSync);

  Serial.println("Starting NTP sync...");
  // configTime sets GMT offset — when offset=0, getLocalTime uses TZ variable for conversion
  // After configTime, re-apply TZ so it isn't overridden
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  applyTimezone();

  // Compute and cache the UTC offset while TZ is correctly set.
  // gmtime_r sets tm_isdst=0 which would make mktime use standard time (EST = UTC-5),
  // even when DST is active (EDT = UTC-4). We use localtime_r to get the correct
  // DST flag for the current date, then pass it to mktime for the EST5EDT timezone.
  const char* oldTz = getenv("TZ");
  char tzCopy[64] = {0};
  if (oldTz != nullptr && oldTz[0] != '\0') {
    strncpy(tzCopy, oldTz, sizeof(tzCopy) - 1);
  }
  struct tm nowTm;
  time_t now = time(nullptr);
  gmtime_r(&now, &nowTm);

  // Get UTC epoch with TZ=UTC0 (mktime interprets tm as UTC)
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t utcEpoch = mktime(&nowTm);

  // Get DST flag for this date using localtime_r with the real TZ
  if (tzCopy[0] != '\0') {
    setenv("TZ", tzCopy, 1);
  } else {
    setenv("TZ", "UTC0", 1);
  }
  tzset();
  struct tm tmp;
  localtime_r(&utcEpoch, &tmp);
  int dstFlag = tmp.tm_isdst;  // 1 if DST active, 0 if not

  // Now compute local epoch with TZ=EST5EDT and correct DST flag
  setenv("TZ", "UTC0", 1);
  tzset();
  nowTm.tm_isdst = dstFlag;
  time_t utcEpochCheck = mktime(&nowTm);  // same UTC epoch

  if (tzCopy[0] != '\0') {
    setenv("TZ", tzCopy, 1);
  } else {
    setenv("TZ", "UTC0", 1);
  }
  tzset();
  nowTm.tm_isdst = dstFlag;
  time_t localEpoch = mktime(&nowTm);

  gmtOffsetSeconds = (long)(localEpoch - utcEpochCheck);
  Serial.printf("UTC offset computed: %+ld seconds (%+ld hours), DST=%d\n", gmtOffsetSeconds, gmtOffsetSeconds / 3600, dstFlag);

  struct tm t;
  int attempts = 0;
  while (!getLocalTime(&t, 100) && attempts < 50) {
    attempts++;
  }
  if (t.tm_year > 100) {  // Year > 2000 means real time
    ntpSynced = true;
    time_t afterSync;
    time(&afterSync);

    char utcBuf[32];
    strftime(utcBuf, sizeof(utcBuf), "%Y-%m-%d %H:%M:%S UTC", gmtime(&afterSync));
    char localBuf[32];
    strftime(localBuf, sizeof(localBuf), "%Y-%m-%d %H:%M:%S local", &t);
    Serial.printf("NTP synced: %s | UTC: %s\n", localBuf, utcBuf);

    // Calculate and print drift
#ifndef BOARD_TTWR
    if (rtcFound && beforeSync > 1000000000) {
      long drift = (long)(afterSync - beforeSync);
      Serial.printf("RTC was %+ld seconds off from NTP\n", drift);
    }
#endif

#ifndef BOARD_TTWR
    if (rtcFound) {
      time_t now;
      time(&now);
      struct tm utc;
      gmtime_r(&now, &utc);
      ds3231Write(utc);
      Serial.println("RTC updated from NTP");
    }
#endif
  } else {
    Serial.println("NTP sync failed (timeout)");
    // Try GPS as fallback
    syncRTCFromGPS();
  }
}
