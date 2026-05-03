#include "tts.h"
#include "config.h"
#include <WiFi.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <esp_task_wdt.h>
#include "espeak.h"
#include "radio.h"
#include "weather_sensor.h"
#include "gps.h"
#include "sun.h"

// TTS task configuration
#define TTS_TASK_STACK_SIZE (64 * 1024)  // 64KB stack in BYTES (ESP32 IDF uses bytes, not words)
static TaskHandle_t s_ttsTaskHandle = nullptr;
static QueueHandle_t s_ttsQueue = nullptr;

// TTS completion synchronization
static EventGroupHandle_t s_ttsEvents = nullptr;
#define TTS_DONE_BIT (1 << 0)

// TTS output buffer
static int16_t ttsBuffer[512];
static int ttsBufferIndex = 0;

// eSpeak audio output — Print subclass that feeds our audio output
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
        // Pure integer math to prevent FPU overhead
        sample = (int16_t)(((int32_t)sample * samVolumePercent) / 100);
        ttsBuffer[ttsBufferIndex++] = sample;
        i += 2;

        if (ttsBufferIndex >= 512) {
          audioWrite(ttsBuffer, ttsBufferIndex); // i2sWrite naturally blocks if full
          ttsBufferIndex = 0;
          esp_task_wdt_reset(); // Feed watchdog without causing DMA stutter
        }
      } else {
        i++;  // Odd trailing byte, skip
      }
    }
    return size;
  }

  void flush() {
    if (ttsBufferIndex > 0) {
      audioWrite(ttsBuffer, ttsBufferIndex);
      ttsBufferIndex = 0;
    }
  }
};

static TTSOutput ttsOut;
static ESpeak espeak(ttsOut);

// ==================== Helper Functions ====================
// Convert integer to English words (avoids espeak deep recursion)
// Note: "five" has trailing space to help espeak pronounce it correctly
static const char* s_ones[] = {"zero", "one", "two", "three", "four", "five ", "six", "seven", "eight", "nine",
                                "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
static const char* s_tens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};

static String intToWords(int n) {
  if (n == 0) return "zero";
  String s;
  if (n < 0) { s = "minus "; n = -n; }
  if (n >= 100) { s += s_ones[n/100]; s += " hundred "; n %= 100; }
  if (n >= 20) {
    // Handle 50-59 specially: use "five X" instead of "fifty X" to avoid espeak pronunciation issues
    if (n >= 50 && n < 60) {
      s += "five ";
      s += s_ones[n % 10];
    } else {
      s += s_tens[n/10];
      if (n % 10) { s += " "; s += s_ones[n % 10]; }
    }
  }
  else if (n >= 1) s += s_ones[n];
  return s;
}

// Format hour:minute as English words to avoid espeak number→words translation
// e.g. 3:33 → "three thirty three PM", 6:08 → "six oh eight AM"
static String formatTimeHM(int hour, int minute) {
  if (hour < 0 || minute < 0) return "unknown";
  String s;
  int h12 = hour % 12;
  if (h12 == 0) h12 = 12;
  s = intToWords(h12);
  // intToWords handles 0-59 correctly including teens (15="fifteen" not "ten five")
  if (minute == 0) {
    // Exact hour — say nothing more, e.g. "six AM"
  } else if (minute < 10) {
    s += " oh " + intToWords(minute);  // e.g. "six oh eight"
  } else {
    s += " " + intToWords(minute);     // e.g. "three thirty three"
  }
  s += hour < 12 ? " AM" : " PM";
  return s;
}

// Format decimals like "43.65" as "forty three point six five"
static String formatDecimalString(const String& val) {
  int dotIndex = val.indexOf('.');
  if (dotIndex == -1) return intToWords(val.toInt());
  int intPart = val.substring(0, dotIndex).toInt();
  String out = intToWords(intPart) + " point";
  for (size_t i = dotIndex + 1; i < val.length(); i++) {
    if (val[i] >= '0' && val[i] <= '9') {
      out += " " + intToWords(val[i] - '0');
    }
  }
  return out;
}

static String formatFloatToWords(float value, int decimalPlaces) {
  return formatDecimalString(String(value, decimalPlaces));
}

// Format date as spoken: "May third, twenty twenty-six"
static String formatDateSpoken(int year, int month, int day) {
  static const char* months[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
  };
  static const char* ordinals[] = {
    "first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth", "ninth", "tenth",
    "eleventh", "twelfth", "thirteenth", "fourteenth", "fifteenth", "sixteenth", "seventeenth", "eighteenth", "nineteenth", "twentieth",
    "twenty-first", "twenty-second", "twenty-third", "twenty-fourth", "twenty-fifth", "twenty-sixth", "twenty-seventh", "twenty-eighth", "twenty-ninth", "thirtieth",
    "thirty-first"
  };

  String s;
  if (month >= 1 && month <= 12) {
    s += months[month - 1];
  } else {
    s += "unknown";
  }
  s += " ";

  if (day >= 1 && day <= 31) {
    s += ordinals[day - 1];
  } else {
    s += "unknown";
  }
  s += ", ";

  // Year: 2026 → "twenty twenty-six"
  if (year >= 0) {
    if (year >= 2000) {
      int y = year - 2000;
      if (y < 100) {
        s += intToWords(y);
      } else {
        s += intToWords(year);
      }
    } else if (year >= 1000) {
      s += intToWords(year);
    } else {
      s += intToWords(year);
    }
  } else {
    s += "unknown";
  }

  return s;
}

// Format IP address for TTS — first 3 octets as spaced digits, last octet as words (0-99) or digits (100+)
// e.g. "192.168.1.46" → "1 9 2 dot 1 6 8 dot 1 dot forty six"
static String formatIPWords(const String& ip) {
  String out;
  int octetValues[4] = {0, 0, 0, 0};
  int octetCount = 0;
  int currentOctet = 0;

  for (size_t i = 0; i < ip.length(); i++) {
    if (ip[i] == '.') {
      octetValues[octetCount++] = currentOctet;
      currentOctet = 0;
    } else {
      currentOctet = currentOctet * 10 + (ip[i] - '0');
    }
  }
  octetValues[octetCount] = currentOctet;  // last octet

  for (int i = 0; i <= octetCount; i++) {
    if (i > 0) out += " dot ";
    if (i < 3) {
      // First 3 octets: spaced digits
      String num = String(octetValues[i]);
      for (size_t j = 0; j < num.length(); j++) {
        out += num[j];
        out += " ";
      }
    } else {
      // Last octet: words for 0-99, digits for 100+
      int last = octetValues[i];
      if (last <= 99) {
        out += intToWords(last);
      } else {
        String num = String(last);
        for (size_t j = 0; j < num.length(); j++) {
          out += num[j];
          out += " ";
        }
      }
    }
  }
  return out;
}

// ==================== Macro Expansion ====================
// Expands {tokens} in message strings with live values
String expandMacros(const String &text) {
  String result = text;
  // Date/time macros
  struct tm t;
  if (getLocalTime(&t, 0)) {
    char buf[64];  // Larger buffer to prevent overflow
    result.replace("{date}", formatDateSpoken(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday));
    strftime(buf, sizeof(buf), "%H:%M", &t);
    result.replace("{time}", buf);
    result.replace("{time12}", formatTimeHM(t.tm_hour, t.tm_min));
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
    result.replace("{battery}", intToWords(lastBatteryPct) + " percent");
    result.replace("{voltage}", formatFloatToWords(lastBatteryV, 1) + " volts");
  } else {
    result.replace("{battery}", "unknown");
    result.replace("{voltage}", "unknown");
  }
  // Slot macros
  result.replace("{slot}", intToWords(nextSlot + 1));
  int usedSlots = 0;
  for (int i = 0; i < MAX_SLOTS; i++) {
    if (slots[i].sampleCount > 0) usedSlots++;
  }
  result.replace("{slots_used}", intToWords(usedSlots));
  result.replace("{slots_total}", intToWords(MAX_SLOTS));
  // Radio/system macros
  result.replace("{freq}", formatDecimalString(radioFreq));
  result.replace("{uptime}", intToWords(millis() / 60000) + " minutes");
  result.replace("{ip}", formatIPWords(WiFi.localIP().toString()));
  // Local weather sensor macros (BME280/BMP280)
  if (localWeather.valid) {
    result.replace("{localtemp}", intToWords((int)round(localWeather.temperature)) + " degrees");
    result.replace("{localhumidity}", intToWords((int)round(localWeather.humidity)) + " percent");
    result.replace("{localpressure}", intToWords((int)round(localWeather.pressure / 10.0f)) + " hectopascals");
  } else {
    result.replace("{localtemp}", "sensor unavailable");
    result.replace("{localhumidity}", "sensor unavailable");
    result.replace("{localpressure}", "sensor unavailable");
  }

  // Sun times (calculated from weatherLat/weatherLon)
  {
    time_t now = time(nullptr);
    struct tm* lt = localtime(&now);
    SolarTimes st = calculateSunTimes(weatherLat, weatherLon, lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
    if (st.valid) {
      result.replace("{sunrise}", formatTimeHM(st.sunriseHour, st.sunriseMinute));
      result.replace("{sunset}", formatTimeHM(st.sunsetHour, st.sunsetMinute));
      result.replace("{civil_dawn}", formatTimeHM(st.sunriseCivilHour, st.sunriseCivilMinute));
      result.replace("{civil_dusk}", formatTimeHM(st.sunsetCivilHour, st.sunsetCivilMinute));
      result.replace("{nautical_dawn}", formatTimeHM(st.sunriseNauticalHour, st.sunriseNauticalMinute));
      result.replace("{nautical_dusk}", formatTimeHM(st.sunsetNauticalHour, st.sunsetNauticalMinute));
      result.replace("{astronomical_dawn}", formatTimeHM(st.sunriseAstronomicalHour, st.sunriseAstronomicalMinute));
      result.replace("{astronomical_dusk}", formatTimeHM(st.sunsetAstronomicalHour, st.sunsetAstronomicalMinute));
      result.replace("{golden_hour_morning}", formatTimeHM(st.goldenHourMorningStartHour, st.goldenHourMorningStartMinute));
      result.replace("{golden_hour_evening}", formatTimeHM(st.goldenHourEveningEndHour, st.goldenHourEveningEndMinute));
    } else {
      result.replace("{sunrise}", "unknown");
      result.replace("{sunset}", "unknown");
      result.replace("{civil_dawn}", "unknown");
      result.replace("{civil_dusk}", "unknown");
      result.replace("{nautical_dawn}", "unknown");
      result.replace("{nautical_dusk}", "unknown");
      result.replace("{astronomical_dawn}", "unknown");
      result.replace("{astronomical_dusk}", "unknown");
      result.replace("{golden_hour_morning}", "unknown");
      result.replace("{golden_hour_evening}", "unknown");
    }
  }

  // Next golden hour (morning or evening, whichever is next)
  {
    String gh = getNextGoldenHourWords();
    result.replace("{next_golden_hour}", gh.length() > 0 ? gh : "no golden hour today");
  }

  // GPS coordinates
  if (gpsData.valid) {
    result.replace("{gps_lat}", formatFloatToWords(gpsData.latitude, 4));
    result.replace("{gps_lon}", formatFloatToWords(gpsData.longitude, 4));
  } else {
    result.replace("{gps_lat}", "no fix");
    result.replace("{gps_lon}", "no fix");
  }

  // Timezone
  result.replace("{timezone}", timezonePosix.length() > 0 ? timezonePosix : "UTC");

  return result;
}

// Phoneme pronunciations — DISABLED
// espeak's number→words dictionary lookup can recursively traverse phoneme entries
// causing stack overflow on the loopTask stack even for simple text.
// All phoneme processing is disabled. Custom pronunciations should be implemented
// by moving TTS to a dedicated FreeRTOS task with a larger stack (e.g., 64KB).
static void applyPhonemes(String& /*text*/) {}

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

  // Replace "listening" with "monitoring" — "ten" inside "listening" triggers
  // espeak's number→words lookup, causing garbled pronunciation
  text.replace("listening", "monitoring");

  // Replace words eSpeak's minimal dictionary can't pronounce with phoneme codes
  // DISABLED — causes stack overflow in espeak's number→words translation
  // applyPhonemes(text);

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

// Forward declaration for TTS task
static void ttsTaskFn(void* param);

void initTTS() {
  // Register empty config file — eSpeak's LoadConfig() tries to open /mem/data/config
  // which doesn't exist in the in-memory PROGMEM filesystem, causing a harmless warning.
  espeak.add("/mem/data/config", "", 0);
  if (espeak.begin()) {
    espeak.setVoice("en-us");  // Use US English voice for clarity
    espeak.setRate(160);  // Default 175, range 80-450
    espeak.setFlags(espeakCHARS_AUTO);  // No phoneme codes needed
    Serial.println("eSpeak NG initialized");
  } else {
    Serial.println("ERROR: eSpeak NG init failed!");
  }

  // Create TTS queue and start TTS task with 64KB stack
  s_ttsQueue = xQueueCreate(2, sizeof(char[512]));
  if (s_ttsQueue == nullptr) {
    Serial.println("ERROR: TTS queue creation failed!");
    return;
  }

  s_ttsEvents = xEventGroupCreate();
  if (s_ttsEvents == nullptr) {
    Serial.println("ERROR: TTS events creation failed!");
    return;
  }
  BaseType_t created = xTaskCreatePinnedToCore(
    ttsTaskFn,
    "tts",
    TTS_TASK_STACK_SIZE,  // bytes — ESP32 IDF xTaskCreatePinnedToCore uses bytes, not words
    nullptr,
    configMAX_PRIORITIES - 2,
    &s_ttsTaskHandle,
    0
  );
  if (created != pdPASS) {
    Serial.println("ERROR: TTS task creation failed!");
  } else {
    Serial.println("TTS task started");
  }
}


// TTS task — runs with 64KB stack to handle espeak's deep recursion
static void ttsTaskFn(void* param) {
  for (;;) {
    char textBuf[512];
    if (xQueueReceive(s_ttsQueue, textBuf, portMAX_DELAY) == pdTRUE) {
      String processed = sanitizeForTTS(String(textBuf));
      Serial.printf("TTS: %s\n", processed.c_str());
      espeak.say(processed.c_str());
      ttsOut.flush();
      // Yield after espeak returns to prevent watchdog
      vTaskDelay(1);
      // Signal TTS completion so callers can wait before PTT off
      if (s_ttsEvents) {
        xEventGroupSetBits(s_ttsEvents, TTS_DONE_BIT);
      }
    }
  }
}

void sayText(const char* text) {
  if (s_ttsQueue == nullptr) return;
  // Send to TTS task queue (wait up to 100ms if queue full)
  char textBuf[512];
  strncpy(textBuf, text, sizeof(textBuf) - 1);
  textBuf[sizeof(textBuf) - 1] = '\0';
  xQueueSend(s_ttsQueue, textBuf, pdMS_TO_TICKS(100));
}

void waitForTTSDone() {
  if (s_ttsEvents == nullptr) return;
  // Wait for TTS task to signal completion
  xEventGroupClearBits(s_ttsEvents, TTS_DONE_BIT);
  xEventGroupWaitBits(s_ttsEvents, TTS_DONE_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
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
      audioWrite(buffer, bufIndex);
      bufIndex = 0;
    }
  }

  // Write remaining samples
  if (bufIndex > 0) {
    audioWrite(buffer, bufIndex);
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
