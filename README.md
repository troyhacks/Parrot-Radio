# Doing stupid tricks with the SA868 radio module from NiceRF

<img width="50%" height="50%" alt="A pirate parrot holding a fancy walkie-talkie" src="https://github.com/user-attachments/assets/26e68f07-50eb-4d0b-9f52-cfbfe3091408" />

*Work in progress*, meant as an automated walkie-check system for events. 

It assumes 4MB of PSRAM, and stores your last 8 radio tests. You can recall them by sending DTMF 1..9 from your radio. 

These are overwritten as new ones come in, as a circular buffer (so if your recording is in #5, it'll be in #5 until it's overwritten.)

Recordings are flushed on reboot - temporary memory only, except for the embedded test file. 

DTMF 9 will transmit a clean audio file (encoded in the firmware) so you can see how you're receiving a clean transmit.

TODOs include confirming the radio debug message logic, doing real range testing, maybe weather/temperature sensor reports, scheduled "if you hear this your walkie is working" messages, and likely other stupid things.

_**This will likely need external power as the 5v rail on an ESP32 isn't up to the task**_

And RF shielding. And a bunch of other I2S things for recording and playback, some caps and resistors, etc etc etc, likely some ferrite cores. 

Stuff gets weird when you have a high-gain antenna next to it. 

~~Yes, the robot voice is terrible. TERRIBLE ON PURPOSE.~~ The robot voice is now... Mid-Atlantic?!? I dunno. I like it.


