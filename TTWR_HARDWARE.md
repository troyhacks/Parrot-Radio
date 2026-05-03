# T-TWR Plus Rev 2.1 Hardware Notes

## Overview
This document captures the hardware configuration, initialization sequences, and known quirks for the LilyGo T-TWR Plus Rev 2.1 board (ESP32-S3 based radio parrot project).

## Board Features
- **ESP32-S3** (Xtensa dual-core, 8MB PSRAM, 8MB Flash)
- **SA868** VHF/UHF radio module (built-in)
- **AXP2101** power management IC (PMU)
- **SH1106** OLED display (128x64, I2C)
- **DS3231** RTC (optional, may not be populated)

## I2C Bus

### Pins
- **SDA**: GPIO 8
- **SCL**: GPIO 9
- **Speed**: 400kHz

### Devices on I2C
| Device | Address | Notes |
|--------|---------|-------|
| AXP2101 PMU | 0x34 | Power management |
| SH1106 OLED | 0x3C or 0x3D | Display (auto-detect) |

### Initialization Order
The PMU (XPowersLib) must be initialized BEFORE the display. The PMU controls DC1 which powers the OLED.

```cpp
Wire.begin(PMU_SDA, PMU_SCL);  // 8, 9
Wire.setClock(400000);

// PMU init (enables DC1/OLED power)
pmu.begin(Wire, 0x34);
pmu.enableDC1();  // Powers the OLED

// Display init (after PMU, uses same Wire bus)
u8g2.setI2CAddress(oledAddr << 1);  // addr << 1 is critical!
u8g2.begin();
```

## OLED Display (SH1106)

### Driver
U8g2lib with `U8G2_SH1106_128X64_NONAME_F_HW_I2C`

### Critical Initialization
The I2C address MUST be shifted left by 1 when calling `setI2CAddress()`:
```cpp
u8g2.setI2CAddress(0x3D << 1);  // 0x3D becomes 0x7A
```
This is different from typical Arduino I2C conventions but matches the LilyGo library behavior.

### Address Detection
Scan for display at 0x3C and 0x3D:
```cpp
for (uint8_t addr = 0x3C; addr <= 0x3D; addr++) {
  Wire.beginTransmission(addr);
  if (Wire.endTransmission() == 0) { oledAddr = addr; }
}
```

## ADC Audio Input (ESP32-S3 Internal)

### Configuration
- Uses ADC1 Channel 0 (GPIO 1 if available, or dedicated channel)
- **Mode**: ADC_DIGI (DMA-based, not I2S)
- **Format**: ADC_DIGI_OUTPUT_FORMAT_TYPE2
- **Attenuation**: 6dB (ADC_ATTEN_DB_6) - range ~0-1.1V
- **Bit width**: 12-bit (SOC_ADC_DIGI_MAX_BITWIDTH)
- **Sample rate**: ~20kHz (calibrated)

### Calibration
The ADC must be calibrated at startup to measure true sample rate and DC offset:
```cpp
// Measure DC offset with 1000ms of samples
adcCalibration(true);  // compute DC offset
// Measure sample rate
adcCalibration(false); // measure actual Hz
adcTicksPerSample = measured_ticks;
adcSampleRate = measured_hz;
```

### Key Parameters
| Parameter | Value | Notes |
|-----------|-------|-------|
| SAMPLE_RATE | 22050 Hz | eSpeak NG native rate |
| adcSampleRate | ~20224 Hz | Actual measured rate |
| ADC sample range | 0-4095 | 12-bit |
| Audio DC offset | ~1400-1800 | Measured at startup |
| MIN_AUDIO_LEVEL | 0.02f | Ignore recordings below this |

### DMA Configuration
```cpp
adc_digi_init_config_t initConfig = {
  .max_store_buf_size = 16384,      // Large circular buffer
  .conv_num_each_intr = 1024,       // 1024 samples per interrupt (~51ms at 20kHz)
  .adc1_chan_mask = (1 << 0),      // Channel 0
  .adc2_chan_mask = 0,
};
```

## AI Prompt: Implementing ADC Audio Input and LEDC PWM Audio Output for ESP32

### The Goal

The T-TWR has two audio paths:

```
RECORDING (Radio → ESP32):
  SA868 Audio Out → GPIO 1 (ADC1) → ESP32 DMA → memory buffer

PLAYBACK (ESP32 → Radio):
  memory buffer → ESP32 timer ISR → GPIO 18 (LEDC PWM) → SA868 Mic In
```

### Why ADC DMA?

The ESP32-S3 uses **ADC_DIGI** mode (not I2S) for recording. This is an internal ADC with DMA:
- ADC1 channel 0 is hardwired to the audio input
- DMA transfers samples directly to memory without CPU intervention
- Large circular buffer (16384 bytes) absorbs interrupt latency
- 1024 samples per interrupt at ~20kHz = ~51ms between interrupts

This is more efficient than I2S for single-channel audio input.

### Why LEDC PWM for Output?

The SA868 expects an analog audio input on its MIC pins. Rather than using a true DAC (which ESP32-S3 doesn't have), the project uses **LEDC PWM**:
- LEDC generates a ~39kHz carrier (well above audio band)
- 10-bit duty cycle modulation encodes the audio signal
- Low-pass filtering on the SA868 recovers the audio

The 39kHz carrier is well above audio frequencies (~20kHz max) and well above the PWM refresh rate, so it doesn't interfere.

### Ring Buffer Architecture

Audio uses a producer-consumer ring buffer:

```cpp
typedef struct { int16_t sample; uint16_t ticks; } AudioRingEntry;
static AudioRingEntry audioRingBuf[2048];  // 2048 entries

// Producer (main task): writes samples
void audioWrite(int16_t* data, size_t samples, uint16_t ticksPerSample) {
  for each sample:
    wait for space in buffer
    audioRingBuf[writeIdx] = {sample, ticksPerSample};
    writeIdx = (writeIdx + 1) % 2048;
}

// Consumer (timer ISR): reads samples and plays them
void IRAM_ATTR audioTimerISR() {
  if (writeIdx != readIdx) {
    entry = audioRingBuf[readIdx];
    ledcWrite(0, map(entry.sample to 0-1023 duty cycle));
    timerAlarmWrite(timer, entry.ticks, true);  // variable sample rate!
    readIdx = (readIdx + 1) % 2048;
  }
}
```

The **variable ticks per sample** is key: TTS plays at 22050 Hz, but recorded audio plays at the calibrated ADC rate (~20224 Hz). Each ring buffer entry carries its own timing.

### DC Offset and Audio Levels

ADC reads around 1400-1800 at silence (not exactly 2048 due to hardware). The code:
1. Measures DC offset at startup during calibration
2. Subtracts DC offset from each sample during recording
3. Centers audio around 0 before playback

### Audio Routing (MIC_CH_SEL)

GPIO 17 controls an analog switch:

| GPIO 17 | Route | Use Case |
|---------|-------|----------|
| LOW | Physical mic → SA868 | Normal radio receive |
| HIGH | ESP32 (GPIO 18) → SA868 | TTS/playback transmission |

The ESP32 can either:
- **Record** from the radio (GPIO 17 LOW, ADC reads SA868 audio)
- **Transmit** TTS/playback (GPIO 17 HIGH, LEDC drives SA868 mic input)

### Key Timing Parameters

| Operation | Rate | Timer Ticks |
|-----------|------|-------------|
| TTS playback | 22050 Hz | 45 ticks/sample (1MHz/22050) |
| ADC recording | ~20224 Hz | ~49 ticks/sample (measured) |

Timer runs at 1 MHz (APB 80MHz / prescaler 80). Each sample specifies how many microseconds until the next sample.

---

## AI Prompt: Copy-Paste This for Implementing Similar Audio

```
Task: Implement audio I/O for ESP32-S3 radio project with these requirements:

Recording (ADC DMA):
- Use ESP32-S3 ADC1 in ADC_DIGI mode (not I2S) - more efficient for single-channel
- Configure: 12-bit, ADC_ATTEN_DB_6 (range 0-1.1V), ADC_DIGI_OUTPUT_FORMAT_TYPE2
- DMA with large circular buffer (16384 bytes) and 1024 samples per interrupt
- At startup, calibrate: measure DC offset and true sample rate over 1 second
- Apply DC offset correction to all samples during recording

Playback (LEDC PWM):
- ESP32-S3 has no DAC, so use LEDC PWM at ~39kHz carrier with 10-bit resolution
- Map int16_t audio samples (-32768 to 32767) to PWM duty cycle (0-1023, centered at 512)
- Use a hardware timer with alarm to drive sample output at precise intervals

Variable-Rate Ring Buffer:
- Create a ring buffer where each entry contains: {int16_t sample, uint16_t timerTicks}
- Main task (producer) writes samples with associated timing
- Timer ISR (consumer) reads samples and programs next alarm with that sample's ticks
- This allows mixing audio at different sample rates in the same buffer

Key insight:
- TTS playback: 22050 Hz = 45 timer ticks per sample (1MHz / 22050)
- Recorded audio: ~20224 Hz = 49 timer ticks per sample (measured)
- Each ring buffer entry carries its own ticks so different sources can mix

DC Offset Handling:
- ADC reads ~1400-1800 at silence (not exactly 2048)
- Measure this offset at startup with 1 second of silence
- Subtract offset from all samples, center around 0

Audio Routing (optional but useful):
- Use a GPIO to control an analog switch for routing
- GPIO HIGH: ESP32 audio → radio (for playback/transmit)
- GPIO LOW: radio audio → ESP32 (for recording/receive)

Reference Implementation Details:
- Timer: hw_timer_t with 1 MHz tick (APB 80MHz / prescaler 80)
- Timer ISR must be in IRAM_ATTR for cache consistency
- Ring buffer size: 2048 entries provides good buffering
- Producer should yield (vTaskDelay) if ring buffer is full, not busy-wait
```

---

## SA868 Radio Module

### UART Configuration
- **UART2** on ESP32-S3
- **TX**: GPIO 39 (SA868_RX)
- **RX**: GPIO 48 (SA868_TX)
- **Baud**: 9600, 8N1

### Commands
| Command | Description |
|---------|-------------|
| AT+DMOCONNECT | Check module connection |
| AT+DMOSETGROUP | Set frequency, CTCSS |
| AT+DMOSETVOLUME | Set audio volume (0-8) |
| AT+DMOSETFILTER | Enable/disable filters |

### Pin Control
- **PD_PIN (GPIO 40)**: Power down (active low)
- **PTT_PIN (GPIO 41)**: Push-to-talk (TX mode = HIGH)

## AXP2101 Power Management

### Key Settings
- **DC1**: Powers ESP32, OLED, and PIXEL LEDs
- **ALDO3**: Controls speaker amplifier mute
- **All LDOs**: ALDO1-4, BLDO1-2, DLDO1-2 enabled at startup

### Speaker Mute Control
```cpp
setSpeakerMute(true);   // Enable ALDO3 - mutes speaker
setSpeakerMute(false);  // Disable ALDO3 - unmute speaker
```

## Display States

The display has 4 lines:

| Line | Y range | Content |
| --- | --- | --- |
| Header | 0-9 | IP address (left) + Time (right) |
| Status | 11-30 | Large status text (IDLE, RECORD, PLAYING 1, etc.) |
| Info | 33-46 | Frequency + CTCSS (or custom action text when active) |
| Weather | 50-63 | Weather summary always shown (e.g., "Overcast 8C") |

Display states with custom status text:

| State | Large Status | Info Line |
| --- | --- | --- |
| Idle | IDLE | Frequency + CTCSS |
| Recording | RECORD | Frequency + CTCSS |
| Playing (parrot) | PLAYING X | Frequency + CTCSS (X = slot recorded) |
| Playing slot 1-8 | PLAY SLOT X | Frequency + CTCSS |
| TTS message (#) | TTS MSG | Frequency + CTCSS |
| Weather (*) | WEATHER | Frequency + CTCSS |
| Play test (9) | PLAY TEST | Frequency + CTCSS |

## Known Quirks

1. **OLED I2C Address**: Must use `addr << 1` when calling `setI2CAddress()`. Using raw address (0x3D) doesn't work.

2. **ADC Calibration**: The ADC must be calibrated at startup with 1 second of silence to measure DC offset and true sample rate.

3. **PMU before Display**: The AXP2101 PMU must be initialized and DC1 enabled before the OLED will respond to I2C commands.

4. **Stack Size for TTS**: eSpeak NG TTS is stack-intensive. The loop task stack should be at least 16384 bytes (may need increased):
   ```ini
   build_flags = ... -DCONFIG_ARDUINO_LOOP_STACK_SIZE=16384
   ```

5. **Recording Speed Ratio**: Audio may record at 0.97x speed due to hardware calibration variations. This is normal and compensated during playback.

6. **WiFi Start Delay**: After WiFi connects, the system ignores squelch for 5 seconds to avoid startup RF noise triggering recording.

## Pin Summary (T-TWR)

| Pin | Function |
|-----|----------|
| 1 | ADC input (radio audio) |
| 2 | AUDIO_ON (squelch detect) |
| 8 | I2C SDA |
| 9 | I2C SCL |
| 17 | MIC_CH_SEL (audio routing) |
| 18 | ESP2MIC (audio output) |
| 39 | SA868_RX (UART TX) |
| 40 | PD_PIN (SA868 power down) |
| 41 | PTT_PIN (push-to-talk) |
| 48 | SA868_TX (UART RX) |
