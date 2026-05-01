#pragma once

#include <Arduino.h>

enum class DisplayState {
  Idle,
  Receiving,
  Recording,
  Playing,
  DTMFDetected,
  Transmitting,
  Booting,
  Weather,     // Speaking weather report
  TTS,         // Speaking TTS message
  Prerecord    // Playing pre-recorded slot
};

void initDisplay();
void displayShowBoot();
void displayDebug(const char* msg);
void displayRefresh();
void displayShowFull(const char* ip, const char* time);
void displayShowState();
void displaySetState(DisplayState state);
DisplayState displayGetState();
void displaySetChannel(const char* channel);
void displaySetCTCSS(const char* txCode, const char* rxCode);
void displaySetDtmf(char dtmf);
void displaySetWeather(const char* weather);
void updateDisplay(const char* ip, const char* time, DisplayState state, int rssi, int squelch);
void updateDisplayIpTime(const char* ip, const char* time);
