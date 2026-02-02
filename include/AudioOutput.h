#ifndef _AUDIOOUTPUT_H
#define _AUDIOOUTPUT_H

#include <Arduino.h>

class AudioOutput {
public:
  virtual ~AudioOutput() {}
  virtual bool begin() { return true; }
  virtual bool ConsumeSample(int16_t sample[2]) = 0;
  virtual bool stop() { return true; }
  virtual bool SetRate(int hz) { return true; }
  virtual bool SetBitsPerSample(int bits) { return true; }
  virtual bool SetChannels(int channels) { return true; }
  virtual bool SetGain(float gain) { return true; }
};

#endif
