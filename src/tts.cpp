#include "tts.h"
#include "config.h"
#include <WiFi.h>
#include <time.h>
#include "espeak.h"

// TTS output buffer
static int16_t ttsBuffer[512];
static int ttsBufferIndex = 0;

// eSpeak audio output — Print subclass that feeds our I2S
class TTSOutput : public Print {
public:
  size_t write(uint8_t b) override {
    return write(&b, 1);
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    // eSpeak writes raw 16-bit PCM samples
    size_t i = 0;
    while (i < size) {
      // Accumulate bytes into int16_t samples
      if (i + 1 < size) {
        int16_t sample = (int16_t)(buffer[i] | (buffer[i + 1] << 8));
        sample = (int16_t)(sample * samVolumePercent / 100.0f);
        ttsBuffer[ttsBufferIndex++] = sample;
        i += 2;

        if (ttsBufferIndex >= 512) {
          i2sWrite(ttsBuffer, ttsBufferIndex);
          ttsBufferIndex = 0;
        }
      } else {
        i++;  // Odd trailing byte, skip
      }
    }
    return size;
  }

  void flush() {
    if (ttsBufferIndex > 0) {
      i2sWrite(ttsBuffer, ttsBufferIndex);
      ttsBufferIndex = 0;
    }
  }
};

static TTSOutput ttsOut;
static ESpeak espeak(ttsOut);

// ==================== Macro Expansion ====================
// Expands {tokens} in message strings with live values
String expandMacros(const String &text) {
  String result = text;
  // Date/time macros
  struct tm t;
  if (getLocalTime(&t, 0)) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
    result.replace("{date}", buf);
    strftime(buf, sizeof(buf), "%H:%M", &t);
    result.replace("{time}", buf);
    {
      int hour12 = t.tm_hour % 12;
      if (hour12 == 0) hour12 = 12;
      const char* ampm = t.tm_hour < 12 ? "AM" : "PM";
      if (t.tm_min == 0) {
        snprintf(buf, sizeof(buf), "%d %s", hour12, ampm);
      } else if (t.tm_min < 10) {
        snprintf(buf, sizeof(buf), "%d oh %d %s", hour12, t.tm_min, ampm);
      } else {
        snprintf(buf, sizeof(buf), "%d %d %s", hour12, t.tm_min, ampm);
      }
      result.replace("{time12}", buf);
    }
    strftime(buf, sizeof(buf), "%A", &t);
    result.replace("{day}", buf);
    strftime(buf, sizeof(buf), "%H", &t);
    result.replace("{hour}", buf);
    strftime(buf, sizeof(buf), "%M", &t);
    result.replace("{minute}", buf);
  } else {
    result.replace("{date}", "unknown");
    result.replace("{time}", "unknown");
    result.replace("{time12}", "unknown");
    result.replace("{day}", "unknown");
    result.replace("{hour}", "unknown");
    result.replace("{minute}", "unknown");
  }
  // Battery macros
  if (lastBatteryPct >= 0) {
    result.replace("{battery}", String(lastBatteryPct) + " percent");
    result.replace("{voltage}", String(lastBatteryV, 1) + " volts");
  } else {
    result.replace("{battery}", "unknown");
    result.replace("{voltage}", "unknown");
  }
  // Slot macros
  result.replace("{slot}", String(nextSlot + 1));
  int usedSlots = 0;
  for (int i = 0; i < MAX_SLOTS; i++) {
    if (slots[i].sampleCount > 0) usedSlots++;
  }
  result.replace("{slots_used}", String(usedSlots));
  result.replace("{slots_total}", String(MAX_SLOTS));
  // Radio/system macros
  result.replace("{freq}", radioFreq);
  result.replace("{uptime}", String(millis() / 60000) + " minutes");
  result.replace("{ip}", WiFi.localIP().toString());
  return result;
}

// Phoneme pronunciations for words eSpeak's minimal dictionary can't handle
// Uses eSpeak Kirshenbaum notation inside [[ ]] brackets (requires espeakPHONEMES flag)
struct PhonemeEntry { const char* word; const char* phonemes; };
static const PhonemeEntry ttsPronunciations[] = {
  { "overcast",     "[['oUv@kast]]" },
  // { "drizzle",      "[[dr'Iz@L]]" },
  // { "thunderstorm", "[[T'Vnd@stO:rm]]" },
};
static const int ttsPronunciationCount = sizeof(ttsPronunciations) / sizeof(ttsPronunciations[0]);

// Case-insensitive whole-word replacement with phoneme codes
static void applyPhonemes(String &text) {
  for (int i = 0; i < ttsPronunciationCount; i++) {
    String wordLower = ttsPronunciations[i].word;
    String textLower = text;
    textLower.toLowerCase();
    int pos = 0;
    while ((pos = textLower.indexOf(wordLower, pos)) >= 0) {
      int endPos = pos + wordLower.length();
      bool wordStart = (pos == 0 || !isAlpha(text[pos - 1]));
      bool wordEnd = (endPos >= (int)text.length() || !isAlpha(text[endPos]));
      if (wordStart && wordEnd) {
        String replacement = ttsPronunciations[i].phonemes;
        text = text.substring(0, pos) + replacement + text.substring(endPos);
        textLower = text;
        textLower.toLowerCase();
        pos += replacement.length();
      } else {
        pos++;
      }
    }
  }
}

// Text sanitization for TTS
String sanitizeForTTS(String text) {
  // Remove wind direction arrows
  text.replace("\xe2\x86\x91", "");  // ↑
  text.replace("\xe2\x86\x93", "");  // ↓
  text.replace("\xe2\x86\x90", "");  // ←
  text.replace("\xe2\x86\x92", "");  // →
  text.replace("\xe2\x86\x97", "");  // ↗
  text.replace("\xe2\x86\x98", "");  // ↘
  text.replace("\xe2\x86\x99", "");  // ↙
  text.replace("\xe2\x86\x96", "");  // ↖

  // Temperature units
  text.replace("\xc2\xb0" "C", " degrees");  // °C
  text.replace("\xc2\xb0" "F", " degrees");  // °F

  // Other units
  text.replace("%", " percent");
  text.replace("km/h", " kilometers per hour");

  // Replace words eSpeak's minimal dictionary can't pronounce with phoneme codes
  applyPhonemes(text);

  // Strip any remaining non-ASCII characters eSpeak can't pronounce
  String clean;
  clean.reserve(text.length());
  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c >= 0x20 && c <= 0x7E) {  // printable ASCII only
      clean += c;
    } else if (c == '\n' || c == '\r') {
      clean += ' ';
    }
  }

  // Clean up double spaces
  while (clean.indexOf("  ") >= 0) {
    clean.replace("  ", " ");
  }

  return clean;
}

void initTTS() {
  // Register empty config file — eSpeak's LoadConfig() tries to open /mem/data/config
  // which doesn't exist in the in-memory PROGMEM filesystem, causing a harmless warning.
  espeak.add("/mem/data/config", "", 0);
  if (espeak.begin()) {
    espeak.setVoice("en");
    espeak.setRate(160);  // Default 175, range 80-450
    espeak.setFlags(espeakCHARS_AUTO | espeakPHONEMES);  // Enable inline [[ ]] phoneme codes
    Serial.println("eSpeak NG initialized");
  } else {
    Serial.println("ERROR: eSpeak NG init failed!");
  }
}

void sayText(const char* text) {
  String processed = sanitizeForTTS(String(text));
  Serial.printf("TTS: %s\n", processed.c_str());
  espeak.say(processed.c_str());
  ttsOut.flush();
}

void playTone(int frequency, int duration) {
  int totalSamples = (SAMPLE_RATE * duration) / 1000;
  int16_t buffer[256];
  int bufIndex = 0;

  int amplitude = (32767 * toneVolumePercent) / 100;
  for (int i = 0; i < totalSamples; i++) {
    float t = (float)i / SAMPLE_RATE;
    int16_t value = (int16_t)(amplitude * sin(2 * PI * frequency * t));
    buffer[bufIndex++] = value;

    if (bufIndex >= 256) {
      i2sWrite(buffer, bufIndex);
      bufIndex = 0;
    }
  }

  // Write remaining samples
  if (bufIndex > 0) {
    i2sWrite(buffer, bufIndex);
  }
}

void playVoiceMessage(const char* message) {
  sayText(message);
}

void speakPreMessage() {
  if (preMessage.length() > 0) {
    String expanded = expandMacros(preMessage);
    sayText(expanded.c_str());
  }
}

void speakPostMessage() {
  if (postMessage.length() > 0) {
    String expanded = expandMacros(postMessage);
    sayText(expanded.c_str());
  }
}
