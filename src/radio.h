#ifndef RADIO_H
#define RADIO_H

#include <Arduino.h>
#include <HardwareSerial.h>

// SA868 UART (extern — created in parrot.cpp)
extern HardwareSerial SA868;

// I2S audio functions
void initI2S();
void i2sWrite(int16_t* data, size_t samples);

// SA868 radio functions
void initializeSA868();
int getRSSI();
bool isReceiving();

// PTT control
void pttOn();
void pttOff();

// Recording functions
void startRecording();
void stopRecording();
void recordAudioSamples();

// DTMF detection
void initGoertzel();
char detectDTMF(int16_t* samples, int count);

// Slot functions
void initSlots();
void saveToSlot(int slotIndex);
void playSlot(int slotIndex);
void playRadioTest();

// Playback
void playbackWithFeedback();
void generateQualityFeedback();

#endif // RADIO_H
