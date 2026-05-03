#include <Arduino.h>
#include <HardwareSerial.h>
#include <esp_heap_caps.h>
#include <Wire.h>
#include <time.h>

#include "config.h"
#include "rtc.h"
#include "tts.h"
#include "weather.h"
#include "radio.h"
#include "web.h"
#include "display.h"
#include "gps.h"
#include "sun.h"
#include "weather_sensor.h"

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
int radioBandwidth25;     // SA868 bandwidth: 1=25kHz, 0=12.5kHz

// Audio settings
int samVolumePercent;
int toneVolumePercent;
int playbackVolumePercent;
int radioTestVolumePercent;  // Gain for radio test audio (0-100)

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

// DTMF A reboot setting
bool dtmfARebootEnabled = true;

// DTMF # message
String dtmfHashMessage;

// RTC state
String timezonePosix;
bool rtcFound = false;
bool ntpSynced = false;
long gmtOffsetSeconds = 0;  // UTC offset in seconds, computed after NTP sync

// Pre/post messages
String preMessage;
String postMessage;

// Battery reading
float lastBatteryV = 0;
int lastBatteryPct = -1;

// AXP2101 power state
bool extPowerConnected = false;
bool batteryCharging = false;

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

// ==================== Web Server Task ====================
void webServerTask(void *pvParameters) {
  for (;;) {
    server.handleClient();
    if (apMode) {
      dnsServer.processNextRequest();
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // Yield to prevent watchdog panics
  }
}

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

  // Initialize GPS
  initGPS();

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

  // Initialize Wire/I2C for PMU (OLED shares the same I2C bus)
#ifdef BOARD_TTWR
  Wire.begin(PMU_SDA, PMU_SCL);
  Wire.setClock(400000);
  Serial.println("Wire initialized");

  // Initialize BME280/BMP280 weather sensor
  initWeatherSensor();
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
  } else {
    // Cap to 3 seconds to fit in internal RAM (~132 KB) and avoid OOM crash
    int safeInternalSamples = SAMPLE_RATE * 3;
    audioBuffer = (int16_t*)malloc(safeInternalSamples * sizeof(int16_t));
    Serial.println("WARNING: PSRAM not found. Capping recording buffer to 3 seconds.");
  }

  if (!audioBuffer) {
    Serial.println("ERROR: Failed to allocate audio buffer!");
    while (1) delay(1000);
  }

  // Initialize audio hardware first (enables DC1 which powers OLED)
  initAudioHardware();
  initAudioInput();

  // Give OLED time to power up after DC1 enable
  delay(500);

  // Now initialize OLED display (after OLED has power)
  initDisplay();
  displayDebug("Starting...");
  delay(200);

  // Prime weather cache on boot (after OLED is ready)
  if (apMode) {
    displaySetWeather("AP Mode");
  } else if (weatherLat == 0 && weatherLon == 0) {
    displaySetWeather("Set location in web");
  } else {
    displayDebug("Fetching weather...");
    String weatherStr = fetchWeatherReport();
    displaySetWeather(getWeatherDisplayString().c_str());
    Serial.printf("Initial weather: %s\n", weatherStr.c_str());
  }

  // Initialize Goertzel coefficients using actual ADC sample rate
#ifdef BOARD_TTWR
  float actualSampleRate = adcSampleRate;  // Already measured as calSamples in calibration
#else
  float actualSampleRate = SAMPLE_RATE;
#endif
  initGoertzel(actualSampleRate);
  displayDebug("Goertzel...");
  delay(100);

  // Initialize SA868
  delay(500);
  while (SA868.available()) SA868.read();  // Clear receive buffer
  initializeSA868();
  displayDebug("Radio OK");
  delay(100);

  // Set frequency and CTCSS display from radio settings
  char freqBuf[16];
  snprintf(freqBuf, sizeof(freqBuf), "%s MHz", radioFreq.c_str());
  displaySetChannel(freqBuf);
  displaySetCTCSS(radioTxCTCSS.c_str(), radioRxCTCSS.c_str());
  delay(100);

  // Initialize eSpeak NG speech synthesis
  initTTS();
  displayDebug("TTS OK");
  delay(100);

  // Ignore squelch pin for 5 seconds after boot (RF noise during startup)
  wifiReadyTime = max(wifiReadyTime, millis() + 5000);
  while (wifiReadyTime > millis()) vTaskDelay(1);
  displayDebug("Ready!");
  Serial.println("Ready for radio checks!");

  // Start web server on Core 0
  xTaskCreatePinnedToCore(webServerTask, "WebServer", 8192, NULL, 1, NULL, 0);
}

// ==================== Main Loop ====================

void loop() {
  // Update GPS data
  updateGPS();
  updateTimezoneFromGPS();  // Keep timezone updated from GPS coordinates
  displaySetGPS(gpsHasFix() ? "GPS OK" : "GPS ..");

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
    displaySetState(DisplayState::Recording);
    displayShowState();  // Show full display with IP/time
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
      displaySetState(DisplayState::TTS);
      displaySetStateText("TTS MSG");
      displayShowState();  // Show full display with IP/time
      // DTMF # - speak configurable message with macro expansion
      String expanded = expandMacros(dtmfHashMessage);
      pttOn();
      delay(600);
      setAudioRoutingToRadio(true);
      setSpeakerMute(true);
      sayText(expanded.c_str());
      waitForTTSDone();
      setSpeakerMute(false);
      setAudioRoutingToRadio(false);
      pttOff();
    } else if (detectedDTMF == '*') {
      displaySetState(DisplayState::Weather);
      displayShowState();
      // DTMF * - speak weather (handles PTT and speech internally)
      speakWeather();
    } else if (detectedDTMF == '9') {
      displaySetState(DisplayState::TTS);
      displaySetStateText("PLAY TEST");
      displayShowState();
      // DTMF 9 - play embedded radio test audio
      playRadioTest();
    } else if (detectedDTMF >= '1' && detectedDTMF <= '8') {
      displaySetState(DisplayState::Prerecord);
      char stateText[16];
      snprintf(stateText, sizeof(stateText), "PLAY SLOT %d", detectedDTMF - '0');
      displaySetStateText(stateText);
      displayShowState();
      playSlot(detectedDTMF - '1');
    } else if (detectedDTMF == 'A') {
      // DTMF A - speak sunrise/sunset times
      displaySetState(DisplayState::TTS);
      displaySetStateText("SUN");
      displayShowState();
      pttOn();
      delay(600);
      setAudioRoutingToRadio(true);
      setSpeakerMute(true);
      {
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        SolarTimes st = calculateSunTimes(weatherLat, weatherLon, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
        char buf[512];
        if (st.valid) {
          String nextGH = getNextGoldenHourWords();
          if (nextGH.length() > 0) {
            snprintf(buf, sizeof(buf),
              "Sunrise at %s, sunset at %s. %s.",
              getSunriseWords().c_str(), getSunsetWords().c_str(), nextGH.c_str());
          } else {
            snprintf(buf, sizeof(buf),
              "Sunrise at %s, sunset at %s. No golden hour today.",
              getSunriseWords().c_str(), getSunsetWords().c_str());
          }
        } else {
          snprintf(buf, sizeof(buf), "Unable to calculate sun times for current location");
        }
        sayText(buf);
      }
      waitForTTSDone();
      setSpeakerMute(false);
      setAudioRoutingToRadio(false);
      pttOff();
    } else if (detectedDTMF == 'B') {
      // DTMF B - speak current date
      displaySetState(DisplayState::TTS);
      displaySetStateText("DATE");
      displayShowState();
      pttOn();
      delay(600);
      setAudioRoutingToRadio(true);
      setSpeakerMute(true);
      {
        char dateBuf[64];
        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        strftime(dateBuf, sizeof(dateBuf), "Today is %A, %B %d, %Y", tm_info);
        sayText(dateBuf);
      }
      waitForTTSDone();
      setSpeakerMute(false);
      setAudioRoutingToRadio(false);
      pttOff();
    } else if (detectedDTMF == 'C') {
      // DTMF C - speak current time
      displaySetState(DisplayState::TTS);
      displaySetStateText("TIME");
      displayShowState();
      pttOn();
      delay(600);
      setAudioRoutingToRadio(true);
      setSpeakerMute(true);
      {
        // Simple number-to-words for time
        static const char* ones[] = {"zero", "one", "two", "three", "four", "five ", "six", "seven", "eight", "nine",
                                     "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
        static const char* tens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
        auto numToWords = [&](int n) -> String {
          if (n == 0) return "zero";
          if (n < 20) return String(ones[n]);
          if (n < 60) {
            String s = tens[n / 10];
            if (n % 10) {
              // Handle 50-59: use "five X" instead of "fifty X" for espeak
              if (n >= 50 && n < 60) {
                s = "five ";
                s += ones[n % 10];
              } else {
                s += " ";
                s += ones[n % 10];
              }
            }
            return s;
          }
          return String(n);
        };

        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        int h = tm_info->tm_hour;
        int m = tm_info->tm_min;
        int h12 = h % 12;
        if (h12 == 0) h12 = 12;
        String timeWords = "The current time is ";
        if (m == 0) {
          timeWords += numToWords(h12);
        } else if (m < 10) {
          timeWords += numToWords(h12) + " oh " + numToWords(m);
        } else {
          timeWords += numToWords(h12) + " " + numToWords(m);
        }
        timeWords += h < 12 ? " AM" : " PM";
        sayText(timeWords.c_str());
      }
      waitForTTSDone();
      setSpeakerMute(false);
      setAudioRoutingToRadio(false);
      pttOff();
    } else if (detectedDTMF == 'D') {
      // DTMF D - reboot if enabled
      if (dtmfARebootEnabled) {
        displaySetStateText("REBOOT");
        displayShowState();
        delay(500);
        ESP.restart();
      } else {
        displaySetState(DisplayState::TTS);
        displaySetStateText("BATTERY");
        displayShowState();
        pttOn();
        delay(600);
        setAudioRoutingToRadio(true);
        setSpeakerMute(true);
        if (lastBatteryPct >= 0) {
          String msg = "Battery is " + String(lastBatteryPct) + " percent";
          if (extPowerConnected) {
            msg += ", external power connected";
            if (batteryCharging) msg += ", charging";
          }
          sayText(msg.c_str());
        } else {
          sayText("Battery status unavailable");
        }
        waitForTTSDone();
        setSpeakerMute(false);
        setAudioRoutingToRadio(false);
        pttOff();
      }
    } else {
      // Normal parrot mode - save and playback
      int playedSlot = nextSlot;
      saveToSlot(nextSlot);
      nextSlot = (nextSlot + 1) % MAX_SLOTS;
      displaySetState(DisplayState::Playing);
      char stateText[16];
      snprintf(stateText, sizeof(stateText), "PLAYING %d", playedSlot + 1);
      displaySetStateText(stateText);
      displayShowState();
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
      displaySetState(DisplayState::TTS);
      displaySetStateText("TTS MSG");
      displayShowState();
      String expanded = expandMacros(dtmfHashMessage);
      pttOn();
      delay(600);
      setAudioRoutingToRadio(true);
      setSpeakerMute(true);
      sayText(expanded.c_str());
      waitForTTSDone();
      setSpeakerMute(false);
      setAudioRoutingToRadio(false);
      pttOff();
    } else if (detectedDTMF == '*') {
      displaySetState(DisplayState::Weather);
      displayShowState();
      speakWeather();
    } else if (detectedDTMF == '9') {
      displaySetState(DisplayState::TTS);
      displaySetStateText("PLAY TEST");
      displayShowState();
      playRadioTest();
    } else if (detectedDTMF >= '1' && detectedDTMF <= '8') {
      displaySetState(DisplayState::Prerecord);
      char stateText[16];
      snprintf(stateText, sizeof(stateText), "PLAY SLOT %d", detectedDTMF - '0');
      displaySetStateText(stateText);
      displayShowState();
      playSlot(detectedDTMF - '1');
    } else if (detectedDTMF == 'A') {
      // DTMF A - speak sunrise/sunset times
      displaySetState(DisplayState::TTS);
      displaySetStateText("SUN");
      displayShowState();
      pttOn();
      delay(600);
      setAudioRoutingToRadio(true);
      setSpeakerMute(true);
      {
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        SolarTimes st = calculateSunTimes(weatherLat, weatherLon, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
        char buf[512];
        if (st.valid) {
          String nextGH = getNextGoldenHourWords();
          if (nextGH.length() > 0) {
            snprintf(buf, sizeof(buf),
              "Sunrise at %s, sunset at %s. %s.",
              getSunriseWords().c_str(), getSunsetWords().c_str(), nextGH.c_str());
          } else {
            snprintf(buf, sizeof(buf),
              "Sunrise at %s, sunset at %s. No golden hour today.",
              getSunriseWords().c_str(), getSunsetWords().c_str());
          }
        } else {
          snprintf(buf, sizeof(buf), "Unable to calculate sun times for current location");
        }
        sayText(buf);
      }
      waitForTTSDone();
      setSpeakerMute(false);
      setAudioRoutingToRadio(false);
      pttOff();
    } else if (detectedDTMF == 'B') {
      displaySetState(DisplayState::TTS);
      displaySetStateText("DATE");
      displayShowState();
      pttOn();
      delay(600);
      setAudioRoutingToRadio(true);
      setSpeakerMute(true);
      {
        char dateBuf[64];
        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        strftime(dateBuf, sizeof(dateBuf), "Today is %A, %B %d, %Y", tm_info);
        sayText(dateBuf);
      }
      waitForTTSDone();
      setSpeakerMute(false);
      setAudioRoutingToRadio(false);
      pttOff();
    } else if (detectedDTMF == 'C') {
      displaySetState(DisplayState::TTS);
      displaySetStateText("TIME");
      displayShowState();
      pttOn();
      delay(600);
      setAudioRoutingToRadio(true);
      setSpeakerMute(true);
      {
        char timeBuf[64];
        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        strftime(timeBuf, sizeof(timeBuf), "The current time is %I:%M %p", tm_info);
        sayText(timeBuf);
      }
      waitForTTSDone();
      setSpeakerMute(false);
      setAudioRoutingToRadio(false);
      pttOff();
    } else if (detectedDTMF == 'D') {
      if (dtmfARebootEnabled) {
        displaySetStateText("REBOOT");
        displayShowState();
        delay(500);
        ESP.restart();
      } else {
        displaySetState(DisplayState::TTS);
        displaySetStateText("BATTERY");
        displayShowState();
        pttOn();
        delay(600);
        setAudioRoutingToRadio(true);
        setSpeakerMute(true);
        if (lastBatteryPct >= 0) {
          String msg = "Battery is " + String(lastBatteryPct) + " percent";
          if (extPowerConnected) {
            msg += ", external power connected";
            if (batteryCharging) msg += ", charging";
          }
          sayText(msg.c_str());
        } else {
          sayText("Battery status unavailable");
        }
        waitForTTSDone();
        setSpeakerMute(false);
        setAudioRoutingToRadio(false);
        pttOff();
      }
    } else {
      int playedSlot = nextSlot;
      saveToSlot(nextSlot);
      nextSlot = (nextSlot + 1) % MAX_SLOTS;
      displaySetState(DisplayState::Playing);
      char stateText[16];
      snprintf(stateText, sizeof(stateText), "PLAYING %d", playedSlot + 1);
      displaySetStateText(stateText);
      displayShowState();
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

  // Update display when idle
  if (!recording && !nowReceiving) {
    DisplayState prevState = displayGetState();
    displaySetState(DisplayState::Idle);
    displaySetAction(NULL);  // Clear action to show freq/CTCSS instead
    // Immediately show full idle display with IP/time when returning from active state
    if (prevState != DisplayState::Idle) {
      char timeStr[32];
      char ipStr[32];
      struct tm t;
      if (getLocalTime(&t, 0)) {
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
      } else {
        snprintf(timeStr, sizeof(timeStr), "N/A");
      }
      if (apMode) {
        snprintf(ipStr, sizeof(ipStr), "AP: %s", WiFi.softAPIP().toString().c_str());
      } else {
        snprintf(ipStr, sizeof(ipStr), "%s", WiFi.localIP().toString().c_str());
      }
      updateDisplay(ipStr, timeStr, DisplayState::Idle, lastKnownRSSI, -1);
    }
  }

  // Refresh OLED display periodically (updateDisplay handles IP/time)
  // Only update when idle to avoid overwriting state messages during playback
  static unsigned long lastDisplayUpdate = 0;
  DisplayState state = displayGetState();
  if (!recording && !nowReceiving && state == DisplayState::Idle &&
      millis() - lastDisplayUpdate > 5000) {
    lastDisplayUpdate = millis();

    char timeStr[32];
    char ipStr[32];
    struct tm t;
    if (getLocalTime(&t, 0)) {
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    } else {
      snprintf(timeStr, sizeof(timeStr), "N/A");
    }
    if (apMode) {
      snprintf(ipStr, sizeof(ipStr), "AP: %s", WiFi.softAPIP().toString().c_str());
    } else {
      snprintf(ipStr, sizeof(ipStr), "%s", WiFi.localIP().toString().c_str());
    }

    DisplayState state = displayGetState();
    updateDisplay(ipStr, timeStr, state, lastKnownRSSI, -1);

    // Refresh weather cache if needed (fetchWeatherReport handles 15min cache internally)
    if (!apMode && weatherLat != 0 && weatherLon != 0) {
      fetchWeatherReport();
      displaySetWeather(getWeatherDisplayString().c_str());
    }

    // Update sun times if we have GPS or manual coordinates
    if (weatherLat != 0 && weatherLon != 0) {
      time_t now = time(nullptr);
      struct tm* t = localtime(&now);
      calculateSunTimes(weatherLat, weatherLon, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    }

    // Update local weather sensor reading
    if (weatherSensorFound()) {
      BME280Data data;
      readWeatherSensor(&data);
    }

    // Update AXP2101 battery/power state
    updatePowerState();
  }
}
