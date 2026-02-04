#include <Arduino.h>
#include <HardwareSerial.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <AudioOutput.h>
#include <ESP8266SAM.h>
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

// Serial for SA868
HardwareSerial SA868(2);  // UART2

// Recording parameters
#define SAMPLE_RATE 22050  // Match SAM's native rate
#define MAX_RECORDING_SECONDS 10
#define MAX_SAMPLES (SAMPLE_RATE * MAX_RECORDING_SECONDS)
#define TONE_AMPLITUDE 4000   // Beep volume (max 32767)
#define SAM_VOLUME 0.25f      // SAM speech volume (0.0 - 1.0)

// Buffers (allocated in PSRAM)
int16_t* audioBuffer = nullptr;
int recordIndex = 0;
bool recording = false;

// SAM speech synthesis
int16_t samBuffer[512];
int samBufferIndex = 0;

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

void playSlot(int slotIndex) {
  if (slotIndex < 0 || slotIndex >= MAX_SLOTS) return;

  // Key PTT first - need enough time for radio to key up
  digitalWrite(PTT_PIN, LOW);
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
  digitalWrite(PTT_PIN, HIGH);
}

void playRadioTest() {
  // Key PTT first
  digitalWrite(PTT_PIN, LOW);
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
  digitalWrite(PTT_PIN, HIGH);
  Serial.println("Radio test complete!");
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
    .mck_io_num = I2S_MCLK,
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRCLK,
    .data_out_num = I2S_SD_OUT,
    .data_in_num = I2S_SD_IN
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

  // Set frequency (451.0 MHz simplex, no CTCSS, squelch 4)
  SA868.println("AT+DMOSETGROUP=0,451.0000,451.0000,0000,4,0000");
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
  // Audio ON pin goes LOW when receiving
  return digitalRead(AUDIO_ON_PIN) == LOW;
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
    if (dtmf >= '1' && dtmf <= '9') {
      detectedDTMF = dtmf;
      Serial.printf("*** DTMF %c detected - will play slot %c ***\n", dtmf, dtmf);
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

// SAM audio output class
class SAMAudioOutput : public AudioOutput {
public:
  SAMAudioOutput() {}

  bool begin() { return true; }

  bool ConsumeSample(int16_t sample[2]) {
    // No downsampling needed - I2S runs at 22050Hz to match SAM
    int16_t mono = (int16_t)(sample[0] * SAM_VOLUME);
    samBuffer[samBufferIndex++] = mono;

    if (samBufferIndex >= 512) {
      i2sWrite(samBuffer, samBufferIndex);
      samBufferIndex = 0;
    }
    return true;
  }

  bool stop() {
    // Flush remaining samples
    if (samBufferIndex > 0) {
      i2sWrite(samBuffer, samBufferIndex);
      samBufferIndex = 0;
    }
    return true;
  }
};

SAMAudioOutput samOut;
ESP8266SAM* sam = nullptr;

void sayText(const char* text) {
  Serial.printf("SAM: %s\n", text);
  if (sam) {
    sam->Say(&samOut, text);
    samOut.stop();  // Flush buffer
  }
}

void playTone(int frequency, int duration) {
  int totalSamples = (SAMPLE_RATE * duration) / 1000;
  int16_t buffer[256];
  int bufIndex = 0;

  for (int i = 0; i < totalSamples; i++) {
    float t = (float)i / SAMPLE_RATE;
    int16_t value = (int16_t)(TONE_AMPLITUDE * sin(2 * PI * frequency * t));
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
    playVoiceMessage("audio distorted, reduce volume");
  }
}

void playbackWithFeedback() {
  Serial.println("Starting playback...");

  // Key PTT
  digitalWrite(PTT_PIN, LOW);
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
  digitalWrite(PTT_PIN, HIGH);

  Serial.println("Playback complete!");
}

void setup() {
  // Set PTT HIGH immediately to prevent TX during boot
  pinMode(PTT_PIN, OUTPUT);
  digitalWrite(PTT_PIN, HIGH);  // RX mode

  Serial.begin(115200);
  SA868.begin(9600, SERIAL_8N1, SA868_TX, SA868_RX);

  // Pin setup
  pinMode(PD_PIN, OUTPUT);
  pinMode(AUDIO_ON_PIN, INPUT);
  digitalWrite(PD_PIN, HIGH);   // Normal operation (not power down)

  Serial.println("ESP32 Radio Parrot Starting...");

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

  delay(1000);  // Let module boot
  while (SA868.available()) SA868.read();  // Clear any boot noise

  // Initialize SA868
  initializeSA868();

  // Initialize SAM speech synthesis
  sam = new ESP8266SAM();
  Serial.println("SAM initialized");

  Serial.println("Ready for radio checks!");
}

void loop() {
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

    if (detectedDTMF == '9') {
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

    if (detectedDTMF == '9') {
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
