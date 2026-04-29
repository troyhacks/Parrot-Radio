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
int radioVolume;          // SA868 volume (0-8)
int radioFilterBP;        // AT+SETFILTER bandpass: 0=off, 1=on
int radioFilterDENoise;   // AT+SETFILTER de-noise: 0=off, 1=on
int radioFilterDER;       // AT+SETFILTER de-emphasis: 0=off, 1=on

// Audio settings
int samVolumePercent;
int toneVolumePercent;
int playbackVolumePercent;

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
  Serial.begin(115200);
  delay(500);
  Serial.println("Setup starting...");

  // Set PTT HIGH immediately to prevent TX during boot
#ifdef BOARD_TTWR
  pinMode(41, OUTPUT);  // T-TWR default PTT pin
  digitalWrite(41, HIGH);  // RX mode
  Serial.println("PTT pin set");
#else
  pinMode(33, OUTPUT);  // Original board default PTT pin
  digitalWrite(33, HIGH);  // RX mode
#endif

  // Pin setup - do this BEFORE SA868 communication
  pinMode(PD_PIN, OUTPUT);
  pinMode(AUDIO_ON_PIN, INPUT_PULLUP);  // SA868 squelch is open-collector, needs pull-up
  digitalWrite(PD_PIN, HIGH);   // Power on SA868
  delay(200);  // Give SA868 time to power up
  Serial.printf("PD_PIN (GPIO%d) set HIGH\n", PD_PIN);
  Serial.println("Pin modes set, SA868 powered on");

  // Try 9600 baud for SA868 communication
  // Explicitly set pin mapping for UART2 on ESP32-S3
  // UART2 TX=GPIO39 (SA868_RX), UART2 RX=GPIO48 (SA868_TX)
  // Note: TX of ESP32 connects to RX of SA868 and vice versa
  SA868.begin(9600, SERIAL_8N1, SA868_RX, SA868_TX);
  Serial.println("SA868 serial begun at 9600 baud");

  Serial.println("ESP32 Radio Parrot Starting...");

  // Initialize WiFi and load preferences
  initWiFi();
  Serial.println("WiFi initialized");

  // Initialize RTC first (TZ is still UTC, so mktime reads DS3231 correctly)
  initRTC();
  Serial.println("RTC initialized");

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
#ifdef BOARD_TTWR
  Serial.println("Board: T-TWR");
#else
  Serial.println("Board: Original ESP32-WROVER-KIT");
#endif
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

  // Initialize audio hardware (board-specific: I2S or ADC/LEDC)
  initAudioHardware();
  initAudioInput();

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
    if (rssi > 0) lastKnownRSSI = rssi;  // Cache for squelch fallback
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
