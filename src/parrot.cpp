#include <Arduino.h>
#include <HardwareSerial.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "espeak.h"
#include <radio_test_audio.h>

// Pin definitions
#define PTT_PIN 33
#define PD_PIN 13
#define AUDIO_ON_PIN 4  // Our squelch detect!

#define SA868_TX 21   // goes to SA868 module's RX
#define SA868_RX 22   // goes to SA868 module's TX

// I2S pins (external codec in slave mode)
#define I2S_MCLK 0
#define I2S_SD_IN 14   // Audio from external device
#define I2S_LRCLK 27
#define I2S_BCLK 26
#define I2S_SD_OUT 25  // Audio to external device

#define I2S_PORT I2S_NUM_0

// Default pins (actual values loaded from Preferences):
// PTT=33, PD=13, AudioOn=4
// I2S: MCLK=0, BCLK=26, LRCLK=27, DIN=14, DOUT=25

// Serial for SA868
HardwareSerial SA868(2);  // UART2

// Recording parameters
#define SAMPLE_RATE 22050  // eSpeak NG native rate
#define MAX_RECORDING_SECONDS 10
#define MAX_SAMPLES (SAMPLE_RATE * MAX_RECORDING_SECONDS)
// Audio volumes are configurable via web interface (0-100%)
// Defaults: Voice=25%, Tone=12%

// WiFi and Weather defaults
#define AP_SSID "RadioParrot"
#define AP_PASSWORD "parrot123"
// Default location: Stone Mills, Ontario
#define DEFAULT_LAT 44.45f
#define DEFAULT_LON -76.88f

// WiFi state
bool apMode = false;
unsigned long wifiReadyTime = 0;  // millis() when WiFi settled, ignore AudioOn until then
#define WIFI_SETTLE_MS 5000       // Ignore squelch pin for this long after WiFi connects
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;
String wifiSSID;
String wifiPassword;

// Weather location (stored in Preferences)
float weatherLat;   // Latitude
float weatherLon;   // Longitude

// Radio settings (stored in Preferences)
String radioFreq;      // e.g. "451.0000"
String radioTxCTCSS;   // e.g. "0000" (none) or "0001"-"0038"
String radioRxCTCSS;   // e.g. "0000" (none) or "0001"-"0038"
int radioSquelch;      // 0-8

// Audio settings (stored in Preferences, 0-100%)
int samVolumePercent;    // TTS speech volume (kept as samvol for preferences compat)
int toneVolumePercent;   // Beep/tone volume

// Pin configuration (stored in Preferences)
int pinPTT;
int pinPD;
int pinAudioOn;
int pinI2S_MCLK;
int pinI2S_BCLK;
int pinI2S_LRCLK;
int pinI2S_DIN;
int pinI2S_DOUT;

// Testing mode - disables PTT
bool testingMode;

// Buffers (allocated in PSRAM)
int16_t* audioBuffer = nullptr;
int recordIndex = 0;
bool recording = false;

// TTS output buffer
int16_t ttsBuffer[512];
int ttsBufferIndex = 0;

// Signal quality tracking
int peakRSSI = 0;
int minRSSI = 999;
float peakAudioLevel = 0;
int clipCount = 0;  // Count of clipped samples
#define CLIP_THRESHOLD 32112  // 98% of 32768
#define CLIP_COUNT_WARN 100   // Need this many clipped samples to warn

// ==================== DTMF MAILBOX ====================
#define MAX_SLOTS 8  // DTMF 1-8 (9 slots won't fit in PSRAM with 10-sec recordings)
#define DTMF_BLOCK_SIZE 205   // ~9.3ms at 22050Hz, good for Goertzel

// Recording slots (allocated in PSRAM)
struct RecordingSlot {
  int16_t* buffer;
  int sampleCount;  // 0 = empty
};
RecordingSlot slots[MAX_SLOTS];
int nextSlot = 0;  // Circular index for auto-save

// DTMF detection state
char detectedDTMF = 0;  // '1'-'9' or 0 for none

// DTMF frequencies (Hz)
const float DTMF_FREQS[8] = {697, 770, 852, 941, 1209, 1336, 1477, 1633};
// Row/column mapping to digits
const char DTMF_CHARS[4][4] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// Goertzel coefficients (precomputed for SAMPLE_RATE)
float goertzelCoeff[8];

void initGoertzel() {
  for (int i = 0; i < 8; i++) {
    float k = (DTMF_BLOCK_SIZE * DTMF_FREQS[i]) / SAMPLE_RATE;
    goertzelCoeff[i] = 2.0f * cos(2.0f * PI * k / DTMF_BLOCK_SIZE);
  }
}

float goertzelMagnitude(int16_t* samples, int count, int freqIndex) {
  float s0 = 0, s1 = 0, s2 = 0;
  float coeff = goertzelCoeff[freqIndex];

  for (int i = 0; i < count; i++) {
    s0 = samples[i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }

  // Magnitude squared
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

char detectDTMF(int16_t* samples, int count) {
  float magnitudes[8];
  float maxRow = 0, maxCol = 0;
  int rowIdx = -1, colIdx = -1;

  // Calculate magnitudes for all 8 frequencies
  for (int i = 0; i < 8; i++) {
    magnitudes[i] = goertzelMagnitude(samples, count, i);
  }

  // Find strongest row frequency (0-3)
  for (int i = 0; i < 4; i++) {
    if (magnitudes[i] > maxRow) {
      maxRow = magnitudes[i];
      rowIdx = i;
    }
  }

  // Find strongest column frequency (4-7)
  for (int i = 4; i < 8; i++) {
    if (magnitudes[i] > maxCol) {
      maxCol = magnitudes[i];
      colIdx = i - 4;
    }
  }

  // Need both row and column to be significantly above noise
  // Threshold tuned for radio audio - increase if getting false positives
  float threshold = 1e12;  // Adjust based on testing
  if (maxRow > threshold && maxCol > threshold) {
    // Check that the two strongest are much stronger than others
    char digit = DTMF_CHARS[rowIdx][colIdx];
    Serial.printf("DTMF detected: %c (row=%d col=%d mag=%.0f/%.0f)\n",
                  digit, rowIdx, colIdx, maxRow, maxCol);
    return digit;
  }

  return 0;
}

void initSlots() {
  for (int i = 0; i < MAX_SLOTS; i++) {
    slots[i].buffer = (int16_t*)ps_malloc(MAX_SAMPLES * sizeof(int16_t));
    slots[i].sampleCount = 0;
    if (!slots[i].buffer) {
      Serial.printf("ERROR: Failed to allocate slot %d!\n", i + 1);
    }
  }
  Serial.printf("Allocated %d recording slots in PSRAM\n", MAX_SLOTS);
  Serial.printf("PSRAM remaining: %d bytes\n", ESP.getFreePsram());
}

void saveToSlot(int slotIndex) {
  if (slotIndex < 0 || slotIndex >= MAX_SLOTS) return;
  if (!slots[slotIndex].buffer) return;

  // Copy current recording to slot
  int copyCount = min(recordIndex, MAX_SAMPLES);
  memcpy(slots[slotIndex].buffer, audioBuffer, copyCount * sizeof(int16_t));
  slots[slotIndex].sampleCount = copyCount;
  Serial.printf("Saved %d samples to slot %d\n", copyCount, slotIndex + 1);
}

// Forward declarations
void sayText(const char* text);
void i2sWrite(int16_t* data, size_t samples);

// PTT helpers (respect testingMode)
void pttOn() {
  if (!testingMode) {
    digitalWrite(pinPTT, LOW);
  }
  Serial.println(testingMode ? "PTT ON (disabled - testing mode)" : "PTT ON");
}

void pttOff() {
  digitalWrite(pinPTT, HIGH);  // Always release PTT
  Serial.println("PTT OFF");
}

void playSlot(int slotIndex) {
  if (slotIndex < 0 || slotIndex >= MAX_SLOTS) return;

  // Key PTT first - need enough time for radio to key up
  pttOn();
  delay(600);

  if (slots[slotIndex].sampleCount == 0 || !slots[slotIndex].buffer) {
    Serial.printf("Slot %d is empty\n", slotIndex + 1);
    sayText("no recording");
  } else {
    Serial.printf("Playing slot %d (%d samples)\n", slotIndex + 1, slots[slotIndex].sampleCount);

    // Play back the slot
    for (int i = 0; i < slots[slotIndex].sampleCount; i += 256) {
      int chunkSize = min(256, slots[slotIndex].sampleCount - i);
      i2sWrite(&slots[slotIndex].buffer[i], chunkSize);
    }
  }

  delay(300);
  pttOff();
}

void playRadioTest() {
  // Key PTT first
  pttOn();
  delay(900);

  Serial.printf("Playing radio test audio (%d samples, %.1f sec)\n",
                RADIO_TEST_SAMPLES, (float)RADIO_TEST_SAMPLES / RADIO_TEST_SAMPLE_RATE);

  // Play embedded audio from PROGMEM
  int16_t buffer[256];
  for (int i = 0; i < RADIO_TEST_SAMPLES; i += 256) {
    int chunkSize = min(256, RADIO_TEST_SAMPLES - i);
    // Copy from PROGMEM to RAM buffer
    for (int j = 0; j < chunkSize; j++) {
      buffer[j] = pgm_read_word(&radioTestAudio[i + j]);
    }
    i2sWrite(buffer, chunkSize);
  }

  delay(300);
  pttOff();
  Serial.println("Radio test complete!");
}

// Text sanitization for TTS
String sanitizeForTTS(String text) {
  // Remove wind direction arrows
  text.replace("↑", "");
  text.replace("↓", "");
  text.replace("←", "");
  text.replace("→", "");
  text.replace("↗", "");
  text.replace("↘", "");
  text.replace("↙", "");
  text.replace("↖", "");

  // Temperature units
  text.replace("°C", " degrees");
  text.replace("°F", " degrees");

  // Other units
  text.replace("%", " percent");
  text.replace("km/h", " kilometers per hour");

  // Clean up double spaces
  while (text.indexOf("  ") >= 0) {
    text.replace("  ", " ");
  }

  return text;
}

// Convert number to spoken words (e.g., 23 -> "twenty three")
String numberToWords(int n) {
  if (n < 0) return "minus " + numberToWords(-n);
  if (n == 0) return "zero";

  const char* ones[] = {"", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
                        "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
                        "seventeen", "eighteen", "nineteen"};
  const char* tens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};

  if (n < 20) return ones[n];
  if (n < 100) {
    String result = tens[n / 10];
    if (n % 10 != 0) result += " " + String(ones[n % 10]);
    return result;
  }
  if (n < 1000) {
    String result = String(ones[n / 100]) + " hundred";
    if (n % 100 != 0) result += " " + numberToWords(n % 100);
    return result;
  }
  // For very large numbers, just read digits
  return String(n);
}

// Convert Open-Meteo weather code to description
String weatherCodeToText(int code) {
  if (code == 0) return "clear sky";
  if (code == 1) return "mainly clear";
  if (code == 2) return "partly cloudy";
  if (code == 3) return "overcast";
  if (code == 45 || code == 48) return "foggy";
  if (code >= 51 && code <= 55) return "drizzle";
  if (code >= 56 && code <= 57) return "freezing drizzle";
  if (code >= 61 && code <= 65) return "rain";
  if (code >= 66 && code <= 67) return "freezing rain";
  if (code >= 71 && code <= 75) return "snow";
  if (code == 77) return "snow grains";
  if (code >= 80 && code <= 82) return "rain showers";
  if (code >= 85 && code <= 86) return "snow showers";
  if (code == 95) return "thunderstorm";
  if (code >= 96 && code <= 99) return "thunderstorm with hail";
  return "unknown conditions";
}

// Extract the "current" section from Open-Meteo JSON
String extractCurrentSection(const String& json) {
  int idx = json.indexOf("\"current\":{");
  if (idx < 0) return "";
  idx += 10;  // Move past "current":{
  int depth = 1;
  int endIdx = idx;
  while (endIdx < json.length() && depth > 0) {
    if (json[endIdx] == '{') depth++;
    if (json[endIdx] == '}') depth--;
    endIdx++;
  }
  return json.substring(idx, endIdx);
}

// Extract a float value from JSON like "temperature_2m":-4.2
float extractJsonFloat(const String& json, const String& key) {
  String searchKey = "\"" + key + "\":";
  int idx = json.indexOf(searchKey);
  if (idx < 0) return -999;
  idx += searchKey.length();
  int endIdx = idx;
  while (endIdx < json.length() && (isDigit(json[endIdx]) || json[endIdx] == '-' || json[endIdx] == '.')) {
    endIdx++;
  }
  return json.substring(idx, endIdx).toFloat();
}

// Extract an int value from JSON like "weather_code":71
int extractJsonInt(const String& json, const String& key) {
  String searchKey = "\"" + key + "\":";
  int idx = json.indexOf(searchKey);
  if (idx < 0) return -999;
  idx += searchKey.length();
  int endIdx = idx;
  while (endIdx < json.length() && (isDigit(json[endIdx]) || json[endIdx] == '-')) {
    endIdx++;
  }
  return json.substring(idx, endIdx).toInt();
}

void speakWeather() {
  String report;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Weather: WiFi not connected");
    report = "no wifi";
  } else {
    Serial.println("Fetching weather...");

    // Build weather URL with stored coordinates
    String weatherURL = "http://api.open-meteo.com/v1/forecast?latitude=" + String(weatherLat, 4) +
                        "&longitude=" + String(weatherLon, 4) +
                        "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m&temperature_unit=celsius&wind_speed_unit=kmh";
    Serial.printf("Weather URL: %s\n", weatherURL.c_str());

    HTTPClient http;
    http.begin(weatherURL.c_str());
    http.setTimeout(10000);

    int httpCode = http.GET();

    if (httpCode == 200) {
      String json = http.getString();
      Serial.printf("Weather raw: %s\n", json.c_str());

      // Extract the "current" section (skips "current_units" which has string values)
      String current = extractCurrentSection(json);
      Serial.printf("Current section: %s\n", current.c_str());

      // Parse Open-Meteo JSON response
      float temp = extractJsonFloat(current, "temperature_2m");
      float feelsLike = extractJsonFloat(current, "apparent_temperature");
      int humidity = (int)extractJsonFloat(current, "relative_humidity_2m");
      float wind = extractJsonFloat(current, "wind_speed_10m");
      int weatherCode = extractJsonInt(current, "weather_code");

      // Build spoken weather report with natural number pronunciation
      report = weatherCodeToText(weatherCode);
      report += ", " + numberToWords((int)round(temp)) + " degrees";
      report += ", feels like " + numberToWords((int)round(feelsLike)) + " degrees";
      report += ", humidity " + numberToWords(humidity) + " percent";
      report += ", winds " + numberToWords((int)round(wind)) + " K P H";
    } else {
      Serial.printf("Weather fetch failed: %d\n", httpCode);
      report = "weather unavailable";
    }

    http.end();
  }

  Serial.printf("Weather report: %s\n", report.c_str());
  pttOn();
  delay(600);
  sayText(report.c_str());
  delay(1000);
  pttOff();
}

// Web server handlers
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><title>Radio Parrot</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;margin:20px;max-width:400px;}";
  html += "input,select{margin:5px 0;padding:8px;width:100%;box-sizing:border-box;}";
  html += ".status{padding:10px;margin:10px 0;border-radius:5px;}";
  html += ".connected{background:#d4edda;}.disconnected{background:#f8d7da;}";
  html += ".btn{background:#007bff;color:white;border:none;padding:10px;cursor:pointer;margin:5px 0;}";
  html += ".coords{display:flex;gap:10px;}.coords input{width:48%;}</style></head>";
  html += "<body><h1>Radio Parrot</h1>";

  // Status
  html += "<div class='status ";
  html += (WiFi.status() == WL_CONNECTED) ? "connected'>Connected to: " + wifiSSID : "disconnected'>Not connected (AP Mode)";
  html += "</div>";

  // WiFi form
  html += "<h2>WiFi Settings</h2>";
  html += "<form action='/save' method='POST'>";
  html += "<label>SSID:</label><input name='ssid' value='" + wifiSSID + "'>";
  html += "<label>Password:</label><input name='pass' type='password' placeholder='Enter new password'>";

  // Weather location
  html += "<h2>Weather Location</h2>";
  html += "<div class='coords'>";
  html += "<input name='lat' id='lat' type='text' placeholder='Latitude' value='" + String(weatherLat, 4) + "'>";
  html += "<input name='lon' id='lon' type='text' placeholder='Longitude' value='" + String(weatherLon, 4) + "'>";
  html += "</div>";
  html += "<button type='button' class='btn' onclick='detectLocation()'>Detect My Location</button>";
  html += "<div id='locStatus'></div>";

  // Radio settings
  html += "<h2>Radio Settings</h2>";
  html += "<label>Frequency (MHz):</label><input name='freq' value='" + radioFreq + "' placeholder='451.0000'>";
  html += "<label>TX CTCSS (0000=none):</label><input name='txctcss' value='" + radioTxCTCSS + "' placeholder='0000'>";
  html += "<label>RX CTCSS (0000=none):</label><input name='rxctcss' value='" + radioRxCTCSS + "' placeholder='0000'>";
  html += "<label>Squelch (0-8):</label><input name='squelch' type='number' min='0' max='8' value='" + String(radioSquelch) + "'>";

  // Audio settings
  html += "<h2>Audio Settings</h2>";
  html += "<label>Voice Volume (0-100%):</label><input name='samvol' type='number' min='0' max='100' value='" + String(samVolumePercent) + "'>";
  html += "<label>Tone Volume (0-100%):</label><input name='tonevol' type='number' min='0' max='100' value='" + String(toneVolumePercent) + "'>";

  // Testing mode
  html += "<h2>Mode</h2>";
  html += "<label><input type='checkbox' name='testmode' value='1'" + String(testingMode ? " checked" : "") + "> Testing Mode (PTT disabled)</label>";

  html += "<br><br><input type='submit' value='Save & Reboot'>";
  html += "</form>";

  // Link to pins page
  html += "<p><a href='/pins'>Configure Pins</a></p>";

  // JavaScript for location detection
  html += "<script>";
  html += "function detectLocation(){";
  html += "document.getElementById('locStatus').innerHTML='Detecting...';";
  html += "fetch('http://ip-api.com/json/?fields=lat,lon,city,country')";
  html += ".then(r=>r.json()).then(d=>{";
  html += "document.getElementById('lat').value=d.lat.toFixed(4);";
  html += "document.getElementById('lon').value=d.lon.toFixed(4);";
  html += "document.getElementById('locStatus').innerHTML='Found: '+d.city+', '+d.country;";
  html += "}).catch(e=>{document.getElementById('locStatus').innerHTML='Detection failed';});";
  html += "}";
  html += "</script>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  String newSSID = server.arg("ssid");
  String newPass = server.arg("pass");
  String newLat = server.arg("lat");
  String newLon = server.arg("lon");
  String newFreq = server.arg("freq");
  String newTxCTCSS = server.arg("txctcss");
  String newRxCTCSS = server.arg("rxctcss");
  String newSquelch = server.arg("squelch");
  String newSamVol = server.arg("samvol");
  String newToneVol = server.arg("tonevol");
  bool newTestMode = server.hasArg("testmode");

  preferences.begin("parrot", false);

  if (newSSID.length() > 0) {
    preferences.putString("ssid", newSSID);
  }
  if (newPass.length() > 0) {
    preferences.putString("password", newPass);
  }
  if (newLat.length() > 0) {
    preferences.putFloat("lat", newLat.toFloat());
  }
  if (newLon.length() > 0) {
    preferences.putFloat("lon", newLon.toFloat());
  }
  if (newFreq.length() > 0) {
    preferences.putString("freq", newFreq);
  }
  if (newTxCTCSS.length() > 0) {
    preferences.putString("txctcss", newTxCTCSS);
  }
  if (newRxCTCSS.length() > 0) {
    preferences.putString("rxctcss", newRxCTCSS);
  }
  if (newSquelch.length() > 0) {
    preferences.putInt("squelch", newSquelch.toInt());
  }
  if (newSamVol.length() > 0) {
    preferences.putInt("samvol", constrain(newSamVol.toInt(), 0, 100));
  }
  if (newToneVol.length() > 0) {
    preferences.putInt("tonevol", constrain(newToneVol.toInt(), 0, 100));
  }
  preferences.putBool("testmode", newTestMode);

  preferences.end();

  String html = "<!DOCTYPE html><html><head><title>Saved</title>";
  html += "<meta http-equiv='refresh' content='3;url=/'></head>";
  html += "<body><h1>Settings Saved!</h1><p>Rebooting...</p></body></html>";
  server.send(200, "text/html", html);

  delay(1000);
  ESP.restart();
}

void handleStatus() {
  String json = "{";
  json += "\"wifi\":\"" + String((WiFi.status() == WL_CONNECTED) ? "connected" : "disconnected") + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"ssid\":\"" + wifiSSID + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"ap_mode\":" + String(apMode ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handlePins() {
  String html = "<!DOCTYPE html><html><head><title>Pin Configuration</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;margin:20px;max-width:400px;}";
  html += "input{margin:5px 0;padding:8px;width:80px;}</style></head>";
  html += "<body><h1>Pin Configuration</h1>";
  html += "<p><strong>Warning:</strong> Incorrect pin settings can prevent boot. Only change if you know your hardware.</p>";
  html += "<form action='/savepins' method='POST'>";

  html += "<h2>Control Pins</h2>";
  html += "<label>PTT Pin:</label><input name='ptt' type='number' value='" + String(pinPTT) + "'><br>";
  html += "<label>Power Down Pin:</label><input name='pd' type='number' value='" + String(pinPD) + "'><br>";
  html += "<label>Audio/Squelch Pin:</label><input name='audioon' type='number' value='" + String(pinAudioOn) + "'><br>";

  html += "<h2>I2S Pins</h2>";
  html += "<label>MCLK:</label><input name='mclk' type='number' value='" + String(pinI2S_MCLK) + "'><br>";
  html += "<label>BCLK:</label><input name='bclk' type='number' value='" + String(pinI2S_BCLK) + "'><br>";
  html += "<label>LRCLK:</label><input name='lrclk' type='number' value='" + String(pinI2S_LRCLK) + "'><br>";
  html += "<label>Data In:</label><input name='din' type='number' value='" + String(pinI2S_DIN) + "'><br>";
  html += "<label>Data Out:</label><input name='dout' type='number' value='" + String(pinI2S_DOUT) + "'><br>";

  html += "<br><input type='submit' value='Save & Reboot'>";
  html += "</form>";
  html += "<p><a href='/'>Back to Main</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSavePins() {
  preferences.begin("parrot", false);

  if (server.hasArg("ptt")) preferences.putInt("pinPTT", server.arg("ptt").toInt());
  if (server.hasArg("pd")) preferences.putInt("pinPD", server.arg("pd").toInt());
  if (server.hasArg("audioon")) preferences.putInt("pinAudioOn", server.arg("audioon").toInt());
  if (server.hasArg("mclk")) preferences.putInt("pinMCLK", server.arg("mclk").toInt());
  if (server.hasArg("bclk")) preferences.putInt("pinBCLK", server.arg("bclk").toInt());
  if (server.hasArg("lrclk")) preferences.putInt("pinLRCLK", server.arg("lrclk").toInt());
  if (server.hasArg("din")) preferences.putInt("pinDIN", server.arg("din").toInt());
  if (server.hasArg("dout")) preferences.putInt("pinDOUT", server.arg("dout").toInt());

  preferences.end();

  String html = "<!DOCTYPE html><html><head><title>Saved</title>";
  html += "<meta http-equiv='refresh' content='3;url=/pins'></head>";
  html += "<body><h1>Pin Settings Saved!</h1><p>Rebooting...</p></body></html>";
  server.send(200, "text/html", html);

  delay(1000);
  ESP.restart();
}

void initWiFi() {
  // Load settings from Preferences
  preferences.begin("parrot", true);  // read-only
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  weatherLat = preferences.getFloat("lat", DEFAULT_LAT);
  weatherLon = preferences.getFloat("lon", DEFAULT_LON);
  radioFreq = preferences.getString("freq", "451.0000");
  radioTxCTCSS = preferences.getString("txctcss", "0000");
  radioRxCTCSS = preferences.getString("rxctcss", "0000");
  radioSquelch = preferences.getInt("squelch", 4);

  // Audio settings
  samVolumePercent = preferences.getInt("samvol", 25);
  toneVolumePercent = preferences.getInt("tonevol", 12);

  // Pin configuration
  pinPTT = preferences.getInt("pinPTT", 33);
  pinPD = preferences.getInt("pinPD", 13);
  pinAudioOn = preferences.getInt("pinAudioOn", 4);
  pinI2S_MCLK = preferences.getInt("pinMCLK", 0);
  pinI2S_BCLK = preferences.getInt("pinBCLK", 26);
  pinI2S_LRCLK = preferences.getInt("pinLRCLK", 27);
  pinI2S_DIN = preferences.getInt("pinDIN", 14);
  pinI2S_DOUT = preferences.getInt("pinDOUT", 25);

  // Testing mode (default ON for safety)
  testingMode = preferences.getBool("testmode", true);

  preferences.end();

  Serial.printf("Loaded SSID: %s\n", wifiSSID.c_str());
  Serial.printf("Weather location: %.4f, %.4f\n", weatherLat, weatherLon);

  // If no SSID configured, go straight to AP mode
  if (wifiSSID.length() == 0) {
    Serial.println("No WiFi configured, starting AP mode...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    dnsServer.start(53, "*", WiFi.softAPIP());  // Captive portal DNS
    Serial.printf("AP started: %s (password: %s)\n", AP_SSID, AP_PASSWORD);
    Serial.printf("Connect and visit http://%s\n", WiFi.softAPIP().toString().c_str());
    apMode = true;
  } else {
    // Try to connect
    Serial.printf("WiFi connecting to %s...\n", wifiSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

    // Wait up to 10 seconds for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi connected! IP: http://%s\n", WiFi.localIP().toString().c_str());
      apMode = false;
      wifiReadyTime = millis() + WIFI_SETTLE_MS;
    } else {
      // Connection failed, start AP mode
      Serial.println("WiFi connection failed, starting AP mode...");
      WiFi.mode(WIFI_AP);
      WiFi.softAP(AP_SSID, AP_PASSWORD);
      dnsServer.start(53, "*", WiFi.softAPIP());  // Captive portal DNS
      Serial.printf("AP started: %s (password: %s)\n", AP_SSID, AP_PASSWORD);
      Serial.printf("Connect and visit http://%s\n", WiFi.softAPIP().toString().c_str());
      apMode = true;
    }
  }

  // Minimize WiFi RF interference with radio
  WiFi.setTxPower(WIFI_POWER_MINUS_1dBm);
  WiFi.setSleep(true);  // Modem sleep between beacons
  Serial.println("WiFi TX power set to minimum, modem sleep enabled");

  // Start web server in either mode
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/pins", handlePins);
  server.on("/savepins", HTTP_POST, handleSavePins);
  server.on("/status", handleStatus);

  // Captive portal - redirect all unknown URLs to root
  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println("Web server started on port 80");
}

void initI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // Mono
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .mck_io_num = pinI2S_MCLK,
    .bck_io_num = pinI2S_BCLK,
    .ws_io_num = pinI2S_LRCLK,
    .data_out_num = pinI2S_DOUT,
    .data_in_num = pinI2S_DIN
  };

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("I2S driver install failed: %d\n", err);
    return;
  }

  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("I2S set pins failed: %d\n", err);
    return;
  }

  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("I2S initialized");
}

void printSA868Response() {
  while (SA868.available()) {
    String response = SA868.readStringUntil('\n');
    Serial.println("SA868: " + response);
  }
}

void initializeSA868() {
  Serial.println("Initializing SA868...");

  // Handshake
  SA868.println("AT+DMOCONNECT");
  delay(500);
  printSA868Response();

  // Set frequency from stored settings (simplex mode: TX=RX)
  String cmd = "AT+DMOSETGROUP=0," + radioFreq + "," + radioFreq + "," + radioTxCTCSS + "," + String(radioSquelch) + "," + radioRxCTCSS;
  Serial.printf("Radio config: %s\n", cmd.c_str());
  SA868.println(cmd);
  delay(500);
  printSA868Response();

  // Set volume to 8 (max)
  SA868.println("AT+DMOSETVOLUME=8");
  delay(500);
  printSA868Response();

  // Set filter (all on - pre-emph, highpass, lowpass)
  SA868.println("AT+SETFILTER=0,0,0");
  delay(500);
  printSA868Response();

  Serial.println("SA868 initialized!");
}

int getRSSI() {
  // Clear buffer
  while (SA868.available()) SA868.read();

  // Send RSSI query (note: no "AT+" prefix according to datasheet)
  SA868.println("RSSI?");

  // Wait for response
  delay(100);

  String response = "";
  while (SA868.available()) {
    char c = SA868.read();
    if (c == '\n' || c == '\r') break;
    response += c;
  }

  // Parse "RSSI=120"
  if (response.startsWith("RSSI=")) {
    int rssi = response.substring(5).toInt();
    return rssi;
  }

  return 0;
}

bool isReceiving() {
  // Ignore squelch pin in AP mode or while WiFi is settling (RF noise causes false triggers)
  if (apMode || millis() < wifiReadyTime) return false;
  // Audio ON pin goes LOW when receiving
  return digitalRead(pinAudioOn) == LOW;
}

void startRecording() {
  recording = true;
  recordIndex = 0;
  peakRSSI = 0;
  minRSSI = 999;
  peakAudioLevel = 0;
  clipCount = 0;
  detectedDTMF = 0;  // Reset DTMF detection

  Serial.println("Recording started...");
}

void stopRecording() {
  recording = false;
  Serial.printf("Recording stopped. %d samples captured.\n", recordIndex);
  Serial.printf("RSSI: min=%d, peak=%d\n", minRSSI, peakRSSI);
  Serial.printf("Audio: peak=%.1f, clipped samples=%d\n", peakAudioLevel, clipCount);
}

void recordAudioSamples() {
  // Read a chunk of samples from I2S
  int16_t samples[256];
  size_t bytesRead = 0;

  esp_err_t err = i2s_read(I2S_PORT, samples, sizeof(samples), &bytesRead, 0);
  if (err != ESP_OK || bytesRead == 0) return;

  int samplesRead = bytesRead / sizeof(int16_t);

  for (int i = 0; i < samplesRead && recordIndex < MAX_SAMPLES; i++) {
    int16_t sample = samples[i];
    audioBuffer[recordIndex++] = sample;

    // Track peak level
    float level = abs(sample) / 32768.0;
    if (level > peakAudioLevel) {
      peakAudioLevel = level;
    }

    // Detect clipping
    if (abs(sample) > CLIP_THRESHOLD) {
      clipCount++;
    }
  }

  // DTMF detection - check periodically during recording
  // Only detect once (first DTMF wins)
  static int dtmfCheckCounter = 0;
  dtmfCheckCounter += samplesRead;
  if (detectedDTMF == 0 && dtmfCheckCounter >= DTMF_BLOCK_SIZE && recordIndex >= DTMF_BLOCK_SIZE) {
    dtmfCheckCounter = 0;
    // Check the most recent samples for DTMF
    int startIdx = max(0, recordIndex - DTMF_BLOCK_SIZE);
    char dtmf = detectDTMF(&audioBuffer[startIdx], DTMF_BLOCK_SIZE);
    if ((dtmf >= '1' && dtmf <= '9') || dtmf == '*') {
      detectedDTMF = dtmf;
      Serial.printf("*** DTMF %c detected ***\n", dtmf);
    }
  }

  // Debug: print progress
  static int lastPrint = 0;
  if (recordIndex / 1000 > lastPrint) {
    lastPrint = recordIndex / 1000;
    Serial.printf("Recording: %d samples\n", recordIndex);
  }
}

void i2sWrite(int16_t* data, size_t samples) {
  size_t bytesWritten = 0;
  i2s_write(I2S_PORT, data, samples * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
}

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

TTSOutput ttsOut;
ESpeak espeak(ttsOut);

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

void generateQualityFeedback() {
  if (peakRSSI > 140) {
    playTone(1200, 200);
    playVoiceMessage("excellent signal");
  } else if (peakRSSI > 120) {
    playTone(1000, 200);
    delay(100);
    playTone(1000, 200);
    playVoiceMessage("good signal");
  } else if (peakRSSI > 100) {
    playTone(800, 200);
    delay(100);
    playTone(800, 200);
    delay(100);
    playTone(800, 200);
    playVoiceMessage("fair signal");
  } else if (peakRSSI > 0) {
    playTone(400, 500);
    playVoiceMessage("weak signal, check antenna");
  } else {
    playTone(300, 300);
    delay(100);
    playTone(300, 300);
    playVoiceMessage("no signal");
  }

  if (clipCount > CLIP_COUNT_WARN) {
    delay(300);
    playVoiceMessage("audio clipping, reduce volume");
  }
}

void playbackWithFeedback() {
  Serial.println("Starting playback...");

  // Key PTT
  pttOn();
  delay(300);  // PTT tail delay

  // Play back recorded audio via I2S
  for (int i = 0; i < recordIndex; i += 256) {
    int chunkSize = min(256, recordIndex - i);
    i2sWrite(&audioBuffer[i], chunkSize);
  }

  delay(500);  // Gap before feedback tones

  // Generate quality feedback
  generateQualityFeedback();

  delay(300);  // Final tail

  // Release PTT
  pttOff();

  Serial.println("Playback complete!");
}

void setup() {
  // Set PTT HIGH immediately to prevent TX during boot
  // Note: Using default pin here since preferences not loaded yet
  pinMode(33, OUTPUT);  // Default PTT pin
  digitalWrite(33, HIGH);  // RX mode

  Serial.begin(115200);
  SA868.begin(9600, SERIAL_8N1, SA868_TX, SA868_RX);

  // Pin setup
  pinMode(PD_PIN, OUTPUT);
  pinMode(AUDIO_ON_PIN, INPUT);
  digitalWrite(PD_PIN, HIGH);   // Normal operation (not power down)

  Serial.println("ESP32 Radio Parrot Starting...");

  // Initialize WiFi and load preferences
  initWiFi();

  // Pin setup (using loaded preferences)
  pinMode(pinPTT, OUTPUT);
  digitalWrite(pinPTT, HIGH);  // Ensure RX mode
  pinMode(pinPD, OUTPUT);
  pinMode(pinAudioOn, INPUT);
  digitalWrite(pinPD, HIGH);   // Normal operation (not power down)

  Serial.printf("Pins: PTT=%d, PD=%d, AudioOn=%d\n", pinPTT, pinPD, pinAudioOn);
  Serial.printf("I2S: MCLK=%d, BCLK=%d, LRCLK=%d, DIN=%d, DOUT=%d\n",
                pinI2S_MCLK, pinI2S_BCLK, pinI2S_LRCLK, pinI2S_DIN, pinI2S_DOUT);
  Serial.printf("Testing mode: %s\n", testingMode ? "ON" : "OFF");

  // Allocate audio buffer in PSRAM
  if (psramFound()) {
    audioBuffer = (int16_t*)ps_malloc(MAX_SAMPLES * sizeof(int16_t));
    Serial.printf("PSRAM: %d bytes free, audio buffer allocated\n", ESP.getFreePsram());

    // Initialize recording slots
    initSlots();

    // Initialize Goertzel coefficients for DTMF detection
    initGoertzel();
  } else {
    audioBuffer = (int16_t*)malloc(MAX_SAMPLES * sizeof(int16_t));
    Serial.println("Warning: PSRAM not found, using internal RAM (no DTMF mailbox)");
  }

  if (!audioBuffer) {
    Serial.println("ERROR: Failed to allocate audio buffer!");
    while (1) delay(1000);
  }

  // Initialize I2S
  initI2S();

  // Initialize SA868
  delay(500);
  while (SA868.available()) SA868.read();  // Clear receive buffer
  initializeSA868();

  // Initialize eSpeak NG speech synthesis
  if (espeak.begin()) {
    espeak.setVoice("en");
    espeak.setRate(160);  // Default 175, range 80-450
    Serial.println("eSpeak NG initialized");
  } else {
    Serial.println("ERROR: eSpeak NG init failed!");
  }

  // Ignore squelch pin for 10 seconds after boot (RF noise during startup)
  wifiReadyTime = max(wifiReadyTime, millis() + 10000);

  Serial.println("Ready for radio checks!");
}

void loop() {
  // Handle web server requests
  server.handleClient();

  // Handle DNS for captive portal (AP mode only)
  if (apMode) {
    dnsServer.processNextRequest();
  }

  static bool wasReceiving = false;
  static unsigned long recordStartTime = 0;
  static unsigned long lastRSSISample = 0;

  bool nowReceiving = isReceiving();

  // Track RSSI periodically during reception
  if (nowReceiving && millis() - lastRSSISample > 100) {
    int rssi = getRSSI();
    if (rssi > peakRSSI) peakRSSI = rssi;
    if (rssi < minRSSI && rssi > 0) minRSSI = rssi;
    lastRSSISample = millis();
  }

  // Detect start of transmission
  if (nowReceiving && !wasReceiving) {
    startRecording();
    recordStartTime = millis();
  }

  // Record audio samples via I2S
  if (recording && nowReceiving) {
    recordAudioSamples();
  }

  // Detect end of transmission
  if (!nowReceiving && wasReceiving && recording) {
    stopRecording();
    delay(2000);

    if (detectedDTMF == '*') {
      // DTMF * - speak weather (handles PTT and speech internally)
      speakWeather();
    } else if (detectedDTMF == '9') {
      // DTMF 9 - play embedded radio test audio
      playRadioTest();
    } else if (detectedDTMF >= '1' && detectedDTMF <= '8') {
      // DTMF 1-8 - play back requested slot
      int slotIndex = detectedDTMF - '1';  // '1' -> slot 0, '8' -> slot 7
      playSlot(slotIndex);
    } else {
      // Normal parrot mode - save and playback
      saveToSlot(nextSlot);
      nextSlot = (nextSlot + 1) % MAX_SLOTS;
      playbackWithFeedback();
    }
  }

  // Timeout safety
  if (recording && (millis() - recordStartTime > 10000)) {
    Serial.println("Recording timeout!");
    stopRecording();
    delay(2000);

    if (detectedDTMF == '*') {
      speakWeather();
    } else if (detectedDTMF == '9') {
      playRadioTest();
    } else if (detectedDTMF >= '1' && detectedDTMF <= '8') {
      int slotIndex = detectedDTMF - '1';
      playSlot(slotIndex);
    } else {
      saveToSlot(nextSlot);
      nextSlot = (nextSlot + 1) % MAX_SLOTS;
      playbackWithFeedback();
    }
  }

  wasReceiving = nowReceiving;
}
