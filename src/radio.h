#ifndef RADIO_H
#define RADIO_H

#include <Arduino.h>
#include <HardwareSerial.h>

// SA868 UART (extern — created in parrot.cpp)
extern HardwareSerial SA868;

// Audio functions - board-specific implementation
void initAudioHardware();
void audioWrite(int16_t* data, size_t samples);
void audioWrite(int16_t* data, size_t samples, uint16_t ticksPerSample);
int audioRead(int16_t* buffer, size_t samples);
void initAudioInput();  // ADC for T-TWR, I2S for original

// Calibrated ADC ticks per sample (updated during ADC calibration, used for recorded audio playback)
extern uint16_t adcTicksPerSample;
// Actual measured ADC sample rate in Hz (computed during calibration, used for DTMF Goertzel)
extern float adcSampleRate;

// Cached RSSI for squelch fallback (updated by parrot.cpp main loop)
extern int lastKnownRSSI;

// T-TWR specific: control analog switch to route audio to SA868
void setAudioRoutingToRadio(bool enable);

// T-TWR specific: mute/unmute speaker via PMU
void setSpeakerMute(bool mute);

// T-TWR specific: update AXP2101 power state (call periodically from main loop)
void updatePowerState();

// SA868 radio functions
void initializeSA868();
int getRSSI();
bool isReceiving();

// PTT control
void pttOn();
void pttOff();

// Audio drain - wait for ring buffer to empty
void drainAudio();

// Recording functions
void startRecording();
void stopRecording();
void recordAudioSamples();

// DTMF detection
void initGoertzel(float sampleRate);
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
