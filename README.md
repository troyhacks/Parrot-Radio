# Doing stupid tricks with the SA868 radio module from NiceRF

<img width="50%" height="50%" alt="A pirate parrot holding a fancy walkie-talkie" src="https://github.com/user-attachments/assets/26e68f07-50eb-4d0b-9f52-cfbfe3091408" />

---

## T-TWR Plus Rev 2.1 Features (Current Best)

This is the recommended hardware platform. ESP32-S3 based with integrated SA868 radio, PMU, OLED, and GPS.

### Radio
- **SA868 VHF/UHF module** (built-in antenna connector)
- Configurable frequency, TX/RX CTCSS tones
- Volume control (0-8)
- **Filters**: Bandpass, De-noise, De-emphasis (individually toggleable)
- **25 kHz / 12.5 kHz** channel bandwidth selection
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

### Macros (for pre/post messages)
- `{temp}` - Temperature
- `{conditions}` - Weather conditions
- `{humidity}` - Humidity (BME280 only)
- `{pressure}` - Pressure
- `{battery}` - Battery percentage
- `{voltage}` - Battery voltage
- `{freq}` - Radio frequency
- `{txctcss}` - TX CTCSS
- `{rxctcss}` - RX CTCSS
- `{version}` - Firmware version

### Hardware
- **OLED display** (SH1106 128x64): Shows IP, time, status, frequency, weather
- **AXP2101 PMU**: Battery monitoring, speaker mute control
- **BME280/BMP280**: Temperature, pressure, humidity sensor
- **DS3231 RTC**: Accurate timekeeping
- **GPS**: Time synchronization
- **WiFi**: Web configuration, NTP sync
- **Battery**: LiPo support with monitoring

### Configuration
- Web server on port 80
- AP mode: SSID "RadioParrot", password "parrot123"
- Preferences stored in flash (survives reboot)
- WiFi auto-reconnect enabled

### Enclosure
3D printed case available on Printables: [LilyGo T-TWR V2.1 Case](https://www.printables.com/model/983103-lilygo-t-twr-v21-case)

~~Yes, the robot voice is terrible. TERRIBLE ON PURPOSE.~~ The robot voice is now... Mid-Atlantic?!? I dunno. I like it.
