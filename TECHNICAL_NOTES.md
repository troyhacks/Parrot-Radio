# Upload Process (for reference)

## Finding the Device
```bash
pio device list
```

## Uploading Firmware
1. Kill any Python processes that may hold the port:
   ```bash
   taskkill //F //IM python.exe
   ```
2. Wait a moment and upload:
   ```bash
   pio run --target upload --environment ttwr
   ```

## Monitoring Output
```bash
pio device monitor --environment ttwr
```

Note: The `monitor_reset.py` script requires pyserial which isn't available, so use PlatformIO's monitor directly.

---

# Technical Notes

## espeak-ng Stack Overflow Issue (2026-05-01)

### Problem
The ESP32-S3 would crash with a stack overflow (`Stack canary watchpoint triggered (loopTask)`) when espeak-ng's TTS engine tried to convert numbers to words in English.

The crash consistently occurred in `TransposeAlphabet` during dictionary lookup in `numbers.c`, deep in espeak-ng's recursive number-to-word conversion algorithm.

Example crash:
```
TTS: Friday at 2 42 PM
Guru Meditation Error: Core  1 panic'ed (Unhandled debug exception).
Debug exception reason: Stack canary watchpoint triggered (loopTask)
#0  TransposeAlphabet at dictionary.c:2381
#4  LookupNum2 at numbers.c:1101
#6  TranslateNumber_1 at numbers.c:1706
```

### Root Cause
espeak-ng's English number pronunciation uses deeply recursive algorithms. For example, to pronounce "123", it recursively processes "one hundred twenty-three" with many nested dictionary lookups. The ESP32's 32KB loop task stack was insufficient for this recursion depth.

The issue appeared after adding GPS, sun calculation modules, and DTMF A,B,C,D handlers - likely due to changes in memory layout that reduced stack available to espeak.

### Solution
Pre-convert all numbers to English words **before** passing text to espeak-ng, using a simple iterative number-to-words converter:

```cpp
auto intToWords = [](int n) -> String {
    if (n == 0) return "zero";
    static const char* ones[] = {"zero", "one", "two", ...};
    static const char* tens[] = {"", "", "twenty", ...};
    String s;
    if (n < 0) { s = "minus "; n = -n; }
    if (n >= 100) { s += ones[n/100]; s += " hundred "; n %= 100; }
    if (n >= 20) { s += tens[n/10]; if (n%10) s += " ", s += ones[n%10]; }
    else if (n >= 1) s += ones[n];
    return s;
};
```

This bypasses espeak's number processing entirely and produces identical output.

### Affected Macros
All macros that pass numbers to espeak were affected:
- `{time12}` - e.g., "2 42 PM" → "two forty two PM"
- `{battery}` - percentage
- `{voltage}` - battery voltage
- `{slot}`, `{slots_used}`, `{slots_total}` - slot counts
- `{uptime}` - minutes running
- `{hour}`, `{minute}` - time components

All now use `intToWords()` before macro replacement.

### Files Modified
- `src/tts.cpp` - Added `intToWords` lambda and updated all macro expansions

---

## AXP2101 Battery Monitor Never Updated (2026-05-01)

### Problem
Battery percentage and voltage always showed "unknown" in TTS macros, even though the AXP2101 PMU was properly initialized.

### Root Cause
The `updatePowerState()` function existed in `radio.cpp` and correctly read battery values from the AXP2101, but it was **never called** from the main loop.

### Solution
Added a call to `updatePowerState()` in the periodic idle update section of `parrot.cpp` (every 5 seconds when idle).

### Files Modified
- `src/parrot.cpp` - Added `updatePowerState()` call in idle loop

### Status
RESOLVED - Battery readings now update properly.

---

## BME280/BMP280 Weather Sensor Not Initialized (2026-05-01)

### Problem
The T-TWR Rev 2.1 has a BME280/BMP280 sensor on the I2C bus, but the `weather_sensor` module was never initialized or read.

### Solution
1. Added `initWeatherSensor()` call in `setup()` after Wire/I2C initialization
2. Added `readWeatherSensor()` call in periodic idle updates
3. Added TTS macros for local sensor data:
   - `{localtemp}` - temperature in Celsius
   - `{localhumidity}` - humidity percent (0 for BMP280)
   - `{localpressure}` - pressure in hectopascals

### Files Modified
- `src/parrot.cpp` - Added weather_sensor initialization and periodic reading
- `src/tts.cpp` - Added local weather macros
- `src/weather_sensor.h` - Added `localWeather` extern
- `src/weather_sensor.cpp` - Added `localWeather` storage and macro exports

### Status
IMPLEMENTED - Local weather sensor now available for TTS macros.

---

## String Temporary Lifetime Bug (2026-05-01)

### Problem
After fixing espeak-ng stack overflow, a new crash appeared: `LoadProhibited` in `snprintf` at line 88 of tts.cpp.

### Root Cause
Passing `intToWords().c_str()` directly to `snprintf` as variadic arguments caused undefined behavior. The String temporary could be destroyed before `snprintf` read the pointer.

### Solution
Store String results in local variables before passing to snprintf:

```cpp
// WRONG - String temporary may be destroyed before snprintf reads it
snprintf(buf, sizeof(buf), "%s", intToWords(n).c_str());

// CORRECT - String stays alive through snprintf call
String s = intToWords(n);
snprintf(buf, sizeof(buf), "%s", s.c_str());
```

### Files Modified
- `src/tts.cpp` - Fixed `{time12}` macro expansion

---

## espeak-ng Voice Selection Fix (2026-05-01)

### Problem
TTS output was garbled - espeak was producing random/unrecognizable words ("symbol", "F.B.", hex digits) instead of proper English.

### Root Cause
Using `espeak.setVoice("en")` didn't select a proper voice. Changed to `espeak.setVoice("en-us")` for US English which works correctly.

### Solution
```cpp
espeak.setVoice("en-us");  // Use explicit US English voice
```

### Files Modified
- `src/tts.cpp` - initTTS() voice selection

---

## TTS/PTT Synchronization Fix (2026-05-02)

### Problem
PTT was being released while TTS was still transmitting, cutting off the end of TTS messages. Also, TTS and recorded audio could interleave in the ring buffer, causing playback issues.

Example symptom:

```text
TTS: Weather report, clear sky...
Speaker unmuted
PTT OFF        <-- PTT released while TTS still playing
TTS: ...4 degrees, feels like 0 degrees...
```

### Root Cause

1. `sayText()` queues TTS asynchronously and returns immediately - it does NOT wait for TTS to finish playing
2. The original code used `delay(1000)` before `pttOff()` which was insufficient
3. `playbackWithFeedback()` started TTS pre-message, then immediately began the recorded audio loop without waiting - causing TTS and recorded audio to interleave

### Solution

**1. Added TTS completion EventGroup synchronization (tts.cpp):**
```cpp
static EventGroupHandle_t s_ttsEvents = nullptr;
#define TTS_DONE_BIT (1 << 0)

// In ttsTaskFn, after flush():
if (s_ttsEvents) {
  xEventGroupSetBits(s_ttsEvents, TTS_DONE_BIT);
}

// New function:
void waitForTTSDone() {
  if (s_ttsEvents == nullptr) return;
  xEventGroupClearBits(s_ttsEvents, TTS_DONE_BIT);
  xEventGroupWaitBits(s_ttsEvents, TTS_DONE_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
}
```

**2. Added watchdog yield in TTS output (tts.cpp):**
```cpp
// In TTSOutput::write(), after each 512-sample buffer:
audioWrite(ttsBuffer, ttsBufferIndex);
ttsBufferIndex = 0;
vTaskDelay(1);  // Yield to prevent watchdog timeout
```

**3. Fixed all TTS callers to wait for completion before PTT off:**

- `speakWeather()`: drains and waits after each of speakPreMessage, sayText, speakPostMessage
- `playbackWithFeedback()`: drains and waits after speakPreMessage, before recorded audio loop
- DTMF A/B/C/D/# handlers in parrot.cpp: waitForTTSDone() instead of delay(1000)

### Files Modified

- `src/tts.cpp` - EventGroup sync, vTaskDelay(1) yield
- `src/tts.h` - waitForTTSDone() declaration
- `src/weather.cpp` - proper drain/wait sequencing
- `src/radio.cpp` - playbackWithFeedback sequencing
- `src/radio.h` - drainAudio() export
- `src/parrot.cpp` - all DTMF TTS handlers

### Key Pattern for TTS + PTT
```cpp
speakPreMessage();           // Queue TTS
drainAudio();                // Wait for ring buffer to empty
waitForTTSDone();            // Wait for TTS generation to complete
drainAudio();                // Final drain of last TTS samples

// Now safe to release PTT
pttOff();
```

---

## Sun Calculation Fix (2026-05-02)

### Problem
Sunrise/sunset times were completely wrong - sunrise at 3:33 AM, sunset at 5:45 PM (when Toronto should have sunrise ~6:08 AM, sunset ~8:21 PM in early May).

### Root Cause
The original custom sun calculation code had multiple bugs in the Julian Day calculation, obliquity correction, and EoT formula.

### Solution
Replaced the custom implementation with the well-tested `buelowp/sunset` Arduino library. Key fix was understanding the timezone offset sign:

```cpp
// gmtOffsetSeconds is +14400 for EDT (UTC-4)
// Sunset library adds this to UTC to get local, so we negate
float tzOffsetHours = -gmtOffsetSeconds / 3600.0f;  // = -4 for EDT
```

### Files Modified
- `platformio.ini` - Added `buelowp/sunset@^1.1.0` library
- `src/sun.cpp` - Replaced custom calculation with Sunset library
- `src/parrot.cpp` - Changed "Astronomical dusk" to "Night sky dusk" (espeak pronunciation issue)

### Additional Fix
Changed "Astronomical" to "Night sky" in TTS output because espeak-ng's minimal dictionary doesn't recognize "Astronomical" and produces garbled output.
