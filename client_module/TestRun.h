#pragma once
#include <Arduino.h>

namespace TestRun {
  void begin();
  void enable(bool en);
  bool isEnabled();
  void configure(float azStart, float azEnd, float peakElDeg, uint32_t durSec, uint32_t holdSec = 0, bool loop=false);
  void startAt(time_t unixStart);
  void startNow();
  bool getAzEl(time_t utcNow, float& azOut, float& elOut);
  void printStatus(Stream& s);
}