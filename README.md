# Doing stupid tricks with the SA868 radio module from NiceRF

<img width="50%" height="50%" alt="A pirate parrot holding a fancy walkie-talkie" src="https://github.com/user-attachments/assets/26e68f07-50eb-4d0b-9f52-cfbfe3091408" />

*Work in progress*, meant as an automated walkie-check system for events. 

It assumes 4MB of PSRAM, and stores your last 8 radio tests. 

These are overwritten as new ones come in, as a circular buffer (so if your recording is in #5, it'll be in #5 until it's overwritten.)

Recordings are flushed on reboot - temporary memory only, except for the embedded test file. 

You can now specify pre/post message strings for the TTS. These support various variable expansions. 

DTMF 1..8 will recall that particular radio test. 
DTMF 9 will transmit a clean audio file (encoded in the firmware) so you can see how you're receiving a clean transmit.
DTMF * will transmit a read of your local weather conditions.
DTMF # will transmit a customized message. (no pre/post messsages for this one)
DTMF A,B,C,D are yet to be defined. (If you didn't know there's A,B,C,D in DTMF, you're too young.)

TODOs include confirming the radio debug message logic, doing real range testing, scheduled "if you hear this your walkie is working" messages, and likely other stupid things. Also likely add Ethernet support so you don't have two radio next to each other.

There is now a webserver onboard. Preferences are stored via the "Preferences" module so unless you entirely erase the board, settings are safe. It'll come up in "safe mode" so it doesn't transmit. WiFi is set to the lowest power and also sleep mode is enabled to help reduce RF interference.

AP mode for configuration will come up as "RadioParrot" and the password is "parrot123"

_**This will need external power as the 5v rail on an ESP32 isn't up to the task**_

And RF shielding. And a bunch of other I2S things for recording and playback, some caps and resistors, etc etc etc, likely some ferrite cores. 

Stuff gets weird when you have a high-gain antenna next to it. 

~~Yes, the robot voice is terrible. TERRIBLE ON PURPOSE.~~ The robot voice is now... Mid-Atlantic?!? I dunno. I like it.


