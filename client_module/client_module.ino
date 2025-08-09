#include "config.h"
#include "Motors.h"
#include "AudioPassthrough.h"
#include "Tracking.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>  // struct tm, mktime

// -------------------- Networking --------------------
WiFiClient client;
WiFiUDP udp;

// UDP time from master as ISO string
String currentUTC;

// -------------------- Modes --------------------
enum Mode : uint8_t { MODE_WAIT = 0, MODE_TRACK, MODE_SWEEP, MODE_DEMO };
static Mode mode = MODE_WAIT;

// TRACK retry timing
unsigned long lastSatRequest = 0;

// Manual time override for TRACK (0 = use UDP)
static unsigned long manualUnixTime = 0;

// Local gating for start/end beeps (so serial can toggle them live)
static bool beepStartOn = BEEP_ON_TRACK_START;
static bool beepEndOn   = BEEP_ON_TRACK_END;

// --- Serial command parser buffer ---
static String cmdBuf;

// =====================================================
// Helpers
// =====================================================
static unsigned long isoToUnix(const String &iso) {
  struct tm t = {};
  int y,M,d,h,m,s;
  if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%dZ", &y,&M,&d,&h,&m,&s) != 6) return 0UL;
  t.tm_year = y - 1900;
  t.tm_mon  = M - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min  = m;
  t.tm_sec  = s;
  t.tm_isdst = -1;
  return mktime(&t);
}

static bool requestSatellite(bool afterComplete) {
  if (!client.connect(MASTER_IP, TCP_PORT)) {
    if (DEBUG) Serial.println(F("❌ Failed to connect to master"));
    return false;
  }
  if (afterComplete) client.println(F("{\"status\":\"complete\"}"));

  String satData = client.readStringUntil('\n');
  client.stop();

  if (satData.length() == 0) {
    if (DEBUG) Serial.println(F("⚠ No satellite data from master"));
    return false;
  }

  StaticJsonDocument<1024> doc;
  auto err = deserializeJson(doc, satData);
  if (err) {
    if (DEBUG) { Serial.print(F("❌ JSON parse error: ")); Serial.println(err.c_str()); }
    return false;
  }

  String name = doc["name"].as<String>();
  String tle1 = doc["tle"]["line-1"].as<String>();
  String tle2 = doc["tle"]["line-2"].as<String>();
  double lat = doc["latitude"].as<double>();
  double lon = doc["longitude"].as<double>();

  trackingInit(name.c_str(), tle1.c_str(), tle2.c_str(), lat, lon, 0.0);
  if (DEBUG) { Serial.print(F("📡 Tracking: ")); Serial.println(name); }
  return true;
}

// Preset test TLEs (mechanical/demo only)
static void loadPresetTLE(int id) {
  const char* name = "TEST";
  const char* L1 = "";
  const char* L2 = "";

  switch (id) {
    case 1:
      name = "ISS (TEST)";
      L1 = "1 25544U 98067A   16065.25775256 -.00164574  00000-0 -25195-2 0  9990";
      L2 = "2 25544  51.6436 216.3171 0002750 185.0333 238.0864 15.54246933988812";
      break;
    case 2:
      name = "NOAA 18 (TEST)";
      L1 = "1 28654U 05018A   16065.47251769  .00000077  00000-0  68629-4 0  9995";
      L2 = "2 28654  99.0037  76.9243 0014957  34.3811 325.8203 14.12512534702256";
      break;
    case 3:
      name = "HST (TEST)";
      L1 = "1 20580U 90037B   16065.51704939  .00000833  00000-0  00000+0 0  9998";
      L2 = "2 20580  28.4690  51.6243 0002845  38.0202 322.0847 15.09170411303941";
      break;
    default:
      Serial.println(F("ERR preset id. Use 1=ISS 2=NOAA18 3=HST"));
      return;
  }

  double lat, lon, alt;
  trackingGetCurrentSite(lat, lon, alt);
  if (!trackingReady() || (lat == 0 && lon == 0)) {
    trackingSetSite(52.5200, 13.4050, 35.0); // Berlin default for tests
  }
  trackingSetTLE(name, L1, L2);
  Serial.printf("OK preset loaded: %s\n", name);
}

// =====================================================
// SWEEP mode state (with PINGPONG)
// =====================================================
static bool  sweepActive   = false;
static bool  sweepPingPong = false;
static char  sweepAxis     = 'A'; // 'A' for AZ, 'E' for EL
static float sweepStart    = 0.0f;
static float sweepEnd      = 0.0f;
static float sweepRateDps  = 5.0f; // deg/sec
static unsigned long sweepT0 = 0;  // millis

static void startSweep(char axis, float startDeg, float endDeg, float rateDps, bool pingpong) {
  if (axis != 'A' && axis != 'E') { Serial.println(F("ERR axis must be AZ or EL")); return; }
  sweepAxis = axis;
  sweepStart = startDeg;
  sweepEnd   = endDeg;
  sweepRateDps = fabs(rateDps);
  sweepPingPong = pingpong;
  sweepT0 = millis();
  sweepActive = true;
  mode = MODE_SWEEP;
  motorsSetTrackingActive(false); // not tracking in SWEEP
  Serial.printf("OK SWEEP %cZ start=%.2f end=%.2f rate=%.2f dps%s\n",
                axis, startDeg, endDeg, sweepRateDps, pingpong?" PINGPONG":"");
  // Move immediately to start
  if (axis == 'A') motorsMoveTo(sweepStart, EL_MIN_DEG);
  else             motorsMoveTo(0.0f, sweepStart);
}

static void reverseSweepLeg(unsigned long nowMs) {
  float tmp = sweepStart;
  sweepStart = sweepEnd;
  sweepEnd = tmp;
  sweepT0 = nowMs;
  if (DEBUG) Serial.println(F("[SWEEP] Reversing (ping-pong)"));
}

static void stopSweep() {
  if (!sweepActive) return;
  sweepActive = false;
  Serial.println(F("OK SWEEP STOP"));
  mode = MODE_WAIT;
}

// =====================================================
// DEMO ORBIT mode state
// =====================================================
static bool demoActive = false;
static float demoAzRateDps = 10.0f;   // azimuth rate (deg/sec)
static float demoElCenter  = 45.0f;   // elevation center
static float demoElAmp     = 30.0f;   // elevation amplitude
static float demoElRateHz  = 0.05f;   // elevation oscillation rate (Hz)
static unsigned long demoT0 = 0;      // millis
static float demoAz0 = 0.0f;

static void startDemoOrbit(float azRateDps, float elCenter, float elAmp, float elRateHz) {
  demoAzRateDps = azRateDps;
  demoElCenter  = elCenter;
  demoElAmp     = fabs(elAmp);
  demoElRateHz  = fabs(elRateHz);
  demoT0 = millis();
  demoAz0 = 0.0f;
  demoActive = true;
  mode = MODE_DEMO;
  motorsSetTrackingActive(false); // not tracking in DEMO
  Serial.printf("OK DEMO ORBIT azRate=%.2f dps, el=%.1f±%.1f @ %.3f Hz\n",
                demoAzRateDps, demoElCenter, demoElAmp, demoElRateHz);
}

static void stopDemoOrbit() {
  if (!demoActive) return;
  demoActive = false;
  Serial.println(F("OK DEMO STOP"));
  mode = MODE_WAIT;
}

// =====================================================
// Serial commands (audio + beep + laser + tracking + sweep/demo)
// =====================================================
static void handleCommand(const String& line) {
  String cmd = line; cmd.trim();
  if (cmd.length() == 0) return;
  String up = cmd; up.toUpperCase();

  // ---- Audio controls ----
  if (up.startsWith("VOL ")) {
    int v = up.substring(4).toInt();
    v = constrain(v, 0, 255);
    audioSetVolume((uint8_t)v);
    Serial.printf("OK VOL=%d\n", v);
    return;
  }
  if (up.startsWith("GATE ")) {
    int a, b;
    if (sscanf(up.c_str()+5, "%d %d", &a, &b) == 2) {
      audioSetGate(a, b);
      Serial.printf("OK GATE open=%d close=%d\n", a, b);
    } else Serial.println("ERR usage: GATE <open> <close>");
    return;
  }
  if (up.startsWith("LIMIT ")) {
    int lim = up.substring(6).toInt();
    audioSetLimiter(lim);
    Serial.printf("OK LIMIT=%d\n", lim);
    return;
  }
  if (up.startsWith("NOISE ")) {
    if (up.indexOf("OFF") > 0) { audioSetNoise(false, 0, 0); Serial.println("OK NOISE=OFF"); }
    else {
      float mix = 0.1f; int floorv = 180;
      int n = sscanf(up.c_str()+6, "ON %f %d", &mix, &floorv);
      if (n >= 1) { if (n == 1) floorv = 180; audioSetNoise(true, mix, floorv);
        Serial.printf("OK NOISE=ON mix=%.2f floor=%d\n", mix, floorv);
      } else Serial.println("ERR usage: NOISE ON <mix 0..1> <floor>  | NOISE OFF");
    }
    return;
  }
  if (up.startsWith("NOTCH ")) {
    if (up.indexOf("OFF") > 0) { audioSetNotch(false, 0, 0); Serial.println("OK NOTCH=OFF"); }
    else {
      float hz=2600, q=12; int n = sscanf(up.c_str()+6, "ON %f %f", &hz, &q);
      if (n >= 1) { if (n == 1) q = 12; audioSetNotch(true, hz, q);
        Serial.printf("OK NOTCH=ON f=%.1f Q=%.1f\n", hz, q);
      } else Serial.println("ERR usage: NOTCH ON <hz> <q>  | NOTCH OFF");
    }
    return;
  }
  if (up == "STATUS") { audioPrintStatus(Serial); return; }
  if (up == "SAVESET") { Serial.println(audioSaveSettingsToSD() ? "OK saved /audio_settings.json" : "ERR saving settings"); return; }
  if (up == "LOADSET") { Serial.println(audioLoadSettingsFromSD() ? "OK loaded /audio_settings.json" : "ERR loading settings"); return; }
  if (up == "RESETSET") { audioResetToDefaults(); Serial.println("OK settings restored to defaults."); return; }
  if (up == "FACTORYRESET") { bool ok = audioDeleteSettingsFromSD(); audioResetToDefaults(); Serial.println(ok ? "OK deleted /audio_settings.json and reset defaults" : "ERR factory reset"); return; }

  // ---- Beep controls ----
  if (up.startsWith("BEEP SET ")) {
    float f=800.0f, decay=0.5f; int dur=150, edel=120, vol=180;
    int n = sscanf(cmd.c_str()+9, "%f %d %d %f %d", &f, &dur, &edel, &decay, &vol);
    if (n >= 1) {
      if (n < 2) dur=150; if (n < 3) edel=120; if (n < 4) decay=0.5f; if (n < 5) vol=180;
      audioBeepSetParams(f,dur,edel,decay,(uint8_t)constrain(vol,0,255));
      Serial.printf("OK BEEP SET f=%.1f dur=%d echoDelay=%d decay=%.2f vol=%d\n", f,dur,edel,decay,vol);
    } else Serial.println("ERR usage: BEEP SET <freqHz> <durMs> <echoDelayMs> <echoDecay0..1> <vol0..255>");
    return;
  }
  if (up == "BEEP START ON")  { beepStartOn = true;  audioBeepEnableStart(true);  Serial.println("OK BEEP START=ON");  return; }
  if (up == "BEEP START OFF") { beepStartOn = false; audioBeepEnableStart(false); Serial.println("OK BEEP START=OFF"); return; }
  if (up == "BEEP END ON")    { beepEndOn   = true;  audioBeepEnableEnd(true);    Serial.println("OK BEEP END=ON");    return; }
  if (up == "BEEP END OFF")   { beepEndOn   = false; audioBeepEnableEnd(false);   Serial.println("OK BEEP END=OFF");   return; }
  if (up == "BEEP TEST")      { audioBeepTest();     Serial.println("OK BEEP TEST");   return; }

  // ---- Laser controls ----
  if (up.startsWith("LASER MODE ")) {
    if (up.endsWith("OFF"))       { motorsSetLaserMode(LASER_OFF);   Serial.println("OK LASER MODE OFF"); }
    else if (up.endsWith("ON"))   { motorsSetLaserMode(LASER_ON);    Serial.println("OK LASER MODE ON"); }
    else if (up.endsWith("TRACK")){ motorsSetLaserMode(LASER_TRACK); Serial.println("OK LASER MODE TRACK"); }
    else Serial.println("ERR usage: LASER MODE OFF|ON|TRACK");
    return;
  }
  if (up == "LASER STATUS") {
    LaserMode m = motorsGetLaserMode();
    Serial.printf("LASER MODE: %s\n", m==LASER_OFF?"OFF":(m==LASER_ON?"ON":"TRACK"));
    return;
  }

  // ---- Tracking / Motion ----
  if (up == "HOME") { motorsReturnToNull(); Serial.println(F("OK HOME")); mode = MODE_WAIT; motorsSetTrackingActive(false); return; }
  if (up == "NEXTSAT") {
    bool ok = requestSatellite(true);
    if (ok) { mode = MODE_TRACK; motorsSetTrackingActive(true); if (beepStartOn) audioBeepPlay(); Serial.println(F("OK NEXTSAT")); }
    else    { mode = MODE_WAIT;  motorsSetTrackingActive(false); Serial.println(F("ERR NEXTSAT")); }
    return;
  }
  if (up == "TRACK OFF") { mode = MODE_WAIT; motorsSetTrackingActive(false); Serial.println(F("OK TRACK OFF")); return; }
  if (up == "TRACK ON")  {
    if (trackingReady()) { mode = MODE_TRACK; motorsSetTrackingActive(true); if (beepStartOn) audioBeepPlay(); Serial.println(F("OK TRACK ON")); }
    else                 { mode = MODE_WAIT;  motorsSetTrackingActive(false); Serial.println(F("ERR TRACK ON (not ready)")); }
    return;
  }

  if (up.startsWith("SETSITE ")) {
    double la=0, lo=0, al=0;
    int n = sscanf(cmd.c_str()+8, "%lf %lf %lf", &la, &lo, &al);
    if (n >= 2) { if (n == 2) al = 0; trackingSetSite(la, lo, al);
      Serial.printf("OK SETSITE lat=%.6f lon=%.6f alt=%.1f\n", la, lo, al);
    } else Serial.println(F("ERR usage: SETSITE <lat> <lon> [alt]"));
    return;
  }

  if (up.startsWith("SETTIME ")) {
    String iso = cmd.substring(8); iso.trim();
    unsigned long u = isoToUnix(iso);
    if (u) { manualUnixTime = u; Serial.printf("OK SETTIME %s (unix=%lu)\n", iso.c_str(), u); }
    else Serial.println(F("ERR time format. Use YYYY-MM-DDTHH:MM:SSZ"));
    return;
  }
  if (up == "TIME NOW") { manualUnixTime = 0; Serial.println(F("OK TIME NOW (use UDP)")); return; }

  if (up.startsWith("LOADTLE ")) {
    // Format: LOADTLE name|line1|line2
    String body = cmd.substring(8);
    int p1 = body.indexOf('|');
    int p2 = (p1 >= 0) ? body.indexOf('|', p1+1) : -1;
    if (p1 > 0 && p2 > p1) {
      String nm = body.substring(0, p1);
      String l1 = body.substring(p1+1, p2);
      String l2 = body.substring(p2+1);
      trackingSetTLE(nm.c_str(), l1.c_str(), l2.c_str());
      Serial.println(F("OK LOADTLE"));
    } else Serial.println(F("ERR usage: LOADTLE name|line1|line2"));
    return;
  }

  if (up.startsWith("TESTSAT ")) { int id = cmd.substring(8).toInt(); loadPresetTLE(id); return; }

  if (up.startsWith("MOVE AZEL ")) {
    float az=0, el=0;
    if (sscanf(cmd.c_str()+9, "%f %f", &az, &el) == 2) { motorsMoveTo(az, el); Serial.printf("OK MOVE AZEL %.2f %.2f\n", az, el); }
    else Serial.println(F("ERR usage: MOVE AZEL <az> <el>"));
    return;
  }
  if (up.startsWith("MOVE AZ ")) {
    float az=0;
    if (sscanf(cmd.c_str()+8, "%f", &az) == 1) { motorsMoveTo(az, EL_MIN_DEG); Serial.printf("OK MOVE AZ %.2f\n", az); }
    else Serial.println(F("ERR usage: MOVE AZ <deg>"));
    return;
  }
  if (up.startsWith("MOVE EL ")) {
    float el=0;
    if (sscanf(cmd.c_str()+8, "%f", &el) == 1) { motorsMoveTo(0.0f, el); Serial.printf("OK MOVE EL %.2f\n", el); }
    else Serial.println(F("ERR usage: MOVE EL <deg>"));
    return;
  }

  if (up == "STATUSTRK") {
    String nm,l1,l2; trackingGetCurrentTLE(nm,l1,l2);
    double la,lo,al; trackingGetCurrentSite(la,lo,al);
    Serial.println(F("=== Tracking Status ==="));
    Serial.printf("READY    : %s\n", trackingReady() ? "YES":"NO");
    Serial.printf("MODE     : %s\n", mode==MODE_TRACK?"TRACK":(mode==MODE_SWEEP?"SWEEP":(mode==MODE_DEMO?"DEMO":"WAIT")));
    Serial.printf("SITE     : lat=%.6f lon=%.6f alt=%.1f\n", la, lo, al);
    Serial.printf("TLE NAME : %s\n", nm.c_str());
    Serial.printf("TIME SRC : %s\n", manualUnixTime ? "MANUAL" : "UDP");
    Serial.printf("MANUAL   : %lu\n", manualUnixTime);
    return;
  }

  // ---- SWEEP (with optional PINGPONG) ----
  if (up.startsWith("SWEEP AZ ")) {
    float s=0,e=0,r=0; bool ping=false;
    if (up.endsWith(" PINGPONG")) ping = true;
    if (sscanf(cmd.c_str()+8, "%f %f %f", &s,&e,&r) == 3) startSweep('A', s,e,r,ping);
    else Serial.println(F("ERR usage: SWEEP AZ <start> <end> <deg_per_sec> [PINGPONG]"));
    return;
  }
  if (up.startsWith("SWEEP EL ")) {
    float s=0,e=0,r=0; bool ping=false;
    if (up.endsWith(" PINGPONG")) ping = true;
    if (sscanf(cmd.c_str()+9, "%f %f %f", &s,&e,&r) == 3) startSweep('E', s,e,r,ping);
    else Serial.println(F("ERR usage: SWEEP EL <start> <end> <deg_per_sec> [PINGPONG]"));
    return;
  }
  if (up == "SWEEP STOP") { stopSweep(); return; }

  // ---- DEMO ORBIT ----
  if (up.startsWith("DEMO ORBIT ")) {
    float azr=10, elc=45, ela=30, elrh=0.05f;
    if (sscanf(cmd.c_str()+11, "%f %f %f %f", &azr, &elc, &ela, &elrh) >= 1) {
      if (!isfinite(elc)) elc = 45;
      if (!isfinite(ela)) ela = 30;
      if (!isfinite(elrh)) elrh = 0.05f;
      startDemoOrbit(azr, elc, ela, elrh);
    } else {
      Serial.println(F("ERR usage: DEMO ORBIT <az_deg_per_sec> <el_center> <el_amp> <el_rate_hz>"));
    }
    return;
  }
  if (up == "DEMO STOP") { stopDemoOrbit(); return; }

  Serial.println(F("Unknown command. See README for full list."));
}

// =====================================================
// Arduino
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  // SD first (position restore happens in motorsInit)
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) Serial.println(F("❌ SD init failed"));
  else if (DEBUG)       Serial.println(F("✅ SD card ready"));

  motorsInit();
  motorsSetTrackingActive(false); // ensure laser obeys mode at boot
  audioInit();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("Connecting to WiFi"));
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(400); Serial.print('.');
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? F("\n✅ WiFi connected") : F("\n❌ WiFi failed"));

  udp.begin(UDP_PORT);

  // First assignment
  if (requestSatellite(false)) {
    mode = MODE_TRACK;
    motorsSetTrackingActive(true);
    if (beepStartOn) audioBeepPlay();
  } else {
    mode = MODE_WAIT;
    motorsSetTrackingActive(false);
  }
  lastSatRequest = millis();
}

void loop() {
  // --- read serial line ---
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { handleCommand(cmdBuf); cmdBuf = ""; }
    else cmdBuf += c;
  }

  // Audio always-on
  audioLoop();

  // Time sync from UDP
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char buf[256];
    int len = udp.read(buf, sizeof(buf) - 1);
    if (len > 0) buf[len] = '\0';
    StaticJsonDocument<256> tdoc;
    if (!deserializeJson(tdoc, buf)) {
      currentUTC = tdoc["current_time_utc"].as<String>();
      if (DEBUG) { Serial.print(F("⏰ UTC ")); Serial.println(currentUTC); }
    }
  }

  // -------- Mode FSM --------
  const unsigned long nowMs = millis();

  if (mode == MODE_WAIT) {
    motorsSetTrackingActive(false);
    // Retry for next satellite periodically
    if (nowMs - lastSatRequest >= SAT_REQUEST_INTERVAL_MS) {
      lastSatRequest = nowMs;
      if (DEBUG) Serial.println(F("🔁 Requesting next satellite..."));
      if (requestSatellite(true)) {
        mode = MODE_TRACK;
        motorsSetTrackingActive(true);
        if (beepStartOn) audioBeepPlay();
      } else {
        if (DEBUG) Serial.println(F("⏳ No satellite available; will retry."));
      }
    }
    return;
  }

  if (mode == MODE_TRACK) {
    unsigned long unixTime = 0;
    if (manualUnixTime) unixTime = manualUnixTime;
    else if (currentUTC.length() > 0) unixTime = isoToUnix(currentUTC);
    if (!unixTime) return;

    float az, el;
    if (trackingGetAzEl(unixTime, az, el)) {
      if (DEBUG) { Serial.printf("[TRK] AZ=%.2f  EL=%.2f\n", az, el); }
      motorsMoveTo(az, el);
    } else {
      if (DEBUG) Serial.println(F("🌅 Below horizon -> home + request next"));
      if (beepEndOn) audioBeepPlay();
      motorsSetTrackingActive(false);
      motorsReturnToNull();
      mode = MODE_WAIT;
      lastSatRequest = 0; // force immediate request on next loop
    }
    return;
  }

  if (mode == MODE_SWEEP) {
    if (!sweepActive) { mode = MODE_WAIT; return; }
    float dt = (nowMs - sweepT0) / 1000.0f;
    float span = sweepEnd - sweepStart;
    float dir = (span >= 0) ? +1.0f : -1.0f;
    float maxTravel = sweepRateDps * dt * dir;
    float target = sweepStart + maxTravel;

    // Clamp to end
    bool reached = (dir > 0 && target >= sweepEnd) || (dir < 0 && target <= sweepEnd);
    if (reached) {
      target = sweepEnd;
      if (sweepAxis == 'A') motorsMoveTo(target, EL_MIN_DEG);
      else                  motorsMoveTo(0.0f, target);

      if (sweepPingPong) {
        reverseSweepLeg(nowMs);
        return; // next leg
      } else {
        sweepActive = false;
        Serial.println(F("SWEEP done"));
        mode = MODE_WAIT;
        return;
      }
    }

    if (sweepAxis == 'A') motorsMoveTo(target, EL_MIN_DEG);
    else                  motorsMoveTo(0.0f, target);
    return;
  }

  if (mode == MODE_DEMO) {
    if (!demoActive) { mode = MODE_WAIT; return; }
    float t = (nowMs - demoT0) / 1000.0f;
    float az = fmodf(demoAz0 + demoAzRateDps * t, 360.0f);
    if (az < 0) az += 360.0f;
    float el = demoElCenter + demoElAmp * sinf(2.0f * PI * demoElRateHz * t);
    if (el < EL_MIN_DEG) el = EL_MIN_DEG;
    if (el > EL_MAX_DEG) el = EL_MAX_DEG;
    motorsMoveTo(az, el);
    return;
  }
}
