#ifndef TTS_H
#define TTS_H

#include <Arduino.h>

// Forward declare for audioWrite dependency (defined in radio.h)
void audioWrite(int16_t* data, size_t samples);

// TTS functions
void initTTS();
void sayText(const char* text);
void waitForTTSDone();
void playTone(int frequency, int duration);
void playVoiceMessage(const char* message);

// Message helpers
String expandMacros(const String &text);
void speakPreMessage();
void speakPostMessage();

// Text processing
String sanitizeForTTS(String text);

#endif // TTS_H
