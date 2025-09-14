#include "TestRun.h"
#include <sys/time.h>
#include <math.h>

namespace TestRun {

  static bool gEnabled = false;
  static bool gLoop = false;

  static float gAzStart = 0.0f, gAzEnd = 180.0f, gPeakEl = 45.0f;
  static uint32_t gDurSec = 120;
  static uint32_t gHoldSec = 0;

  static time_t gStartUnix = 0;
  static bool gStarted = false;

  static float norm360(float a){ while(a<0) a+=360.0f; while(a>=360.0f) a-=360.0f; return a; }

  void begin() {}
  void enable(bool en){ gEnabled = en; if(!en) gStarted=false; }
  bool isEnabled(){ return gEnabled; }

  void configure(float azStart, float azEnd, float peakElDeg, uint32_t durSec, uint32_t holdSec, bool loop){
    gAzStart = norm360(azStart);
    gAzEnd   = norm360(azEnd);
    gPeakEl  = constrain(peakElDeg, 0.0f, 90.0f);
    gDurSec  = max<uint32_t>(durSec, 10);
    gHoldSec = holdSec;
    gLoop    = loop;
  }

  void startAt(time_t unixStart){ gStartUnix = unixStart; gStarted = true; }
  void startNow(){
    time_t nowSec = 0;
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) == 0) nowSec = tv.tv_sec;
    else nowSec = millis()/1000;
    startAt(nowSec);
  }

  bool getAzEl(time_t utcNow, float& azOut, float& elOut){
    if (!gEnabled) return false;
    if (!gStarted) startAt(utcNow);

    long dt = (long)utcNow - (long)gStartUnix;
    if (dt < 0) dt = 0;

    if ((uint32_t)dt > (gDurSec + gHoldSec)) {
      if (gLoop) {
        gStartUnix = utcNow;
        dt = 0;
      } else {
        elOut = -5.0f; azOut = gAzEnd;
        return false;
      }
    }

    float t = constrain((float)dt / (float)gDurSec, 0.0f, 1.0f);

    float a0 = gAzStart;
    float d  = gAzEnd - gAzStart;
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    float az = norm360(a0 + d * t);

    float el = gPeakEl * (1.0f - cosf(2.0f * PI * t)) * 0.5f;
    if ((uint32_t)dt > gDurSec) { az = gAzEnd; el = 0.0f; }

    azOut = az; elOut = el;
    return el >= 0.0f || (uint32_t)dt <= (gDurSec + gHoldSec);
  }

  void printStatus(Stream& s){
    s.printf("TestRun: %s, az %.1f->%.1f, peakEl=%.1f, dur=%us hold=%us, loop=%s, started=%s\n",
      gEnabled?"ENABLED":"DISABLED",
      gAzStart, gAzEnd, gPeakEl, (unsigned)gDurSec, (unsigned)gHoldSec,
      gLoop?"yes":"no", gStarted?"yes":"no");
  }

} // namespace