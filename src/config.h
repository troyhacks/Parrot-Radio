#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ==================== Pin Definitions ====================

#ifdef BOARD_TTWR
// T-TWR Plus Rev 2.1 pin definitions
#define PTT_PIN 41
#define PD_PIN 40
#define AUDIO_ON_PIN 2   // Squelch detect

// SA868 UART (on T-TWR)
#define SA868_TX 39
#define SA868_RX 48

// T-TWR audio paths
#define ESP2MIC_PIN 18      // Audio output from ESP32 to SA868
#define MIC_CH_SEL_PIN 17   // Analog switch: HIGH=ESP32 audio, LOW=physical mic
#define RADIO_AUDIO_PIN 1   // Audio input from SA868 to ESP32 (ADC)

// AXP2101 PMU I2C for speaker mute control
#define PMU_SDA 8
#define PMU_SCL 9
#define PMU_IRQ 4

// GPS module pins (T-TWR has GPS)
#define GPS_TX 6    // ESP32 TX to GPS RX
#define GPS_RX 5    // ESP32 RX from GPS TX
#define GPS_PPS 7   // GPS Pulse Per Second (timing)

// Note: T-TWR uses internal ADC for audio input, LEDC for audio output
// I2S is not used for audio on T-TWR

#else
// Original ESP32-WROVER-KIT pin definitions
// Control pins (defaults, configurable via web)
#define PTT_PIN 33
#define PD_PIN 13
#define AUDIO_ON_PIN 4  // Squelch detect

// SA868 UART
#define SA868_TX 21   // goes to SA868 module's RX
#define SA868_RX 22   // goes to SA868 module's TX

// DS3231 RTC (I2C)
#define RTC_SDA 19
#define RTC_SCL 23
#define DS3231_ADDR 0x68

// I2S pins (external codec in slave mode)
#define I2S_MCLK 0
#define I2S_SD_IN 14   // Audio from external device
#define I2S_LRCLK 27
#define I2S_BCLK 26
#define I2S_SD_OUT 25  // Audio to external device

#define I2S_PORT I2S_NUM_0

#endif // BOARD_TTWR

// Battery voltage monitoring (ESP-WROVER-KIT: 100K/100K divider on IO35)
#define VBAT_PIN 35
#define VBAT_DIVIDER 2.0      // 100K/100K voltage divider
#define VBAT_LIPO_MIN 3.0     // LiPo minimum voltage
#define VBAT_LIPO_MAX 4.3     // Slightly above full charge to detect presence
#define VBAT_CHECK_INTERVAL 30000  // Check every 30 seconds

// ==================== Audio Settings ====================

#define SAMPLE_RATE 22050  // eSpeak NG native rate
#define MAX_RECORDING_SECONDS 10
#define MAX_SAMPLES (SAMPLE_RATE * MAX_RECORDING_SECONDS)

// Minimum recording thresholds (ignore squelch pops / no-signal)
#define MIN_RECORDING_SAMPLES (SAMPLE_RATE / 2)  // 0.5 seconds
#define MIN_AUDIO_LEVEL 0.02f                     // Peak level below this = no signal

// Signal quality thresholds
#define CLIP_THRESHOLD 32112  // 98% of 32768
#define CLIP_COUNT_WARN 100   // Need this many clipped samples to warn

// ==================== DTMF Settings ====================

#define MAX_SLOTS 8  // DTMF 1-8 (9 slots won't fit in PSRAM with 10-sec recordings)
#define DTMF_BLOCK_SIZE 205   // ~9.3ms at 22050Hz, good for Goertzel

// ==================== WiFi Settings ====================

#define AP_SSID "RadioParrot"
#define AP_PASSWORD "parrot123"
#define WIFI_SETTLE_MS 5000  // Ignore squelch pin for this long after WiFi connects

// Default location: Stone Mills, Ontario, Canada
#define DEFAULT_LAT 44.45f
#define DEFAULT_LON -76.88f

// Weather cache duration
#define WEATHER_CACHE_MS 900000  // 15 minutes

// ==================== Global State ====================
// (extern declarations - defined in parrot.cpp)

// WiFi state
extern bool apMode;
extern unsigned long wifiReadyTime;
extern String wifiSSID;
extern String wifiPassword;

// Weather location
extern float weatherLat;
extern float weatherLon;

// Radio settings
extern String radioFreq;
extern String radioTxCTCSS;
extern String radioRxCTCSS;
extern int radioSquelch;
extern int radioVolume;          // SA868 volume (0-8)
extern int radioFilterBP;        // AT+SETFILTER bandpass: 0=off, 1=on
extern int radioFilterDENoise;   // AT+SETFILTER de-noise: 0=off, 1=on
extern int radioFilterDER;       // AT+SETFILTER de-emphasis: 0=off, 1=on

// Audio settings
extern int samVolumePercent;
extern int toneVolumePercent;
extern int playbackVolumePercent;  // Gain for recorded audio playback (0-100+)

// Pin configuration (runtime)
extern int pinPTT;
extern int pinPD;
extern int pinAudioOn;
extern int pinI2S_MCLK;
extern int pinI2S_BCLK;
extern int pinI2S_LRCLK;
extern int pinI2S_DIN;
extern int pinI2S_DOUT;
extern int pinVBAT;

// Testing mode
extern bool testingMode;

// DTMF A reboot setting
extern bool dtmfARebootEnabled;

// DTMF # message
extern String dtmfHashMessage;

// RTC state
extern String timezonePosix;
extern bool rtcFound;
extern bool ntpSynced;
extern long gmtOffsetSeconds;  // UTC offset in seconds (e.g., -14400 for EDT = UTC-4)

// Pre/post messages
extern String preMessage;
extern String postMessage;

// Battery reading
extern float lastBatteryV;
extern int lastBatteryPct;

// AXP2101 power state
extern bool extPowerConnected;
extern bool batteryCharging;

// Recording buffers
extern int16_t* audioBuffer;
extern int recordIndex;
extern bool recording;

// Signal quality tracking
extern int peakRSSI;
extern int minRSSI;
extern float peakAudioLevel;
extern int clipCount;

// Recording slots
struct RecordingSlot {
  int16_t* buffer;
  int sampleCount;  // 0 = empty
};
extern RecordingSlot slots[MAX_SLOTS];
extern int nextSlot;

// DTMF detection
extern char detectedDTMF;

#endif // CONFIG_H
