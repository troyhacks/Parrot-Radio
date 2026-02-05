#include "rtc.h"
#include "config.h"
#include <Wire.h>
#include <sys/time.h>
#include <WiFi.h>

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
}

void syncNTP() {
  if (WiFi.status() != WL_CONNECTED) return;

  // Capture time before NTP sync to measure drift
  time_t beforeSync;
  time(&beforeSync);

  Serial.println("Starting NTP sync...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  // Re-apply timezone — configTime can reset TZ internally
  applyTimezone();
  struct tm t;
  int attempts = 0;
  while (!getLocalTime(&t, 100) && attempts < 50) {
    attempts++;
  }
  if (t.tm_year > 100) {  // Year > 2000 means real time
    ntpSynced = true;
    time_t afterSync;
    time(&afterSync);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    Serial.printf("NTP synced: %s (local)\n", buf);

    // Calculate and print drift
    long drift = (long)(afterSync - beforeSync);
    if (beforeSync > 1000000000) {  // Only if RTC had valid time
      Serial.printf("RTC was %+ld seconds off from NTP\n", drift);
    }

    if (rtcFound) {
      time_t now;
      time(&now);
      struct tm utc;
      gmtime_r(&now, &utc);
      ds3231Write(utc);
      Serial.println("RTC updated from NTP");
    }
  } else {
    Serial.println("NTP sync failed (timeout)");
  }
}
