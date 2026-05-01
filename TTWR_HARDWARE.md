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

## Audio Output (T-TWR)

### Route: ESP32 to Radio (Playback)
- **GPIO 17** (MIC_CH_SEL): HIGH = route ESP32 audio to SA868
- **GPIO 18** (ESP2MIC): Audio output from ESP32 to SA868

### Route: Radio to ESP32 (Recording)
- **GPIO 1**: ADC input from SA868 audio output
- **MIC_CH_SEL** = LOW routes physical mic

### Audio Output Method
LEDC (Pulse Width Modulation) at ~39kHz, 10-bit resolution.

```cpp
ledcSetup(0, 39100, 10);  // ~39kHz, 10-bit
ledcAttachPin(ESP2MIC_PIN, 0);
ledcWrite(0, 512);  // Center point (10-bit = 0-1023)
```

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

The display shows different information based on state:

| State | Display |
|-------|---------|
| Booting | Splash screen + status message |
| Idle | IP, Time, IDLE |
| Recording | "RECORDING" indicator |
| Playing | "PLAYING" indicator |
| Transmitting | "TX" indicator (during TTS/weather/playback) |
| DTMFDetected | Shows detected DTMF digit |

## Known Quirks

1. **OLED I2C Address**: Must use `addr << 1` when calling `setI2CAddress()`. Using raw address (0x3D) doesn't work.

2. **ADC Calibration**: The ADC must be calibrated at startup with 1 second of silence to measure DC offset and true sample rate.

3. **PMU before Display**: The AXP2101 PMU must be initialized and DC1 enabled before the OLED will respond to I2C commands.

4. **Stack Size for TTS**: eSpeak NG TTS is stack-intensive. The loop task stack should be at least 16384 bytes:
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
