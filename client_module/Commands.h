#pragma once
#include <Arduino.h>
#include "config.h"
#include "Motors.h"
#include "Tracking.h"
#include "AudioPassthrough.h"
#if USE_TESTRUN
#include "TestRun.h"
#endif

namespace Commands {
  void begin(Stream& io);
  void poll();
  void println(const String& s);

  void setRequestSatelliteCallback(bool (*cb)());
  void setStatusPrinter(void (*cb)(Stream& s));
  void setStartStopCallbacks(bool (*startCb)(), void (*stopCb)());
  void setStepCallbacks(void (*stepAz)(int32_t), void (*stepEl)(int32_t));
  void setGotoCallbacks(void (*gotoAz)(float), void (*gotoEl)(float));

  // single, public injection point for UDP/web/other inputs
  void CommandsInject(const String& line);
}