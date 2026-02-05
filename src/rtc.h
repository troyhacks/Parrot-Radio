#ifndef RTC_H
#define RTC_H

#include <Arduino.h>
#include <time.h>

// DS3231 RTC functions
bool ds3231Read(struct tm &t);
void ds3231Write(const struct tm &t);
void applyTimezone();
void initRTC();
void syncNTP();

#endif // RTC_H
