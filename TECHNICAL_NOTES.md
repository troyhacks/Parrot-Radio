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
