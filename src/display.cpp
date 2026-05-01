#include "display.h"
#include "config.h"
#include <U8g2lib.h>
#include <Wire.h>
#include <string.h>
#include <stdlib.h>

// SH1106 OLED on I2C - same as LilyGo T-TWR library
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Current display values
static DisplayState currentState = DisplayState::Idle;
static char currentChannel[16] = "";  // Frequency display
static char currentCTCSS[28] = "";    // CTCSS display line (TX and/or RX)
static char currentDtmf = 0;

// Stored IP/time
static char lastIp[32] = "";
static char lastTime[32] = "";

// Weather line (always shown at bottom)
static char weatherLine[32] = "Fetching weather...";

// Dirty flags for selective update
static bool dirtyHeader = false;
static bool dirtyState = false;
static bool dirtyInfo = false;
static bool dirtyWeather = false;

// Display regions
#define REGION_HEADER_Y 0
#define REGION_HEADER_H 10
#define REGION_STATE_Y 11
#define REGION_STATE_H 20
#define REGION_INFO_Y 33
#define REGION_INFO_H 14
#define REGION_WEATHER_Y 50
#define REGION_WEATHER_H 14

// SA868 CTCSS codes to frequencies (code index 1-38)
static const char* ctcssFreqs[] = {
  "",        // 0 = no tone
  "67.0",    // 1
  "71.9",    // 2
  "74.4",    // 3
  "77.0",    // 4
  "79.7",    // 5
  "82.5",    // 6
  "85.4",    // 7
  "88.5",    // 8
  "91.5",    // 9
  "94.8",    // 10
  "97.4",    // 11
  "100.0",   // 12
  "103.5",   // 13
  "107.2",   // 14
  "110.9",   // 15
  "114.8",   // 16
  "118.8",   // 17
  "123.0",   // 18
  "127.3",   // 19
  "131.8",   // 20
  "136.5",   // 21
  "141.3",   // 22
  "146.2",   // 23
  "151.4",   // 24
  "156.7",   // 25
  "162.2",   // 26
  "167.9",   // 27
  "173.8",   // 28
  "179.9",   // 29
  "186.2",   // 30
  "192.8",   // 31
  "203.5",   // 32
  "210.7",   // 33
  "218.1",   // 34
  "225.7",   // 35
  "233.6",   // 36
  "241.8",   // 37
  "250.3"    // 38
};

// Convert CTCSS code string (like "0019") to frequency string
static const char* ctcssCodeToFreq(const char* code) {
  if (code == NULL || strlen(code) != 4) return "";
  int idx = atoi(code);
  if (idx < 0 || idx > 38) return "";
  return ctcssFreqs[idx];
}

static const char* stateToString(DisplayState state) {
  switch (state) {
    case DisplayState::Idle: return "IDLE";
    case DisplayState::Receiving: return "RX";
    case DisplayState::Recording: return "RECORD";
    case DisplayState::Playing: return "PLAYING";
    case DisplayState::DTMFDetected: return "DTMF";
    case DisplayState::Transmitting: return "TX";
    case DisplayState::Weather: return "WEATHER";
    case DisplayState::TTS: return "TTS";
    case DisplayState::Prerecord: return "TEST REC";
  }
  return "IDLE";
}

void initDisplay() {
  Serial.println("OLED: initializing SH1106...");

  uint8_t oledAddr = 0xFF;
  for (uint8_t addr = 0x3C; addr <= 0x3D; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      oledAddr = addr;
      break;
    }
  }

  if (oledAddr == 0xFF) {
    Serial.println("OLED: not found!");
    return;
  }

  u8g2.setI2CAddress(oledAddr << 1);

  if (!u8g2.begin()) {
    Serial.println("OLED: begin FAILED");
    return;
  }

  u8g2.setContrast(255);
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  Serial.println("OLED: initialized");
}

void displayShowBoot() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB14_tr);
  u8g2.drawStr(0, 30, "Parrot Radio");
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(0, 50, "T-TWR Rev 2.1");
  u8g2.drawStr(0, 60, "Starting up...");
  u8g2.sendBuffer();
}

void displayDebug(const char* msg) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB14_tr);
  u8g2.drawStr(0, 20, "Parrot Radio");
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(0, 35, "T-TWR Rev 2.1");
  u8g2.drawStr(0, 50, msg);
  u8g2.sendBuffer();
}

// Partial update: only redraw changed regions
void displayUpdate() {
  if (dirtyHeader) {
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 8, lastIp);
    u8g2.drawStr(80, 8, lastTime);
    u8g2.updateDisplayArea(0, REGION_HEADER_Y, 128, REGION_HEADER_H);
    dirtyHeader = false;
  }
  if (dirtyState) {
    u8g2.setFont(u8g2_font_ncenB14_tr);
    u8g2.drawStr(0, 28, stateToString(currentState));
    u8g2.updateDisplayArea(0, REGION_STATE_Y, 128, REGION_STATE_H);
    dirtyState = false;
  }
  if (dirtyInfo) {
    u8g2.setFont(u8g2_font_5x8_tr);
    if (currentChannel[0]) {
      u8g2.drawStr(0, 40, currentChannel);
    }
    if (currentCTCSS[0]) {
      u8g2.drawStr(0, 48, currentCTCSS);
    }
    u8g2.updateDisplayArea(0, REGION_INFO_Y, 128, REGION_INFO_H);
    dirtyInfo = false;
  }
  if (dirtyWeather) {
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 61, weatherLine);
    u8g2.updateDisplayArea(0, REGION_WEATHER_Y, 128, REGION_WEATHER_H);
    dirtyWeather = false;
  }
}

void displaySetState(DisplayState state) {
  if (currentState != state) {
    currentState = state;
    dirtyState = true;
    displayUpdate();
  }
}

DisplayState displayGetState() {
  return currentState;
}

void displaySetChannel(const char* channel) {
  if (channel == NULL) {
    if (currentChannel[0] != '\0') {
      currentChannel[0] = '\0';
      dirtyInfo = true;
      displayUpdate();
    }
  } else if (strncmp(currentChannel, channel, sizeof(currentChannel)) != 0) {
    strncpy(currentChannel, channel, sizeof(currentChannel) - 1);
    currentChannel[sizeof(currentChannel) - 1] = '\0';
    dirtyInfo = true;
    displayUpdate();
  }
}

// Set CTCSS from SA868 codes. If both TX and RX are same and non-zero, show one.
// If different and both non-zero, show both. If both zero, clear.
void displaySetCTCSS(const char* txCode, const char* rxCode) {
  // Convert codes to indices
  int txIdx = (txCode && strlen(txCode) == 4) ? atoi(txCode) : 0;
  int rxIdx = (rxCode && strlen(rxCode) == 4) ? atoi(rxCode) : 0;

  if (txIdx == 0 && rxIdx == 0) {
    // Both zero - no CTCSS
    if (currentCTCSS[0] != '\0') {
      currentCTCSS[0] = '\0';
      dirtyInfo = true;
      displayUpdate();
    }
    return;
  }

  char newCTCSS[28];
  if (txIdx == rxIdx) {
    // Same tone - show single
    snprintf(newCTCSS, sizeof(newCTCSS), "CTCSS %sHz", ctcssFreqs[txIdx]);
  } else if (txIdx == 0) {
    // Only RX
    snprintf(newCTCSS, sizeof(newCTCSS), "CTCSS RX%sHz", ctcssFreqs[rxIdx]);
  } else if (rxIdx == 0) {
    // Only TX
    snprintf(newCTCSS, sizeof(newCTCSS), "CTCSS TX%sHz", ctcssFreqs[txIdx]);
  } else {
    // Different tones - show both
    snprintf(newCTCSS, sizeof(newCTCSS), "TX%s/RX%sHz", ctcssFreqs[txIdx], ctcssFreqs[rxIdx]);
  }

  if (strncmp(currentCTCSS, newCTCSS, sizeof(currentCTCSS)) != 0) {
    strncpy(currentCTCSS, newCTCSS, sizeof(currentCTCSS) - 1);
    currentCTCSS[sizeof(currentCTCSS) - 1] = '\0';
    dirtyInfo = true;
    displayUpdate();
  }
}

void displaySetDtmf(char dtmf) {
  if (currentDtmf != dtmf) {
    currentDtmf = dtmf;
    dirtyInfo = true;
    displayUpdate();
  }
}

void displaySetWeather(const char* weather) {
  if (weather == NULL) {
    if (weatherLine[0] != '\0') {
      weatherLine[0] = '\0';
      dirtyWeather = true;
      displayUpdate();
    }
  } else if (strncmp(weatherLine, weather, sizeof(weatherLine)) != 0) {
    strncpy(weatherLine, weather, sizeof(weatherLine) - 1);
    weatherLine[sizeof(weatherLine) - 1] = '\0';
    dirtyWeather = true;
    displayUpdate();
  }
}

// Full redraw for initial display
void displayShowFull(const char* ip, const char* time) {
  if (ip) {
    strncpy(lastIp, ip, sizeof(lastIp) - 1);
    lastIp[sizeof(lastIp) - 1] = '\0';
  }
  if (time) {
    strncpy(lastTime, time, sizeof(lastTime) - 1);
    lastTime[sizeof(lastTime) - 1] = '\0';
  }

  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(0, 8, lastIp);
  u8g2.drawStr(80, 8, lastTime);

  u8g2.setFont(u8g2_font_ncenB14_tr);
  u8g2.drawStr(0, 28, stateToString(currentState));

  u8g2.setFont(u8g2_font_5x8_tr);
  if (currentChannel[0]) {
    u8g2.drawStr(0, 40, currentChannel);
  }
  if (currentCTCSS[0]) {
    u8g2.drawStr(0, 48, currentCTCSS);
  }
  u8g2.drawStr(0, 61, weatherLine);

  u8g2.sendBuffer();

  dirtyHeader = false;
  dirtyState = false;
  dirtyInfo = false;
  dirtyWeather = false;
}

// For compatibility
void updateDisplay(const char* ip, const char* time, DisplayState state, int rssi, int squelch) {
  currentState = state;
  displayShowFull(ip, time);
}

// Show state during active playback
void displayShowState() {
  displayShowFull(lastIp, lastTime);
}
