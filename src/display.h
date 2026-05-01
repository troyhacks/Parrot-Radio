#pragma once

#include <Arduino.h>

enum class DisplayState {
  Idle,
  Receiving,
  Recording,
  Playing,
  DTMFDetected,
  Transmitting
};

void initDisplay();
void displayShowBoot();
void displayRefresh();
void displaySetAction(const char* action);
void displaySetState(DisplayState state);
DisplayState displayGetState();
void displaySetChannel(const char* channel);
void displaySetCTCSS(const char* ctcss);
void displaySetDtmf(char dtmf);
void updateDisplay(const char* ip, const char* time, DisplayState state, int rssi, int squelch);
