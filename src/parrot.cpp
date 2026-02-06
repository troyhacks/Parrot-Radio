#include <Arduino.h>
#include <HardwareSerial.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "rtc.h"
#include "tts.h"
#include "weather.h"
#include "radio.h"
#include "web.h"

// ==================== Global State Definitions ====================
// (declared extern in config.h)

// WiFi state
bool apMode = false;
unsigned long wifiReadyTime = 0;
String wifiSSID;
String wifiPassword;

// Weather location
float weatherLat;
float weatherLon;

// Radio settings
String radioFreq;
String radioTxCTCSS;
String radioRxCTCSS;
int radioSquelch;

// Audio settings
int samVolumePercent;
int toneVolumePercent;

// Pin configuration (runtime)
int pinPTT;
int pinPD;
int pinAudioOn;
int pinI2S_MCLK;
int pinI2S_BCLK;
int pinI2S_LRCLK;
int pinI2S_DIN;
int pinI2S_DOUT;
int pinVBAT;

// Testing mode
bool testingMode;

// DTMF # message
String dtmfHashMessage;

// RTC state
String timezonePosix;
bool rtcFound = false;
bool ntpSynced = false;

// Pre/post messages
String preMessage;
String postMessage;

// Battery reading
float lastBatteryV = 0;
int lastBatteryPct = -1;

// Recording buffers
int16_t* audioBuffer = nullptr;
int recordIndex = 0;
bool recording = false;

// Signal quality tracking
int peakRSSI = 0;
int minRSSI = 999;
float peakAudioLevel = 0;
int clipCount = 0;

// Recording slots
RecordingSlot slots[MAX_SLOTS];
int nextSlot = 0;

// DTMF detection
char detectedDTMF = 0;

// ==================== Hardware Objects ====================

HardwareSerial SA868(2);  // UART2
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

// ==================== Main Setup ====================

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

  // Initialize RTC first (TZ is still UTC, so mktime reads DS3231 correctly)
  initRTC();
  // Now apply timezone and sync NTP
  applyTimezone();
  if (!apMode) {
    syncNTP();
  }

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
  initTTS();

  // Ignore squelch pin for 5 seconds after boot (RF noise during startup)
  wifiReadyTime = max(wifiReadyTime, millis() + 5000);
  while (wifiReadyTime > millis()) vTaskDelay(1);
  Serial.println("Ready for radio checks!");
}

// ==================== Main Loop ====================

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

    // Ignore squelch pops and no-signal recordings
    if (recordIndex < MIN_RECORDING_SAMPLES || peakAudioLevel < MIN_AUDIO_LEVEL) {
      Serial.printf("Ignoring short/empty recording (%d samples, peak=%.3f)\n",
                     recordIndex, peakAudioLevel);
      wasReceiving = nowReceiving;
      return;
    }

    delay(2000);

    if (detectedDTMF == '#' && dtmfHashMessage.length() > 0) {
      // DTMF # - speak configurable message with macro expansion
      String expanded = expandMacros(dtmfHashMessage);
      pttOn();
      delay(600);
      sayText(expanded.c_str());
      delay(1000);
      pttOff();
    } else if (detectedDTMF == '*') {
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

    // Ignore squelch pops and no-signal recordings
    if (recordIndex < MIN_RECORDING_SAMPLES || peakAudioLevel < MIN_AUDIO_LEVEL) {
      Serial.printf("Ignoring short/empty recording (%d samples, peak=%.3f)\n",
                     recordIndex, peakAudioLevel);
      wasReceiving = nowReceiving;
      return;
    }

    delay(2000);

    if (detectedDTMF == '#' && dtmfHashMessage.length() > 0) {
      String expanded = expandMacros(dtmfHashMessage);
      pttOn();
      delay(600);
      sayText(expanded.c_str());
      delay(1000);
      pttOff();
    } else if (detectedDTMF == '*') {
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

  // Battery voltage check (only when idle, disabled if pinVBAT == -1)
  static unsigned long lastBattCheck = 0;
  if (pinVBAT >= 0 && !recording && !nowReceiving && millis() - lastBattCheck > VBAT_CHECK_INTERVAL) {
    lastBattCheck = millis();
    long sum = 0;
    for (int i = 0; i < 10; i++) {
      sum += analogReadMilliVolts(pinVBAT);
      delay(5);
    }
    float voltage = (sum / 10) / 1000.0 * VBAT_DIVIDER;
    if (voltage > VBAT_LIPO_MIN && voltage < VBAT_LIPO_MAX) {
      int percent = constrain((int)((voltage - VBAT_LIPO_MIN) / (4.2 - VBAT_LIPO_MIN) * 100), 0, 100);
      lastBatteryV = voltage;
      lastBatteryPct = percent;
      Serial.printf("Battery: %.2fV (%d%%)\n", voltage, percent);
    }
  }

  wasReceiving = nowReceiving;
}
