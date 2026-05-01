#include "display.h"
#include "config.h"
#include <U8g2lib.h>
#include <Wire.h>

// SH1106 OLED on I2C - same as LilyGo T-TWR library
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

static DisplayState currentState = DisplayState::Idle;
static char currentAction[32] = "";
static char currentChannel[16] = "";
static char currentCTCSS[16] = "";
static char currentDtmf = 0;
static bool needsRefresh = false;

void initDisplay() {
  Serial.println("OLED: initializing SH1106...");

  // Scan for OLED address (LilyGo library does this)
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

  // Use addr << 1 as LilyGo library does
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
  // Show splash screen with status message on bottom line
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB14_tr);
  u8g2.drawStr(0, 20, "Parrot Radio");
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(0, 35, "T-TWR Rev 2.1");
  u8g2.drawStr(0, 50, msg);
  u8g2.sendBuffer();
}

void displayRefresh() {
  if (!needsRefresh) return;
  needsRefresh = false;

  u8g2.clearBuffer();

  // Top bar: state indicator
  u8g2.setFont(u8g2_font_5x8_tr);
  const char* stateStr = "IDLE";
  switch (currentState) {
    case DisplayState::Idle: stateStr = "IDLE"; break;
    case DisplayState::Receiving: stateStr = "RX"; break;
    case DisplayState::Recording: stateStr = "RECORD"; break;
    case DisplayState::Playing: stateStr = "PLAYING"; break;
    case DisplayState::DTMFDetected: stateStr = "DTMF"; break;
    case DisplayState::Transmitting: stateStr = "TX"; break;
    case DisplayState::Weather: stateStr = "WEATHER"; break;
    case DisplayState::TTS: stateStr = "TTS"; break;
    case DisplayState::Prerecord: stateStr = "TEST REC"; break;
  }
  u8g2.drawStr(0, 8, stateStr);

  // Show action if set
  if (currentAction[0]) {
    u8g2.drawStr(50, 8, currentAction);
  }

  // Main display area - show channel and CTCSS
  u8g2.setFont(u8g2_font_ncenB14_tr);
  if (currentChannel[0]) {
    u8g2.drawStr(0, 35, currentChannel);
  } else {
    u8g2.drawStr(0, 35, "---");
  }

  // CTCSS
  u8g2.setFont(u8g2_font_5x8_tr);
  if (currentCTCSS[0]) {
    char ctcssLine[24];
    snprintf(ctcssLine, sizeof(ctcssLine), "CTCSS: %s", currentCTCSS);
    u8g2.drawStr(0, 47, ctcssLine);
  }

  // DTMF digit if detected
  if (currentDtmf) {
    char dtmfStr[8];
    snprintf(dtmfStr, sizeof(dtmfStr), "DTMF: %c", currentDtmf);
    u8g2.drawStr(80, 35, dtmfStr);
  }

  // Bottom: action text
  if (currentAction[0]) {
    u8g2.drawStr(0, 60, currentAction);
  }

  u8g2.sendBuffer();
}

void displaySetAction(const char* action) {
  if (action == NULL) {
    currentAction[0] = '\0';
  } else {
    strncpy(currentAction, action, sizeof(currentAction) - 1);
    currentAction[sizeof(currentAction) - 1] = '\0';
  }
  needsRefresh = true;
}

void displaySetState(DisplayState state) {
  if (currentState != state) {
    currentState = state;
    needsRefresh = true;
    // Trigger immediate refresh so state change shows right away
    displayRefresh();
  }
}

DisplayState displayGetState() {
  return currentState;
}

void displaySetChannel(const char* channel) {
  if (channel == NULL) {
    currentChannel[0] = '\0';
  } else {
    strncpy(currentChannel, channel, sizeof(currentChannel) - 1);
    currentChannel[sizeof(currentChannel) - 1] = '\0';
  }
  needsRefresh = true;
}

void displaySetCTCSS(const char* ctcss) {
  if (ctcss == NULL) {
    currentCTCSS[0] = '\0';
  } else {
    strncpy(currentCTCSS, ctcss, sizeof(currentCTCSS) - 1);
    currentCTCSS[sizeof(currentCTCSS) - 1] = '\0';
  }
  needsRefresh = true;
}

void displaySetDtmf(char dtmf) {
  currentDtmf = dtmf;
  needsRefresh = true;
  displayRefresh();  // Show immediately
}

void updateDisplay(const char* ip, const char* time, DisplayState state, int rssi, int squelch) {
  currentState = state;
  updateDisplayIpTime(ip, time);  // Store for later use

  u8g2.clearBuffer();

  // Line 1: IP address (left) and Time (right) on same line
  u8g2.setFont(u8g2_font_5x8_tr);
  if (ip) {
    u8g2.drawStr(0, 8, ip);
  }
  if (time) {
    u8g2.drawStr(80, 8, time);  // Right-aligned (128 - 8 chars * 6px = 80)
  }

  // Line 3: State
  u8g2.setFont(u8g2_font_ncenB14_tr);
  const char* stateStr = "IDLE";
  switch (state) {
    case DisplayState::Idle: stateStr = "IDLE"; break;
    case DisplayState::Receiving: stateStr = "RX"; break;
    case DisplayState::Recording: stateStr = "RECORDING"; break;
    case DisplayState::Playing: stateStr = "PLAYING"; break;
    case DisplayState::DTMFDetected: stateStr = "DTMF"; break;
    case DisplayState::Transmitting: stateStr = "TX"; break;
  }
  u8g2.drawStr(0, 30, stateStr);

  // Line 4: Channel + CTCSS
  u8g2.setFont(u8g2_font_5x8_tr);
  if (currentChannel[0]) {
    u8g2.drawStr(0, 42, currentChannel);
  }
  if (currentCTCSS[0]) {
    u8g2.drawStr(0, 52, currentCTCSS);
  }

  // RSSI / Squelch
  if (rssi > 0) {
    char rssiStr[16];
    snprintf(rssiStr, sizeof(rssiStr), "RSSI:%d", rssi);
    u8g2.drawStr(64, 42, rssiStr);
  }
  if (squelch >= 0) {
    char sqlStr[16];
    snprintf(sqlStr, sizeof(sqlStr), "SQL:%d", squelch);
    u8g2.drawStr(64, 52, sqlStr);
  }

  // DTMF if detected
  if (currentDtmf) {
    char dtmfStr[8];
    snprintf(dtmfStr, sizeof(dtmfStr), "DTMF:%c", currentDtmf);
    u8g2.drawStr(0, 62, dtmfStr);
  }

  // Action if active
  if (currentAction[0]) {
    u8g2.drawStr(40, 62, currentAction);
  }

  u8g2.sendBuffer();
}

// Show full display with current state/action - uses stored IP/time from last updateDisplay call
void displayShowFull(const char* ip, const char* time) {
  u8g2.clearBuffer();

  // Line 1: IP address (left) and Time (right)
  u8g2.setFont(u8g2_font_5x8_tr);
  if (ip) {
    u8g2.drawStr(0, 8, ip);
  }
  if (time) {
    u8g2.drawStr(80, 8, time);
  }

  // Line 2: State
  u8g2.setFont(u8g2_font_ncenB14_tr);
  const char* stateStr = "IDLE";
  switch (currentState) {
    case DisplayState::Idle: stateStr = "IDLE"; break;
    case DisplayState::Receiving: stateStr = "RX"; break;
    case DisplayState::Recording: stateStr = "RECORDING"; break;
    case DisplayState::Playing: stateStr = "PLAYING"; break;
    case DisplayState::DTMFDetected: stateStr = "DTMF"; break;
    case DisplayState::Transmitting: stateStr = "TX"; break;
    case DisplayState::Weather: stateStr = "WEATHER"; break;
    case DisplayState::TTS: stateStr = "TTS"; break;
    case DisplayState::Prerecord: stateStr = "PRERECORD"; break;
  }
  u8g2.drawStr(0, 30, stateStr);

  // Line 3: Channel or action
  u8g2.setFont(u8g2_font_5x8_tr);
  if (currentAction[0]) {
    u8g2.drawStr(0, 42, currentAction);
  } else if (currentChannel[0]) {
    u8g2.drawStr(0, 42, currentChannel);
  }

  // DTMF if detected
  if (currentDtmf) {
    char dtmfStr[8];
    snprintf(dtmfStr, sizeof(dtmfStr), "DTMF:%c", currentDtmf);
    u8g2.drawStr(80, 42, dtmfStr);
  }

  u8g2.sendBuffer();
}

// Static storage for last IP/time so displayShowState can use them
static char lastIp[32] = "";
static char lastTime[32] = "";

// Show current state/action with last known IP/time - call this during active states
void displayShowState() {
  displayShowFull(lastIp[0] ? lastIp : NULL, lastTime[0] ? lastTime : NULL);
}

// Update the stored IP/time (called by updateDisplay)
void updateDisplayIpTime(const char* ip, const char* time) {
  if (ip) strncpy(lastIp, ip, sizeof(lastIp) - 1);
  if (time) strncpy(lastTime, time, sizeof(lastTime) - 1);
}
