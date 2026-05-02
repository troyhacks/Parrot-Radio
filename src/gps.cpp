#include "gps.h"
#include "config.h"
#include "rtc.h"
#include "timezone_lookup.h"
#include <time.h>
#include <sys/time.h>
#include <HardwareSerial.h>

// Use Serial1 for GPS
static HardwareSerial gpsSerial(1);

// GPS state
GPSData gpsData = {0};
bool gpsFound = false;

// PPS pin state
static volatile bool ppsReceived = false;
static unsigned long ppsTime = 0;

static void IRAM_ATTR ppsHandler() {
  ppsReceived = true;
  ppsTime = millis();
}

// Convert NMEA lat/lon to decimal degrees
// Format: ddmm.mmmm (latitude) or dddmm.mmmm (longitude)
static float nmeaToDecimal(const char* nmea, char dir) {
  if (!nmea || strlen(nmea) < 6) return 0;

  // Find the decimal point to split degrees and minutes
  const char* dot = strchr(nmea, '.');
  if (!dot) return 0;

  // Calculate positions
  int dotIdx = dot - nmea;
  int degLen = dotIdx - 2;  // Last 2 digits are minutes
  int minStart = degLen;

  char degStr[4] = {0};
  char minStr[8] = {0};

  strncpy(degStr, nmea, degLen);
  degStr[degLen] = '\0';
  strncpy(minStr, nmea + minStart, 7);
  minStr[7] = '\0';

  float deg = atof(degStr);
  float min = atof(minStr);

  float decimal = deg + min / 60.0f;
  if (dir == 'S' || dir == 's') decimal = -decimal;
  if (dir == 'W' || dir == 'w') decimal = -decimal;
  return decimal;
}

// Parse a GPRMC sentence
// $GPRMC,time,status,lat,N/S,lon,E/W,speed,course,date,...
static bool parseGPRMC(const char* sentence) {
  if (!sentence) return false;

  // Make a copy since strtok modifies the string
  static char buf[128];
  strncpy(buf, sentence, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* token = strtok(buf, ",");
  if (!token) return false;

  // Check sentence type
  if (strcmp(token, "$GPRMC") != 0 && strcmp(token, "$GNRMC") != 0) {
    return false;
  }

  // Time (field 1)
  token = strtok(NULL, ",");
  if (!token || strlen(token) < 6) return false;
  int hour = atoi(token);
  int minute = atoi(token + 2);
  int second = atoi(token + 4);
  gpsData.hour = hour;
  gpsData.minute = minute;
  gpsData.second = second;

  // Status (field 2) - A=active/valid, V=void/invalid
  token = strtok(NULL, ",");
  if (!token) return false;
  if (token[0] != 'A') {
    gpsData.valid = false;
    return false;
  }

  // Latitude (fields 3-4)
  char latStr[16] = {0};
  char latDir = 'N';
  token = strtok(NULL, ",");
  if (token) {
    strncpy(latStr, token, sizeof(latStr) - 1);
    token = strtok(NULL, ",");
    if (token) latDir = token[0];
  }
  gpsData.latitude = nmeaToDecimal(latStr, latDir);

  // Longitude (fields 5-6)
  char lonStr[16] = {0};
  char lonDir = 'E';
  token = strtok(NULL, ",");
  if (token) {
    strncpy(lonStr, token, sizeof(lonStr) - 1);
    token = strtok(NULL, ",");
    if (token) lonDir = token[0];
  }
  gpsData.longitude = nmeaToDecimal(lonStr, lonDir);

  // Speed (field 7) - knots
  token = strtok(NULL, ",");
  if (token) {
    gpsData.speed = atof(token) * 1.852f;  // knots to km/h
  }

  // Course (field 8)
  token = strtok(NULL, ",");
  if (token) {
    gpsData.course = atof(token);
  }

  // Date (field 9) - ddmmyy format
  token = strtok(NULL, ",");
  if (token && strlen(token) >= 6) {
    gpsData.day = atoi(token);
    gpsData.month = atoi(token + 2);
    gpsData.year = 2000 + atoi(token + 4);
  }

  gpsData.valid = true;
  return true;
}

// Parse a GPGGA sentence
static bool parseGPGGA(const char* sentence) {
  if (!sentence) return false;

  static char buf[128];
  strncpy(buf, sentence, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* token = strtok(buf, ",");
  if (!token) return false;
  if (strcmp(token, "$GPGGA") != 0 && strcmp(token, "$GNGGA") != 0) {
    return false;
  }

  // Time (field 1)
  token = strtok(NULL, ",");
  if (token && strlen(token) >= 6) {
    gpsData.hour = atoi(token);
    gpsData.minute = atoi(token + 2);
    gpsData.second = atoi(token + 4);
  }

  // Skip to satellites (field 6)
  for (int i = 0; i < 4; i++) {
    token = strtok(NULL, ",");
    if (!token) break;
  }
  if (token) {
    gpsData.satellites = atoi(token);
  }

  // Skip to altitude (field 9)
  for (int i = 0; i < 3; i++) {
    token = strtok(NULL, ",");
    if (!token) break;
  }
  if (token) {
    gpsData.altitude = atof(token);
  }

  return true;
}

void initGPS() {
  Serial.println("GPS: initializing...");

  // Configure PPS pin
  pinMode(GPS_PPS, INPUT_PULLUP);
  attachInterrupt(GPS_PPS, ppsHandler, RISING);

  // Initialize serial for GPS
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.printf("GPS: serial initialized on TX=%d RX=%d at 9600 baud\n", GPS_TX, GPS_RX);

  // Give GPS time to start up and drain any startup garbage
  delay(1000);
  while (gpsSerial.available()) {
    gpsSerial.read();
  }

  // Send a basic NMEA query to configure GPS
  // This is optional but helps ensure proper mode
  gpsSerial.println("$PUBX,40,GLL,0,0,0,0*5C");  // Disable GLL
  gpsSerial.println("$PUBX,40,RMC,0,1,0,0*47");   // Enable RMC
  gpsSerial.println("$PUBX,40,GG A,0,1,0,0*46"); // Enable GGA

  gpsFound = true;
  Serial.println("GPS: module initialized");
}

bool updateGPS() {
  if (!gpsFound) return false;

  static char line[128];
  static int pos = 0;
  bool newData = false;

  while (gpsSerial.available()) {
    char c = gpsSerial.read();
    if (c == '\n') {
      line[pos] = '\0';
      pos = 0;

      if (strstr(line, "RMC")) {
        if (parseGPRMC(line)) {
          newData = true;
        }
      } else if (strstr(line, "GGA")) {
        parseGPGGA(line);
      }
    } else if (pos < (int)sizeof(line) - 1 && c != '\r') {
      line[pos++] = c;
    }
  }

  return newData;
}

bool gpsHasFix() {
  return gpsData.valid;
}

void syncRTCFromGPS() {
  // Use GPS time to sync system clock (GPS time is in UTC)
  if (!gpsData.valid) {
    Serial.println("GPS: no fix, cannot sync time");
    return;
  }

  // GPS gives us UTC time - convert to epoch
  struct tm t;
  t.tm_year = gpsData.year - 1900;  // struct tm years are from 1900
  t.tm_mon = gpsData.month - 1;      // struct tm months are 0-11
  t.tm_mday = gpsData.day;
  t.tm_hour = gpsData.hour;
  t.tm_min = gpsData.minute;
  t.tm_sec = gpsData.second;
  t.tm_isdst = 0;

  // Convert to epoch seconds (UTC)
  time_t epoch = mktime(&t);
  if (epoch == -1) {
    Serial.println("GPS: failed to convert time");
    return;
  }

  // Set system time
  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, NULL);

  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &t);
  Serial.printf("GPS time synced: %s\n", buf);

  // Look up timezone from coordinates
  const char* tz = getTimezoneForCoords(gpsData.latitude, gpsData.longitude);
  if (tz) {
    timezonePosix = tz;
    applyTimezone();
    Serial.printf("Timezone set from GPS: %s\n", tz);
  }
}

void updateTimezoneFromGPS() {
  // Timezone is set from preferences at boot - GPS coordinates are for display only
  // Do NOT update timezone from GPS coordinates to avoid clock drift issues
}
