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

### Development
This branch: [Lilygo_T-TWR_Rev2.1](https://github.com/troyhacks/Parrot-Radio/tree/Lilygo_T-TWR_Rev2.1)

---

*Work in progress*, meant as an automated walkie-check system for events. 

It assumes 4MB of PSRAM, and stores your last 8 radio tests. 

These are overwritten as new ones come in, as a circular buffer (so if your recording is in #5, it'll be in #5 until it's overwritten.)

Recordings are flushed on reboot - temporary memory only, except for the embedded test file. 

You can now specify pre/post message strings for the TTS. These support various variable expansions. 

* DTMF 1..8 will recall that particular radio test. 
* DTMF 9 will transmit a clean audio file (encoded in the firmware) so you can see how you're receiving a clean transmit.
* DTMF * will transmit a read of your local weather conditions.
* DTMF # will transmit a customized message. (no pre/post messsages for this one)
* DTMF A,B,C,D are yet to be defined. (If you didn't know there's A,B,C,D in DTMF, you're too young.)

TODOs include confirming the radio debug message logic, doing real range testing, scheduled "if you hear this your walkie is working" messages, and likely other stupid things. Also likely add Ethernet support so you don't have two radio next to each other.

There is now a webserver onboard. Preferences are stored via the "Preferences" module so unless you entirely erase the board, settings are safe. It'll come up in "safe mode" so it doesn't transmit. WiFi is set to the lowest power and also sleep mode is enabled to help reduce RF interference.

AP mode for configuration will come up as "RadioParrot" and the password is "parrot123"

_**This will need external power as the 5v rail on an ESP32 isn't up to the task**_

And RF shielding. And a bunch of other I2S things for recording and playback, some caps and resistors, etc etc etc, likely some ferrite cores. 

Stuff gets weird when you have a high-gain antenna next to it. 

~~Yes, the robot voice is terrible. TERRIBLE ON PURPOSE.~~ The robot voice is now... Mid-Atlantic?!? I dunno. I like it.


