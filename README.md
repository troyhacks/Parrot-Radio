# Doing stupid tricks with the SA868 radio module from NiceRF

<img width="50%" height="50%" alt="A pirate parrot holding a fancy walkie-talkie" src="https://github.com/user-attachments/assets/26e68f07-50eb-4d0b-9f52-cfbfe3091408" />

## T-TWR Plus Rev 2.1 Features (Current Best)

**Hardware Details**: [TTWR_HARDWARE.md](TTWR_HARDWARE.md) - Deep dive into ESP32-S3 ADC audio, LEDC PWM output, PMU, and more.

This is the recommended hardware platform. ESP32-S3 based with integrated SA868 radio, PMU, OLED, and GPS.

### Radio
- **SA868 VHF/UHF module** (built-in antenna connector)
- Configurable frequency, TX/RX CTCSS tones
- Volume control (0-8)
- **Filters**: Bandpass, De-noise, De-emphasis (individually toggleable)
- ~~~**25 kHz / 12.5 kHz** channel bandwidth selection~~~ This module is only 12.5 kHz
- RSSI monitoring via squelch pin

### Recording & Playback
- **8 recording slots** (circular buffer, oldest overwritten)
- **Parrot mode**: Immediate playback of last transmission
- **Pre/post message wrapping**: Configurable TTS spoken before/after every transmission
- **Radio test audio**: Embedded clean audio for receive testing
- Volume controls for TTS, playback, and test audio

### TTS Announcements
- Weather report (current conditions)
- Time and date announcements
- **Sunrise/sunset times** with astronomical dusk
- **Golden hour** announcements (morning and evening)
- Battery status
- Signal quality feedback after playback

### DTMF Commands
| Key | Function |
|-----|----------|
| 1-8 | Play recording slot 1-8 |
| 9 | Transmit embedded radio test audio |
| * | Speak weather report |
| # | Speak custom message (configurable) |
| A | Speak sunrise/sunset/golden hour |
| B | Speak current date |
| C | Speak current time |
| D | Reboot (if enabled) |

### Web Interface
- All settings configurable via web browser
- Frequency, CTCSS, filters, bandwidth
- Volume and audio settings
- Pre/post message editor with macro support
- Location settings for weather and sun calculations
- Timezone configuration (POSIX TZ string)
- Testing mode (PTT disabled)
- DTMF A instant reboot toggle
- Live status display with IP, time, RSSI

### Macros (for pre/post and TTS test messages)
- `{date}` - Current date
- `{time}` - Current time
- `{day}` - Day of week
- `{hour}` - Current hour
- `{minute}` - Current minute
- `{battery}` - Battery percentage
- `{voltage}` - Battery voltage
- `{freq}` - Radio frequency
- `{uptime}` - Uptime in minutes
- `{ip}` - IP address
- `{localtemp}` - Local temperature (BME280)
- `{localhumidity}` - Local humidity (BME280)
- `{localpressure}` - Local pressure (BME280)
- `{sunrise}` - Sunrise time
- `{sunset}` - Sunset time
- `{timezone}` - Timezone
- `{slot}` - Next recording slot

### Hardware
- **OLED display** (SH1106 128x64): Shows IP, time, status, frequency, weather
- **AXP2101 PMU**: Battery monitoring, speaker mute control
- **BME280/BMP280**: Temperature, pressure, humidity sensor
- **DS3231 RTC**: Accurate timekeeping
- **GPS**: Time synchronization, location (for weather/sun calculations)
- **WiFi**: Web configuration, NTP sync
- **Battery**: LiPo support with monitoring

### Configuration
- Web server on port 80
- AP mode: SSID "RadioParrot", password "parrot123"
- Preferences stored in flash (survives reboot)
- WiFi auto-reconnect enabled

### Safety
- **Testing Mode defaults to ON** (PTT disabled) on fresh flash
- Must explicitly uncheck "Testing Mode" in web interface to enable transmission
- Prevents accidental transmission during development/configuration

### Timezone (BETA)
Timezone can be auto-detected from GPS coordinates:

1. If timezone is **set in preferences**: Use configured timezone
2. If timezone is **blank**:
   - After NTP sync succeeds → Try GPS coordinates → Apply timezone
   - If NTP fails → Sync time from GPS → Apply timezone from GPS coordinates

**Limitations**:
- Uses bounding-box regions for timezone lookup, not precise timezone boundaries
- May not work correctly near timezone borders
- Antarctica and other remote regions may get incorrect timezone

### Enclosure
3D printed case available on Printables: [LilyGo T-TWR V2.1 Case](https://www.printables.com/model/983103-lilygo-t-twr-v21-case)

~~Yes, the robot voice is terrible. TERRIBLE ON PURPOSE.~~ The robot voice is now... Mid-Atlantic?!? I dunno. I like it.
