#include "Commands.h"
#include <strings.h>

// Resolve references defined in the .ino:
extern String currentUTC;
extern unsigned long isoToUnix(const String& iso);

namespace Commands {

  // ---------- Module state ----------
  static Stream* io = nullptr;
  static String line;

  static bool (*reqSatCb)() = nullptr;
  static void (*statusPrinter)(Stream&) = nullptr;

  static bool (*startCb)() = nullptr;
  static void (*stopCb)() = nullptr;
  static void (*stepAzCb)(int32_t) = nullptr;
  static void (*stepElCb)(int32_t) = nullptr;
  static void (*gotoAzCb)(float) = nullptr;
  static void (*gotoElCb)(float) = nullptr;

  // ---------- utils ----------
  static String token(const String& s, int idx) {
    int start=0, end=0, found=-1;
    while (true) {
      while (start<(int)s.length() && s[start]==' ') start++;
      if (start >= (int)s.length()) break;
      end = s.indexOf(' ', start);
      if (end < 0) end = s.length();
      found++;
      if (found==idx) return s.substring(start,end);
      start = end+1;
    }
    return String();
  }
  static String upcopy(const String& s){ String u=s; u.toUpperCase(); return u; }

  static void help() {
    if (!io) return;
    io->println(F("Commands:"));
    io->println(F("  HELP / ?"));
    io->println(F("  START / STOP"));
    io->println(F("  HOME / HOME SET"));
    io->println(F("  STEP AZ <steps> / STEP EL <steps>"));
    io->println(F("  GOTO AZ <deg> / GOTO EL <deg>"));
    io->println(F("  LASER OFF|ON|TRACK"));
    io->println(F("  SAT NEW"));
    io->println(F("  AUDIO VOL <0-255>"));
    io->println(F("  AUDIO GAIN <mult>"));
    io->println(F("  AUDIO LIMIT <0-4095>"));
    io->println(F("  AUDIO IDLEMUTE ON|OFF"));
    io->println(F("  AUDIO ADCATTN <0|2|6|11>"));
    io->println(F("  AUDIO NOISE <ON|OFF> [mix 0-1] [floor 0-4095]"));
    io->println(F("  AUDIO NOTCH <ON|OFF> [hz] [q]"));
    io->println(F("  AUDIO RESET DEFAULTS / SAVE / LOAD / DELETE"));
    io->println(F("  BEEP VOL <0-255>"));
    io->println(F("  BEEP START <ON|OFF> / BEEP END <ON|OFF>"));
    io->println(F("  BEEP SET <freqHz> <durMs> <echoMs> <decay0-1> <vol0-255>"));
    io->println(F("  BEEP TEST"));
#if USE_TESTRUN
    io->println(F("  TEST START <az0> <az1> <peakEl> <durSec> [holdSec] [LOOP]"));
    io->println(F("  TEST PRESET <1|2|3>"));
    io->println(F("  TEST STOP / TEST STATUS"));
#endif
    io->println(F("  STATUS"));
  }

  // Make the handler NON-static so we can call it from CommandsInject too
  void handleImpl(const String& raw) {
    if (!io) return;
    String s=raw; s.trim(); if(!s.length()) return;
    String up = upcopy(s);

    if (up=="HELP" || up=="?") { help(); return; }

    if (up=="START") { if (startCb && startCb()) io->println(F("OK START")); else io->println(F("ERR START")); return; }
    if (up=="STOP")  { if (stopCb) stopCb(); else io->println(F("ERR STOP")); return; }

    if (up=="HOME") { motorsSetTrackingActive(false); motorsReturnToNull(); io->println(F("OK HOME")); return; }
    if (up=="HOME SET") { motorsZeroHere(); io->println(F("OK HOME SET")); return; }

    if (up.startsWith("STEP ")) {
      String axis = token(up,1);
      long steps = token(s,2).toInt();
      if (axis=="AZ") { if (stepAzCb) stepAzCb((int32_t)steps); else io->println(F("ERR STEP AZ")); }
      else if (axis=="EL") { if (stepElCb) stepElCb((int32_t)steps); else io->println(F("ERR STEP EL")); }
      else io->println(F("ERR STEP AZ|EL <steps>"));
      return;
    }

    if (up.startsWith("GOTO ")) {
      String axis = token(up,1);
      float deg = token(s,2).toFloat();
      if (axis=="AZ") { if (gotoAzCb) gotoAzCb(deg); else io->println(F("ERR GOTO AZ")); }
      else if (axis=="EL") { if (gotoElCb) gotoElCb(deg); else io->println(F("ERR GOTO EL")); }
      else io->println(F("ERR GOTO AZ|EL <deg>"));
      return;
    }

    if (up.startsWith("LASER ")) {
      String m = token(up,1);
      if (m=="OFF") { motorsSetLaserMode(LASER_OFF); io->println(F("OK LASER OFF")); return; }
      if (m=="ON")  { motorsSetLaserMode(LASER_ON);  io->println(F("OK LASER ON"));  return; }
      if (m=="TRACK"){motorsSetLaserMode(LASER_TRACK);io->println(F("OK LASER TRACK")); return; }
      io->println(F("ERR LASER <OFF|ON|TRACK>")); return;
    }

    // AUDIO
    if (up.startsWith("AUDIO VOL "))    { int v=token(s,2).toInt(); v=constrain(v,0,255); audioSetVolume(v); io->printf("OK VOL=%d\n", v); return; }
    if (up.startsWith("AUDIO GAIN "))   { float mult=token(s,2).toFloat(); if(mult<=0){io->println(F("ERR AUDIO GAIN <mult>"));return;} audioSetPTGain(mult); io->printf("OK AUDIO GAIN %.2f\n", mult); return; }
    if (up.startsWith("AUDIO LIMIT "))  { int lim=token(s,2).toInt(); lim=constrain(lim,0,4095); audioSetLimiter(lim); io->printf("OK LIMIT=%d\n", lim); return; }
    if (up.startsWith("AUDIO IDLEMUTE ")){ bool en=token(s,2).equalsIgnoreCase("ON"); audioSetMuteWhenIdle(en); io->printf("OK AUDIO IDLEMUTE %s\n", en?"ON":"OFF"); return; }
    if (up.startsWith("AUDIO ADCATTN ")){ int db=token(s,2).toInt(); if(db!=0&&db!=2&&db!=6&&db!=11){io->println(F("ERR AUDIO ADCATTN 0|2|6|11"));return;} audioSetADCAttenuation(db); io->printf("OK AUDIO ADCATTN %d\n", db); return; }
    if (up.startsWith("AUDIO NOISE "))  {
      String onTok=token(s,2); bool on=onTok.equalsIgnoreCase("ON");
      float mix=INJECT_NOISE_MIX; int floor=INJECT_NOISE_FLOOR; String t3=token(s,3),t4=token(s,4);
      if(t3.length()) mix=t3.toFloat(); if(t4.length()) floor=t4.toInt();
      mix=constrain(mix,0.0f,1.0f); floor=constrain(floor,0,4095);
      audioSetNoise(on,mix,floor); io->printf("OK NOISE %s mix=%.2f floor=%d\n", on?"ON":"OFF", mix, floor); return;
    }
    if (up.startsWith("AUDIO NOTCH "))  {
      String onTok=token(s,2); bool on=onTok.equalsIgnoreCase("ON");
      float hz=NOTCH_HZ, q=NOTCH_Q; String t3=token(s,3),t4=token(s,4);
      if(t3.length()) hz=t3.toFloat(); if(t4.length()) q=t4.toFloat();
      audioSetNotch(on,hz,q); io->printf("OK NOTCH %s f=%.1f q=%.2f\n", on?"ON":"OFF", hz, q); return;
    }
    if (up=="AUDIO RESET DEFAULTS") { audioResetToDefaults(); io->println(F("OK AUDIO DEFAULTS")); return; }
    if (up=="AUDIO SAVE")   { io->println(audioSaveSettingsToSD()?F("OK AUDIO SAVE"):F("ERR AUDIO SAVE")); return; }
    if (up=="AUDIO LOAD")   { io->println(audioLoadSettingsFromSD()?F("OK AUDIO LOAD"):F("ERR AUDIO LOAD")); return; }
    if (up=="AUDIO DELETE") { io->println(audioDeleteSettingsFromSD()?F("OK AUDIO DELETE"):F("ERR AUDIO DELETE")); return; }

    // BEEP
    if (up.startsWith("BEEP VOL "))     { int v=token(s,2).toInt(); v=constrain(v,0,255); audioBeepSetVolume((uint8_t)v); io->printf("OK BEEP VOL %d\n", v); return; }
    if (up.startsWith("BEEP START "))   { bool on=token(s,2).equalsIgnoreCase("ON"); audioBeepEnableStart(on); io->printf("OK BEEP START %s\n", on?"ON":"OFF"); return; }
    if (up.startsWith("BEEP END "))     { bool on=token(s,2).equalsIgnoreCase("ON"); audioBeepEnableEnd(on);   io->printf("OK BEEP END %s\n", on?"ON":"OFF");   return; }
    if (up.startsWith("BEEP SET "))     {
      float f; int d,ed; float dec; int v;
      if (sscanf(s.c_str()+9, "%f %d %d %f %d",&f,&d,&ed,&dec,&v)==5) {
        audioBeepSetParams(max(50.0f,f), max(10,d), max(0,ed), constrain(dec,0.0f,1.0f), (uint8_t)constrain(v,0,255));
        io->println(F("OK BEEP SET"));
      } else io->println(F("ERR BEEP SET <freqHz> <durMs> <echoMs> <decay0-1> <vol0-255>"));
      return;
    }
    if (up=="BEEP TEST") { audioBeepTest(); io->println(F("OK BEEP TEST")); return; }

    if (up=="SAT NEW") {
      if (reqSatCb) io->println(reqSatCb()?F("OK SAT NEW"):F("ERR SAT NEW"));
      else io->println(F("ERR no SAT NEW callback set"));
      return;
    }

#if USE_TESTRUN
    if (up.startsWith("TEST START ")) {
      float az0=0, az1=0, pel=0; long dur=0, hold=0; char loopTxt[8]={0};
      int n=sscanf(s.c_str()+11,"%f %f %f %ld %ld %7s",&az0,&az1,&pel,&dur,&hold,loopTxt);
      bool loop=(n>=6 && strcasecmp(loopTxt,"LOOP")==0);
      if (n<4){ io->println(F("ERR TEST START <az0> <az1> <peakEl> <durSec> [holdSec] [LOOP]")); return; }
      if (n<5) hold=0;
      TestRun::configure(az0,az1,pel,(uint32_t)dur,(uint32_t)hold,loop);
      TestRun::enable(true);
      unsigned long ut=isoToUnix(currentUTC);
      if (!ut) TestRun::startNow(); else TestRun::startAt((time_t)ut);
      io->println(F("OK TEST START")); return;
    }
    if (up=="TEST STOP")   { TestRun::enable(false); io->println(F("OK TEST STOP")); return; }
    if (up=="TEST STATUS") { TestRun::printStatus(*io); return; }
    if (up.startsWith("TEST PRESET ")) {
      int id=token(s,2).toInt();
      if (id==1) TestRun::configure(220,320,45,180,10,true);
      else if (id==2) TestRun::configure( 90,270,70,240, 0,false);
      else if (id==3) TestRun::configure(350, 30,30,120, 5,true);
      else { io->println(F("ERR TEST PRESET 1|2|3")); return; }
      TestRun::enable(true);
      unsigned long ut=isoToUnix(currentUTC);
      if (!ut) TestRun::startNow(); else TestRun::startAt((time_t)ut);
      io->printf("OK TEST PRESET %d\n", id); return;
    }
#endif

    if (up=="STATUS") { if (statusPrinter) statusPrinter(*io); else io->println(F("STATUS printer not set")); return; }

    help();
  }

  // ---------- public API ----------
  void begin(Stream& s) { io=&s; line.reserve(256); }
  void setRequestSatelliteCallback(bool (*cb)()) { reqSatCb=cb; }
  void setStatusPrinter(void (*cb)(Stream& s))   { statusPrinter=cb; }
  void setStartStopCallbacks(bool (*sCb)(), void (*pCb)()) { startCb=sCb; stopCb=pCb; }
  void setStepCallbacks(void (*az)(int32_t), void (*el)(int32_t)) { stepAzCb=az; stepElCb=el; }
  void setGotoCallbacks(void (*az)(float), void (*el)(float)) { gotoAzCb=az; gotoElCb=el; }
  void println(const String& s){ if(io) io->println(s); }

  // Single, public injection function (UDP/web)
  void CommandsInject(const String& cmdLine) {
    handleImpl(cmdLine);
  }

  void poll() {
    if (!io) return;
    while (io->available()) {
      char c=(char)io->read();
      if (c=='\r') continue;
      if (c=='\n') { handleImpl(line); line=""; }
      else { line+=c; if (line.length()>512) line.remove(0,256); }
    }
  }
} // namespace