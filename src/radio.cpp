#include "radio.h"
#include "config.h"
#include "tts.h"

// recording state is managed in parrot.cpp
extern bool recording;
#include <driver/i2s.h>
#include <driver/ledc.h>
#include <driver/adc.h>
#include <driver/timer.h>
#include <hal/adc_types.h>
#include <esp_heap_caps.h>
#include <radio_test_audio.h>

#ifdef BOARD_TTWR
#include <XPowersLib.h>
#include <Wire.h>
static XPowersAXP2101 pmu;
#endif

// DTMF frequencies (Hz)
static const float DTMF_FREQS[8] = {697, 770, 852, 941, 1209, 1336, 1477, 1633};
// Row/column mapping to digits
static const char DTMF_CHARS[4][4] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// Forward declarations for timer-driven audio output (defined later)
void startAudioOutput();
void stopAudioOutput();
void drainAudio();

// Goertzel coefficients (precomputed using actual sample rate)
static float goertzelCoeff[8];

void initGoertzel(float sampleRate) {
  for (int i = 0; i < 8; i++) {
    float k = (DTMF_BLOCK_SIZE * DTMF_FREQS[i]) / sampleRate;
    goertzelCoeff[i] = 2.0f * cos(2.0f * PI * k / DTMF_BLOCK_SIZE);
  }
  Serial.printf("Goertzel initialized for %.0f Hz (DTMF_BLOCK_SIZE=%d)\n", sampleRate, DTMF_BLOCK_SIZE);
}

static float goertzelMagnitude(int16_t* samples, int count, int freqIndex) {
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

  // Debug: show top magnitudes periodically
  static int dbgDtmf = 0;
  if (dbgDtmf < 3) {
    Serial.printf("DTMF mag[%d]: row=%.0f, col=%.0f\n", dbgDtmf, maxRow, maxCol);
    dbgDtmf++;
  }

  // Need both row and column to be significantly above noise
  // With 205 samples at 22050 Hz, real DTMF tones produce magnitudes >> 1e11
  // Use 1e11 to avoid false triggers from noise or speech harmonics
  float threshold = 1e11;
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

  // Copy current recording to slot (at ADC rate - playback uses adcTicksPerSample to match)
  int copyCount = min(recordIndex, MAX_SAMPLES);
  memcpy(slots[slotIndex].buffer, audioBuffer, copyCount * sizeof(int16_t));
  slots[slotIndex].sampleCount = copyCount;
  Serial.printf("Saved %d samples to slot %d\n", copyCount, slotIndex + 1);
}

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
    // Speaker is muted, so routing doesn't matter for feedback prevention
    setSpeakerMute(true);
    sayText("no recording");
    setSpeakerMute(false);
  } else {
    Serial.printf("Playing slot %d (%d samples)\n", slotIndex + 1, slots[slotIndex].sampleCount);
    setAudioRoutingToRadio(true);

    // Play back the slot at the rate it was recorded (adcTicksPerSample)
    for (int i = 0; i < slots[slotIndex].sampleCount; i += 256) {
      int chunkSize = min(256, slots[slotIndex].sampleCount - i);
      audioWrite(&slots[slotIndex].buffer[i], chunkSize, adcTicksPerSample);
    }
    drainAudio();
    setAudioRoutingToRadio(false);
  }

  delay(300);
  pttOff();
}

void playRadioTest() {
  // Key PTT first
  pttOn();
  delay(900);

  Serial.printf("Playing radio test audio (%d samples, %.1f sec) at %d%% volume\n",
                RADIO_TEST_SAMPLES, (float)RADIO_TEST_SAMPLES / RADIO_TEST_SAMPLE_RATE, radioTestVolumePercent);

  setAudioRoutingToRadio(true);

  // Play embedded audio from PROGMEM
  int16_t buffer[256];
  for (int i = 0; i < RADIO_TEST_SAMPLES; i += 256) {
    int chunkSize = min(256, RADIO_TEST_SAMPLES - i);
    // Copy from PROGMEM to RAM buffer with volume scaling
    for (int j = 0; j < chunkSize; j++) {
      buffer[j] = (pgm_read_word(&radioTestAudio[i + j]) * radioTestVolumePercent) / 100;
    }
    audioWrite(buffer, chunkSize);
  }

  drainAudio();
  setAudioRoutingToRadio(false);

  delay(300);
  pttOff();
  Serial.println("Radio test complete!");
}

// Calibrated timer ticks per sample for recorded audio playback (set during ADC calibration)
uint16_t adcTicksPerSample = 1814;  // Default = 22050 Hz; updated in initAudioInput
// Actual measured ADC sample rate in Hz (computed during ADC calibration)
float adcSampleRate = 22050.0f;

#ifdef BOARD_TTWR
// ==================== T-TWR Audio Implementation ====================
// T-TWR uses ESP32-S3 internal ADC for audio input and LEDC for audio output

static bool pmuFound = false;
static bool adcI2sInitialized = false;
static const i2s_port_t ADC_I2S_PORT = I2S_NUM_1;  // Use I2S1 for ADC on ESP32-S3
int lastKnownRSSI = 0;  // Cached RSSI for squelch fallback during recording

void initAudioHardware() {
  Serial.println("initAudioHardware starting...");
  // Initialize PMU for speaker mute control
  // AXP2101 typical I2C address is 0x34
  Serial.println("Attempting PMU init...");
  Wire.begin(PMU_SDA, PMU_SCL);
  Serial.println("Wire begun...");
  if (!pmu.begin(Wire, 0x34, PMU_SDA, PMU_SCL)) {
    Serial.println("PMU not found at 0x34, trying 0x68...");
    if (!pmu.begin(Wire, 0x68, PMU_SDA, PMU_SCL)) {
      Serial.println("PMU not found, speaker mute disabled");
      pmuFound = false;
    } else {
      Serial.println("PMU found at 0x68!");
      pmuFound = true;
    }
  } else {
    Serial.println("PMU found at 0x34!");
    pmuFound = true;
  }
  Serial.println("PMU init complete...");

#ifdef BOARD_TTWR
  // Enable OLED display power - OLED is powered by DC1 on T-TWR
  if (pmuFound) {
    // AXP2101 available LDOs: ALDO1-4, BLDO1-2, DLDO1-2, CPUSLDO
    pmu.enableALDO1(); pmu.enableALDO2(); pmu.enableALDO3(); pmu.enableALDO4();
    pmu.enableBLDO1(); pmu.enableBLDO2();
    pmu.enableDLDO1(); pmu.enableDLDO2();
    Serial.println("All LDOs enabled");

    // Enable DC1 (OLED power) - DC1 powers ESP + OLED + PIXEL on Rev 2.1
    pmu.enableDC1();
    Serial.printf("DC1 enabled: %s\n", pmu.isEnableDC1() ? "yes" : "no");
  }
#endif

  // Configure MIC_CH_SEL pin to route audio to ESP32→SA868 path
  pinMode(MIC_CH_SEL_PIN, OUTPUT);
  digitalWrite(MIC_CH_SEL_PIN, LOW);  // Default: physical mic selected

  // Configure audio output pin (GPIO 18 = ESP2MIC)
  // Use LEDC - 10-bit resolution for better audio quality
  // Target ~39kHz PWM (well above audio band to minimize aliasing with 22.05kHz sample rate)
  // 80MHz / 2 / 1023 ≈ 39.1kHz
  bool ledcOk = ledcSetup(0, 39100, 10);  // ~39kHz, 10-bit
  if (ledcOk) {
    ledcAttachPin(ESP2MIC_PIN, 0);
    ledcWrite(0, 512);  // Center point (10-bit = 0-1023, center is 512)
    Serial.println("LEDC channel 0 configured (10-bit, ~39kHz)");
    startAudioOutput();  // Start timer-driven sample output
  } else {
    Serial.println("LEDC setup failed!");
    // Fallback: just set pin mode, will try direct toggle in audioWrite
    pinMode(ESP2MIC_PIN, OUTPUT);
  }

  Serial.println("T-TWR audio hardware initialized");
}

void setAudioRoutingToRadio(bool enable) {
  // When enabling radio audio path: GPIO17 HIGH routes ESP32 audio to SA868
  // When disabling: GPIO17 LOW routes physical mic to SA868
  digitalWrite(MIC_CH_SEL_PIN, enable ? HIGH : LOW);
  Serial.printf("Audio routing: %s\n", enable ? "ESP32→SA868" : "MIC→SA868");
}

void setSpeakerMute(bool mute) {
  if (pmuFound) {
    if (mute) {
      pmu.enableALDO3();  // ESP32 controls amplifier
      Serial.println("Speaker muted (ALDO3 enabled)");
    } else {
      pmu.disableALDO3();  // Radio controls amplifier
      Serial.println("Speaker unmuted (ALDO3 disabled)");
    }
  }
}

// Update AXP2101 power state - call periodically from main loop
void updatePowerState() {
  if (!pmuFound) return;

  extPowerConnected = pmu.isVbusIn();
  batteryCharging = pmu.isCharging();

  // Update battery voltage/percent if we have a battery
  if (pmu.isBatteryConnect()) {
    int mv = pmu.getBattVoltage();
    if (mv > 0) {
      lastBatteryV = mv / 1000.0f;
      int pct = pmu.getBatteryPercent();
      if (pct >= 0) lastBatteryPct = pct;
    }
  }
}

// ==================== Timer-driven audio output ====================
// Ring buffer for timer-driven LEDC output at proper sample rate
#define AUDIO_RING_BUF_SIZE 2048
// Each entry: sample value + timer ticks per sample (TTS=1814, ADC=measured)
typedef struct { int16_t sample; uint16_t ticks; } AudioRingEntry;
static AudioRingEntry audioRingBuf[AUDIO_RING_BUF_SIZE];
static volatile uint16_t audioRingWriteIdx = 0;
static volatile uint16_t audioRingReadIdx = 0;
static volatile bool audioTimerRunning = false;
static hw_timer_t* audioTimer = nullptr;
// Overflow counter for audio ring buffer
static volatile uint32_t audioRingOverflowCount = 0;

// Measured DC offset of ADC (calibrated at startup)
static uint16_t adcCenter = 2048;  // Default; real value calibrated in initAudioInput
// Adaptive DC estimate (reset at start of each recording)
float dcEstimate = 2048.0f;

// DTMF check counter - samples accumulated since last DTMF check
static int dtmfCheckCounter = 0;
// Timestamp when current transmission started (to ignore brief squelch at start)
static unsigned long transmissionStartTime = 0;
// Timestamp when recording began (to compute actual vs recorded duration)
static unsigned long recordingStartTime = 0;
// Timestamp of last squelch LOW (for debounce timing)
static unsigned long lastSquelchLowTime = 0;
static void IRAM_ATTR audioTimerISR() {
  if (audioRingWriteIdx != audioRingReadIdx) {
    AudioRingEntry entry = audioRingBuf[audioRingReadIdx];
    audioRingReadIdx = (audioRingReadIdx + 1) % AUDIO_RING_BUF_SIZE;
    // Map int16_t (-32768 to 32767) to 10-bit duty (0 to 1023), centered at 512
    uint16_t duty = (uint16_t)constrain((entry.sample >> 6) + 512, 0, 1023);
    ledcWrite(0, duty);
    // Change timer interval for next sample (enables mixed TTS at 45 + recorded at adcTicksPerSample)
    timerAlarmWrite(audioTimer, entry.ticks, true);
  }
}

// Forward declaration for the three-argument version
void audioWrite(int16_t* data, size_t samples, uint16_t ticksPerSample);

// Two-argument version defaults to TTS speed (45 ticks = 1MHz / 22222Hz)
void audioWrite(int16_t* data, size_t samples) {
  audioWrite(data, samples, 45);
}
// Three-argument version with configurable timer ticks per sample
void audioWrite(int16_t* data, size_t samples, uint16_t ticksPerSample) {
  for (size_t i = 0; i < samples; i++) {
    uint16_t next = (audioRingWriteIdx + 1) % AUDIO_RING_BUF_SIZE;
    while (next == audioRingReadIdx) {
      audioRingOverflowCount++;
      vTaskDelay(pdMS_TO_TICKS(1)); // Let FreeRTOS breathe instead of busy-waiting
    }
    audioRingBuf[audioRingWriteIdx] = (AudioRingEntry){data[i], ticksPerSample};
    audioRingWriteIdx = next;
  }
}

void startAudioOutput() {
  if (audioTimer == nullptr) {
    // Use timer 1, divider 80 (1 µs tick), for 22050 Hz
    audioTimer = timerBegin(1, 80, true);
    timerAttachInterrupt(audioTimer, &audioTimerISR, true);
    // 1MHz / 45 = 22222 Hz (close enough to 22050 that TTS pitch is correct)
    timerAlarmWrite(audioTimer, 45, true);
    timerAlarmEnable(audioTimer);
    audioTimerRunning = true;
    Serial.println("Audio output timer started at ~22222 Hz");
  }
}

void stopAudioOutput() {
  if (audioTimer != nullptr) {
    timerAlarmDisable(audioTimer);
    timerDetachInterrupt(audioTimer);
    timerEnd(audioTimer);
    audioTimer = nullptr;
    audioTimerRunning = false;
    ledcWrite(0, 128);  // Center/silence
    Serial.println("Audio output timer stopped");
  }
}

// Wait for ring buffer to drain completely before stopping timer
void drainAudio() {
  // At 22050 Hz, a 1024-sample buffer drains in ~46 ms
  uint32_t deadline = xTaskGetTickCount() + 100 / portTICK_PERIOD_MS;
  while (audioRingWriteIdx != audioRingReadIdx && xTaskGetTickCount() < deadline) {
    vTaskDelay(1); // Non-blocking yield
  }
}

void initAudioInput() {
  Serial.println("T-TWR audio input init - using ADC DMA...");

  // Try to deinitialize first in case of partial state
  adc_digi_deinitialize();

  // Initialize digital ADC DMA
  // BUFFER OVERFLOW FIX: Use very large conv_num_each_intr to minimize interrupt overhead.
  // At 20kHz, 1024 samples = ~51ms per interrupt — main loop can easily drain in time.
  adc_digi_init_config_t initConfig = {
    .max_store_buf_size = 16384, // 4096 samples * 4 bytes — large circular buffer
    .conv_num_each_intr = 1024,  // 1024 samples per interrupt (~51ms at 20kHz)
    .adc1_chan_mask = (1 << 0), // ADC1 channel 0
    .adc2_chan_mask = 0,
  };

  esp_err_t err = adc_digi_initialize(&initConfig);
  if (err != ESP_OK) {
    Serial.printf("ADC DIGI init failed: %d\n", err);
    return;
  }
  Serial.println("ADC DIGI initialized OK");

  // Configure conversion pattern for single channel
  adc_digi_pattern_config_t patternConfig;
  patternConfig.atten = 1;           // ADC_ATTEN_DB_6 = 1 (about 0-1.1V range, better for ~0.3-0.5V signals)
  patternConfig.channel = 0;          // ADC1 channel 0
  patternConfig.unit = 0;             // 0 = ADC_UNIT_1 on ESP32-S3
  patternConfig.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;  // Use SOC macro

  adc_digi_configuration_t digiConfig;
  digiConfig.conv_limit_en = 0;       // disable limit
  digiConfig.conv_limit_num = 255;
  digiConfig.pattern_num = 1;
  digiConfig.adc_pattern = &patternConfig;
  digiConfig.sample_freq_hz = 20000;  // Reduced to 20kHz so main loop can keep up with DMA
  digiConfig.conv_mode = ADC_CONV_SINGLE_UNIT_1;  // Use ADC1 only
  digiConfig.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;  // TYPE2 for ESP32-S3

  Serial.printf("Config: mode=%d, format=%d, atten=%d, chan=%d, unit=%d, bits=%d\n",
                digiConfig.conv_mode, digiConfig.format,
                patternConfig.atten, patternConfig.channel, patternConfig.unit, patternConfig.bit_width);

  err = adc_digi_controller_configure(&digiConfig);
  if (err != ESP_OK) {
    Serial.printf("ADC DIGI config failed: %d\n", err);
    return;
  }
  Serial.println("ADC DIGI configured OK");

  err = adc_digi_start();
  if (err != ESP_OK) {
    Serial.printf("ADC DIGI start failed: %d\n", err);
    return;
  }

  adcI2sInitialized = true;
  Serial.println("ADC DMA started. Calibrating true hardware speed and DC offset (1000ms)...");

  uint32_t calStart = millis();
  uint32_t calSamples = 0;
  uint64_t dcSum = 0;
  uint8_t calResult[512 * 4];
  uint32_t ret_num;

  while (millis() - calStart < 1000) {
    esp_err_t err = adc_digi_read_bytes(calResult, sizeof(calResult), &ret_num, 30);
    if (err == ESP_OK) {
      int n = ret_num / 4;  // TYPE2 format: 4 bytes per sample
      calSamples += n;
      for (int i = 0; i < n; i++) {
        uint32_t val = calResult[i * 4] | (calResult[i * 4 + 1] << 8) |
                       (calResult[i * 4 + 2] << 16) | (calResult[i * 4 + 3] << 24);
        dcSum += (val & 0xFFF);
      }
    }
  }

  if (calSamples > 0) {
    adcCenter = (uint16_t)(dcSum / calSamples);
    Serial.printf("ADC DC offset: %d\n", adcCenter);
    Serial.printf("ADC calibration: %lu samples in 1 second\n", calSamples);

    // Compute tick count from measured ADC rate.
    // Timer clock = 1 MHz (APB 80 MHz / prescaler 80 = 1 µs/tick).
    // Ticks needed = 1,000,000 / samples_per_second.
    adcSampleRate = (float)calSamples;
    adcTicksPerSample = (uint16_t)(1000000.0f / adcSampleRate + 0.5f);  // Round to nearest
    Serial.printf("ADC actual rate: %.0f Hz (%u timer ticks/sample)\n",
                  adcSampleRate, adcTicksPerSample);
  } else {
    Serial.println("ADC calibration failed, using defaults");
    adcTicksPerSample = 45;
    adcSampleRate = 22050.0f;
  }
}

int audioRead(int16_t* buffer, size_t samples) {
  if (!adcI2sInitialized) {
    Serial.println("audioRead: ADC not initialized, using analogRead fallback");
    for (size_t i = 0; i < samples; i++) {
      buffer[i] = (int16_t)(analogRead(RADIO_AUDIO_PIN) - (int)adcCenter) << 4;
    }
    return samples;
  }

  // Read audio samples via ADC DMA - matching WLED-MM DMAadcSource
  // For ESP32-S3, ADC_RESULT_BYTE = 4 (SOC_ADC_DIGI_RESULT_BYTES)
  // Buffer must handle up to conv_num_each_intr=1024 samples
  // Static to avoid stack overflow in main loop task
  static uint8_t result[1024 * 4];  // 1024 samples * 4 bytes
  uint32_t ret_num;

  esp_err_t err = adc_digi_read_bytes(result, sizeof(result), &ret_num, 30);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    // Error recovery - use analogRead fallback
    for (size_t i = 0; i < samples; i++) {
      buffer[i] = (int16_t)(analogRead(RADIO_AUDIO_PIN) - (int)adcCenter) << 4;
    }
    return samples;
  }

  static int dbgCallCount = 0;
  int samplesRead = ret_num / 4;  // TYPE2 format: 4 bytes per sample

  // Debug first few calls
  if (dbgCallCount < 3) {
    Serial.printf("audioRead[%d]: requested=%d, ret_num=%d, samplesRead=%d, adcCenter=%d\n",
                  dbgCallCount, samples, ret_num, samplesRead, adcCenter);
    dbgCallCount++;
  }

  // Parse TYPE2 format - extract 12-bit ADC value from 4-byte result
  // Use adaptive DC removal: very slow-moving average to prevent drift
  // alpha=0.999995 → time constant ≈ 11 sec (slow enough to not distort speech)
  const float dcAlpha = 0.999995f;
  int j = 0;
  for (int i = 0; i < samplesRead && j < (int)samples; i++) {
    uint32_t val = result[i * 4] | (result[i * 4 + 1] << 8) |
                   (result[i * 4 + 2] << 16) | (result[i * 4 + 3] << 24);
    // TYPE2: lower 12 bits is the ADC value
    uint16_t adcValue = val & 0xFFF;

    // Adaptive DC removal: update estimate and subtract
    dcEstimate = dcAlpha * dcEstimate + (1.0f - dcAlpha) * (float)adcValue;
    int32_t sample = ((int32_t)adcValue - (int32_t)dcEstimate) << 4;
    buffer[j++] = (int16_t)constrain(sample, -32768, 32767);
  }

  return j;  // Return actual count of samples read
}

#else
// ==================== Original ESP32-WROVER-KIT Implementation ====================
// Uses I2S for audio input and output

void initAudioHardware() {
  initI2S();
}

void audioWrite(int16_t* data, size_t samples) {
  i2sWrite(data, samples);
}

void initAudioInput() {
  // I2S handles input when initI2S() was called
}

void audioRead(int16_t* buffer, size_t samples) {
  // For original board, we read via I2S in recordAudioSamples
  // This function unused for original board
}

void setAudioRoutingToRadio(bool enable) {
  // Not needed for original board - always routes to radio
}

void setSpeakerMute(bool mute) {
  // Not needed for original board
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

void i2sWrite(int16_t* data, size_t samples) {
  size_t bytesWritten = 0;
  i2s_write(I2S_PORT, data, samples * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
}

#endif // BOARD_TTWR

void initializeSA868() {
  Serial.println("Initializing SA868...");

  // Handshake
  SA868.println("AT+DMOCONNECT");
  delay(500);
  while (SA868.available()) {
    String response = SA868.readStringUntil('\n');
    Serial.println("SA868: " + response);
  }

  // Set frequency from stored settings (simplex mode: TX=RX)
  // AT+DMOSETGROUP=0,<freq_tx>,<freq_rx>,<tx_ctcss>,<rx_ctcss>,<bandwidth>
  // bandwidth: 0=12.5kHz, 1=25kHz
  String cmd = "AT+DMOSETGROUP=0," + radioFreq + "," + radioFreq + "," + radioTxCTCSS + "," + String(radioSquelch) + "," + radioRxCTCSS + "," + String(radioBandwidth25);  // 1 = 25kHz, 0 = 12.5kHz
  Serial.printf("Radio config: %s\n", cmd.c_str());
  SA868.println(cmd);
  delay(500);
  while (SA868.available()) {
    String response = SA868.readStringUntil('\n');
    Serial.println("SA868: " + response);
  }

  // Set volume
  SA868.println("AT+DMOSETVOLUME=" + String(radioVolume));
  delay(500);
  while (SA868.available()) {
    String response = SA868.readStringUntil('\n');
    Serial.println("SA868: " + response);
  }

  // Set filter: bandpass, de-noise, de-emphasis
  SA868.println("AT+SETFILTER=" + String(radioFilterBP) + "," + String(radioFilterDENoise) + "," + String(radioFilterDER));
  delay(500);
  while (SA868.available()) {
    String response = SA868.readStringUntil('\n');
    Serial.println("SA868: " + response);
  }

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
  // Ignore squelch pin while WiFi is settling (RF noise during startup causes false triggers)
  // Note: AP mode does NOT ignore squelch - external radio signals should still be detected
  if (millis() < wifiReadyTime) return false;

  // Audio ON pin goes LOW when receiving
  bool squelchLow = (digitalRead(pinAudioOn) == LOW);

  if (squelchLow) {
    lastSquelchLowTime = millis();
    // Mark when a new transmission starts
    if (!recording) {
      transmissionStartTime = millis();
    }
    return true;
  }

  // Squelch HIGH: wait 500ms before declaring transmission over
  if (millis() - lastSquelchLowTime > 500) {
    return false;
  }

  // Keep alive during recording or brief squelch drops
  return true;
}

void startRecording() {
  recording = true;
  recordIndex = 0;
  peakRSSI = 0;
  minRSSI = 999;
  peakAudioLevel = 0;
  clipCount = 0;
  detectedDTMF = 0;  // Reset DTMF detection
  dtmfCheckCounter = 0;  // Reset DTMF check counter
  lastKnownRSSI = 0;  // Reset RSSI cache at start of new recording
  // squelchHighCount removed - replaced with lastSquelchLowTime
  transmissionStartTime = 0;  // Will be set when squelch next goes LOW

#ifdef BOARD_TTWR
  // Reset DC estimate at start of recording to prevent drift
  dcEstimate = adcCenter;
  // Stop audio output timer during recording to reduce digital noise on ADC path
  stopAudioOutput();
  // Route audio from ESP32 to radio
  setAudioRoutingToRadio(false);  // false = keep physical mic path
#endif

  recordingStartTime = millis();
  Serial.println("Recording started...");
}

void stopRecording() {
  recording = false;

#ifdef BOARD_TTWR
  // Restart audio output timer for playback
  startAudioOutput();
#endif

  // Estimate duration based on sample count and sample rate
  float durationSec = (float)recordIndex / SAMPLE_RATE;
  float elapsedSec = (millis() - recordingStartTime) / 1000.0f;
  Serial.printf("Recording stopped. %d samples captured (%.1f sec @ %d Hz, %.1f sec elapsed).\n",
                recordIndex, durationSec, SAMPLE_RATE, elapsedSec);
  Serial.printf("  Recording speed ratio: %.2fx (1.00 = correct)\n", durationSec / elapsedSec);
  Serial.printf("RSSI: min=%d, peak=%d\n", minRSSI, peakRSSI);
  Serial.printf("Audio: peak=%.1f, clipped samples=%d\n", peakAudioLevel, clipCount);
}

void recordAudioSamples() {
#ifdef BOARD_TTWR
  // T-TWR: Read audio via ADC DMA from GPIO 1
  // Increased to 1024 samples to match conv_num_each_intr=1024 DMA interrupt size
  // Static to avoid stack overflow in main loop task
  static int16_t samples[1024];
  int actual = audioRead(samples, 1024);

  // Debug: show actual returned count for first few calls
  static int dbgRec = 0;
  if (dbgRec < 5) {
    Serial.printf("recordAudioSamples[%d]: actual=%d, recordIndex=%d\n", dbgRec, actual, recordIndex);
    dbgRec++;
  }

  for (int i = 0; i < actual && recordIndex < MAX_SAMPLES; i++) {
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
#else
  // Original board: Read via I2S
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
#endif

  // DTMF detection - check periodically during recording
  // Only detect once (first DTMF wins)
  dtmfCheckCounter += actual;  // Count samples, not calls (batch size varies)
  if (detectedDTMF == 0 && dtmfCheckCounter >= DTMF_BLOCK_SIZE && recordIndex >= DTMF_BLOCK_SIZE) {
    dtmfCheckCounter = 0;
    // Check the most recent samples for DTMF
    int startIdx = max(0, recordIndex - DTMF_BLOCK_SIZE);
    char dtmf = detectDTMF(&audioBuffer[startIdx], DTMF_BLOCK_SIZE);
    if ((dtmf >= '1' && dtmf <= '9') || (dtmf >= 'A' && dtmf <= 'D') || dtmf == '*' || dtmf == '#') {
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

void generateQualityFeedback() {
  if (peakRSSI > 140) {
    playTone(1200, 200);
    sayText("excellent signal");
  } else if (peakRSSI > 120) {
    playTone(1000, 200);
    delay(100);
    playTone(1000, 200);
    sayText("good signal");
  } else if (peakRSSI > 100) {
    playTone(800, 200);
    delay(100);
    playTone(800, 200);
    delay(100);
    playTone(800, 200);
    sayText("fair signal");
  } else if (peakRSSI > 0) {
    playTone(400, 500);
    sayText("weak signal, check antenna");
  } else {
    playTone(300, 300);
    delay(100);
    playTone(300, 300);
    sayText("no signal");
  }

  if (clipCount > CLIP_COUNT_WARN) {
    delay(300);
    sayText("audio clipping, reduce volume");
  }
}

void playbackWithFeedback() {
  Serial.println("Starting playback...");
  Serial.printf("playbackWithFeedback: recordIndex=%d\n", recordIndex);

  // Key PTT
  pttOn();
  delay(300);  // PTT tail delay

#ifdef BOARD_TTWR
  // Route audio to radio on T-TWR
  Serial.println("playbackWithFeedback: setting routing to ESP32");
  setAudioRoutingToRadio(true);
#endif

  speakPreMessage();

  // Wait for pre-message TTS to finish playing before starting recorded audio
  // This ensures TTS and recorded audio don't interleave in the ring buffer
  drainAudio();
  waitForTTSDone();
  drainAudio();

  // Play back recorded audio with volume applied (at the rate it was recorded)
  Serial.printf("playbackWithFeedback: playbackVolumePercent=%d, recordIndex=%d, ticks=%d\n",
                playbackVolumePercent, recordIndex, adcTicksPerSample);
  int16_t buffer[256];
  for (int i = 0; i < recordIndex; i += 256) {
    int chunkSize = min(256, recordIndex - i);
    for (int j = 0; j < chunkSize; j++) {
      buffer[j] = (audioBuffer[i + j] * playbackVolumePercent) / 100;
    }
    audioWrite(buffer, chunkSize, adcTicksPerSample);  // play at recorded rate
  }
  Serial.println("playbackWithFeedback: audio loop done");

#ifdef BOARD_TTWR
  drainAudio();  // Wait for ring buffer to empty before muting
#endif

  delay(500);  // Gap before feedback tones

#ifdef BOARD_TTWR
  // Mute speaker so mic can't pick up TTS - routing can stay ESP32→SA868
  // because muted speaker produces no audio for mic to pick up
  setSpeakerMute(true);
#endif

  // Generate quality feedback (muted - but routing stays ESP32→SA868 for TX)
  generateQualityFeedback();

  speakPostMessage();

  // Wait for ALL TTS to complete before PTT off
  // First wait: for generateQualityFeedback TTS (e.g., "good signal")
  drainAudio();
  waitForTTSDone();
  // Second wait: for speakPostMessage TTS (e.g., "thank you, good bye!")
  drainAudio();
  waitForTTSDone();
  // Final drain: ensure ring buffer is empty before PTT off
  drainAudio();

  delay(300);  // Final tail

  // Release PTT
  pttOff();

#ifdef BOARD_TTWR
  // Unmute speaker after PTT is off
  setSpeakerMute(false);
#endif

  Serial.println("Playback complete!");
}