/**************************************************************
  Satellite Tracker – Client
**************************************************************/
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <time.h>

#include "config.h"
#include "Motors.h"
#include "Tracking.h"
#include "AudioPassthrough.h"
#include "Commands.h"
#if USE_TESTRUN
#include "TestRun.h"
#endif


// --- Sockets ---
WiFiClient client;
WiFiUDP udp;         // time sync (UDP_PORT, e.g. 4210)
WiFiUDP udpCmd;      // command listener (UDP_CMD_PORT, e.g. 4212)
WiFiUDP udpReg;      // registrar HELLO/PING (SERVER_REG_PORT, e.g. 4213)

// --- State ---
String  gModuleName;      // MODULE-xxxxxx
char    udpBuffer[256];

String currentUTC;
String curSatName, curTLE1, curTLE2;

double obsLat = 0.0, obsLon = 0.0, obsAlt = 0.0;

enum Mode { MODE_WAIT, MODE_TRACK, MODE_HOME, MODE_STOP } mode = MODE_WAIT;
static bool gHasLock = false; // becomes true after first valid above-horizon point

unsigned long lastSatRequest = 0;
const unsigned long satRequestIntervalMs = 50;

SPIClass spiSD(VSPI);



// Elevation window considered “trackable” (match your mechanical limits)
static const float EL_MIN = 0.0f;     // horizon
static const float EL_MAX = 180.0f;   // horizon stp

// ===== Helpers =====
unsigned long isoToUnix(const String &iso) {
  if (iso.length() < 19) return 0;
  int y = iso.substring(0,4).toInt();
  int M = iso.substring(5,7).toInt();
  int d = iso.substring(8,10).toInt();
  int h = iso.substring(11,13).toInt();
  int m = iso.substring(14,16).toInt();
  int s = iso.substring(17,19).toInt();

  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = M - 1; t.tm_mday = d;
  t.tm_hour = h; t.tm_min = m; t.tm_sec = s; t.tm_isdst = 0;

  setenv("TZ", "UTC", 1); tzset();
  time_t ut = mktime(&t);           // mktime with TZ=UTC behaves like timegm
  return (unsigned long)ut;
}

// Network
void printWiFiDebug() {
  Serial.println(F("[NET] ---------"));
  Serial.printf("[NET] STA status: %d (3=WL_CONNECTED)\n", WiFi.status());
  Serial.printf("[NET] Local IP  : %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[NET] Gateway   : %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("[NET] Subnet    : %s\n", WiFi.subnetMask().toString().c_str());
  Serial.printf("[NET] DNS       : %s\n", WiFi.dnsIP().toString().c_str());
  Serial.printf("[NET] RSSI      : %d dBm\n", WiFi.RSSI());
  Serial.printf("[NET] Master IP : %s:%d\n", MASTER_IP, TCP_PORT);
  Serial.println(F("[NET] ---------"));
}

// Fetch one JSON line from host:port (server closes after sending)
bool fetchSatelliteLineFrom(IPAddress hostIP, uint16_t port, String& outJsonLine) {
  WiFiClient c;
  c.setTimeout(2000); // ms
  outJsonLine = "";

  for (int attempt = 1; attempt <= 3; attempt++) {
    if (!c.connect(hostIP, port)) {
      Serial.printf("❌ TCP connect failed to %s:%u (attempt %d/3)\n",
                    hostIP.toString().c_str(), port, attempt);
      delay(150);
      continue;
    }
    outJsonLine = c.readStringUntil('\n'); // server sends one line then closes
    c.stop();
    if (outJsonLine.length() == 0) {
      Serial.printf("❌ TCP got 0 bytes from %s:%u (attempt %d/3)\n",
                    hostIP.toString().c_str(), port, attempt);
      delay(150);
      continue;
    }
    Serial.printf("[TCP] %uB from %s:%u\n", (unsigned)outJsonLine.length(),
                  hostIP.toString().c_str(), port);
    return true;
  }
  return false;
}

static void processUDPTime() {
  int sz = udp.parsePacket();
  if (!sz) return;
  int len = udp.read(udpBuffer, sizeof(udpBuffer)-1);
  if (len <= 0) return;
  udpBuffer[len] = 0;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, udpBuffer)) return;
  String iso = doc["current_time_utc"].as<String>();
  if (iso.length()) {
    currentUTC = iso;
#if DEBUG
    static unsigned long lastLog = 0;
    unsigned long now = millis();
    if (now - lastLog > 5000) {
      Serial.print(F("⏰ UTC "));
      Serial.println(currentUTC);
      lastLog = now;
    }
#endif
  }
}

// Uses MASTER_IP first, then falls back to AP gateway if needed
static bool requestSatelliteFromServer() {
  String line;
  IPAddress ipMaster; ipMaster.fromString(MASTER_IP);
  bool ok = fetchSatelliteLineFrom(ipMaster, TCP_PORT, line);

  if (!ok) {
    IPAddress gw = WiFi.gatewayIP();
    if (gw && gw != ipMaster) {
      Serial.printf("[NET] Fallback: trying gateway %s:%u\n", gw.toString().c_str(), TCP_PORT);
      ok = fetchSatelliteLineFrom(gw, TCP_PORT, line);
    }
  }

  if (!ok) {
    static unsigned long lastDump = 0;
    if (millis() - lastDump > 10000) { printWiFiDebug(); lastDump = millis(); }
    return false;
  }

  StaticJsonDocument<2048> d;
  DeserializationError err = deserializeJson(d, line);
  if (err) {
#if DEBUG
    Serial.print(F("❌ JSON error: "));
    Serial.println(err.c_str());
    Serial.println(line);
#endif
    return false;
  }

  if (d.containsKey("error")) {
#if DEBUG
    Serial.printf("⚠️ Server replied error: %s\n", d["error"].as<const char*>());
#endif
    return false;
  }

  curSatName = d["name"].as<String>();
  curTLE1    = d["tle"]["line-1"].as<String>();
  curTLE2    = d["tle"]["line-2"].as<String>();
  currentUTC = d["current_time_utc"].as<String>();
  obsLat     = d["latitude"].as<double>();
  obsLon     = d["longitude"].as<double>();
  obsAlt     = 0.0;

  trackingInit(curSatName.c_str(), curTLE1.c_str(), curTLE2.c_str(), obsLat, obsLon, obsAlt);

#if DEBUG
  Serial.print(F("📡 Tracking: "));
  Serial.println(curSatName);
  Serial.print(F("⏰ UTC "));
  Serial.println(currentUTC);
#endif

  // Do NOT enable tracking/laser yet. Wait for first valid above-horizon point.
  gHasLock = false;
  motorsSetTrackingActive(false); // keeps laser/speaker off until lock
  mode = MODE_TRACK;
  return true;
}

// ===== Command callbacks =====
static bool onStartCmd() { motorsSetTrackingActive(false); mode = MODE_WAIT; lastSatRequest = 0; Serial.println(F("OK START (resuming)")); return true; }
static void onStopCmd()  { motorsSetTrackingActive(false); mode = MODE_STOP; Serial.println(F("OK STOP (paused)")); }
static void onStepAzCmd(int32_t steps) { motorsManualStepAZ(steps); Serial.printf("OK STEP AZ %ld\n",(long)steps); }
static void onStepElCmd(int32_t steps) { motorsManualStepEL(steps); Serial.printf("OK STEP EL %ld\n",(long)steps); }
static void onGotoAzCmd(float az) { motorsGotoAzDeg(az); Serial.printf("OK GOTO AZ %.2f\n", az); }
static void onGotoElCmd(float el) { motorsGotoElDeg(el); Serial.printf("OK GOTO EL %.2f\n", el); }
static bool onRequestSatelliteCmd() { mode = MODE_WAIT; lastSatRequest = 0; return true; }

static void onStatusPrint(Stream& s) {
  s.println(F("=== STATUS ==="));
  s.printf("Mode: %s\n", (mode==MODE_STOP)?"STOP":(mode==MODE_WAIT)?"WAIT":(mode==MODE_TRACK)?"TRACK":"HOME");
  s.printf("UTC: %s\n", currentUTC.c_str());
  double lat, lon, alt; trackingGetCurrentSite(lat, lon, alt);
  s.printf("Site: lat=%.6f lon=%.6f alt=%.1f\n", lat, lon, alt);
  s.printf("Laser: %s\n",(motorsGetLaserMode()==LASER_OFF)?"OFF":(motorsGetLaserMode()==LASER_ON)?"ON":"TRACK");
  audioPrintStatus(s);
  String n,l1,l2; trackingGetCurrentTLE(n,l1,l2);
  s.printf("TLE: %s\n", n.c_str());
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(300);

  // SD (VSPI) robust init
  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  delay(50);
  const uint32_t speeds[] = { 10000000U, 25000000U, 4000000U };
  bool sdOK = false;
  for (int attempt = 0; attempt < 3 && !sdOK; attempt++) {
    sdOK = SD.begin(SD_CS, spiSD, speeds[attempt]);
    if (!sdOK) { Serial.printf("SD init failed at %lu Hz, retrying...\n", speeds[attempt]); delay(150); }
  }
  if (!sdOK) Serial.println(F("❌ SD init failed (VSPI)"));
  else {
    Serial.println(F("✅ SD card initialized (VSPI)"));
    Serial.printf("[SD] Type=%d, Size=%llu MB\n", SD.cardType(), SD.cardSize()/1024ULL/1024ULL);
  }

  motorsInit();
  audioInit();

  Serial.println(F("Connecting to WiFi..."));
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 10000) { delay(250); Serial.print("."); }
  Serial.println();
  if (WiFi.status()==WL_CONNECTED) {
    Serial.println(F("✅ WiFi connected"));
    printWiFiDebug();
  } else {
    Serial.println(F("❌ WiFi connect failed"));
  }

  udpReg.begin(0); // ephemeral send port
  {
    uint8_t mac[6]; 
    WiFi.macAddress(mac);
    char namebuf[20];
    snprintf(namebuf, sizeof(namebuf), "MODULE-%02X%02X%02X", mac[3], mac[4], mac[5]);
    gModuleName = namebuf;

    IPAddress serverIP; serverIP.fromString(MASTER_IP);
    udpReg.beginPacket(serverIP, SERVER_REG_PORT);
    udpReg.print("HELLO "); udpReg.print(gModuleName);
    udpReg.endPacket();

    Serial.printf("👋 Sent HELLO as %s to %s:%d\n",
                  gModuleName.c_str(), serverIP.toString().c_str(), SERVER_REG_PORT);
  }

  udp.begin(UDP_PORT);        // time sync listener
  udpCmd.begin(UDP_CMD_PORT); // command listener
  Serial.printf("🎛️  Listening for commands on UDP %d\n", UDP_CMD_PORT);
#if DEBUG
  Serial.println(F("🕒 Listening UDP time"));
#endif

  // Commands
  Commands::begin(Serial);
  Commands::setRequestSatelliteCallback(&onRequestSatelliteCmd);
  Commands::setStatusPrinter(&onStatusPrint);
  Commands::setStartStopCallbacks(&onStartCmd, &onStopCmd);
  Commands::setStepCallbacks(&onStepAzCmd, &onStepElCmd);
  Commands::setGotoCallbacks(&onGotoAzCmd, &onGotoElCmd);
  Serial.println(F("Type HELP for commands."));

#if USE_TESTRUN
  TestRun::begin();
  // Example to auto test at boot:
  // TestRun::configure(220, 320, 45, 180, 10, true);
  // TestRun::enable(true);
  // TestRun::startNow();
#endif

  mode = MODE_WAIT;
  lastSatRequest = 0;
}

// ===== Loop =====
void loop() {
  // Serial commands (from USB)
  Commands::poll();

  // Time sync & audio
  processUDPTime();
  audioLoop();

  // UDP command listener (from server UI / /send endpoint)
  {
    int sz = udpCmd.parsePacket();
    if (sz > 0) {
      static char cmdbuf[256];
      int n = udpCmd.read(cmdbuf, sizeof(cmdbuf) - 1);
      if (n > 0) {
        cmdbuf[n] = '\0';
        String cmd = String(cmdbuf);
        cmd.trim();
        if (cmd.length()) {
          Commands::CommandsInject(cmd);
          Serial.printf("CMD[udp]: %s\n", cmd.c_str());
        }
      }
    }
  }

  // Periodic PING to server registrar
  static unsigned long lastPing = 0;
  if (millis() - lastPing > 4000UL) {
    lastPing = millis();
    IPAddress serverIP; serverIP.fromString(MASTER_IP);
    udpReg.beginPacket(serverIP, SERVER_REG_PORT);
    udpReg.print("PING "); udpReg.print(gModuleName);
    udpReg.endPacket();
  }

  // STOP mode: idle
  if (mode == MODE_STOP) { delay(10); return; }

  // WAIT mode: ask for a satellite every satRequestIntervalMs
  if (mode == MODE_WAIT) {
    motorsSetTrackingActive(false); // laser off between sats
    if (millis() - lastSatRequest > satRequestIntervalMs) {
      if (!requestSatelliteFromServer()) {
#if DEBUG
        Serial.println(F("…waiting for satellite"));
#endif
      }
      lastSatRequest = millis();
    }
    delay(5);
    return;
  }

  // TRACK mode: compute Az/El and move; enable laser only after first valid point
  if (mode == MODE_TRACK) {
    unsigned long ut = isoToUnix(currentUTC);
    if (!ut) { delay(10); return; }

    float az=0, el=0;
    bool ok = false;

#if USE_TESTRUN
    if (TestRun::isEnabled()) ok = TestRun::getAzEl((time_t)ut, az, el);
    else                      ok = trackingGetAzEl(ut, az, el);
#else
    ok = trackingGetAzEl(ut, az, el);
#endif

    if (ok && el >= EL_MIN && el <= EL_MAX) {
      // First valid point above horizon? Acquire lock and allow laser
      if (!gHasLock) {
        gHasLock = true;
        motorsSetTrackingActive(true); // this may enable laser if LASER_TRACK mode
        audioBeepPlay();               // “start tracking” beep (if enabled)
#if DEBUG
        Serial.println(F("🔒 Lock acquired (laser allowed under TRACK mode)"));
#endif
      }
#if DEBUG
      static unsigned long lastLog=0; static float la=-999, le=-999;
      unsigned long now=millis();
      bool big = (fabs(az-la)>0.5f) || (fabs(el-le)>0.5f);
      if (now-lastLog>1000 || big) { Serial.printf("[TRK] AZ=%.2f  EL=%.2f\n", az, el); lastLog=now; la=az; le=el; }
#endif
      motorsTrackTo(az, el);
      return;
    }

    // Not ok or below horizon/out of window
    if (gHasLock) {
#if DEBUG
      Serial.println(F("🌅 End of pass -> home + request next"));
#endif
      audioBeepPlay();            // “end tracking” beep (if enabled)
    } else {
#if DEBUG
      Serial.println(F("🌅 Below horizon -> home + request next"));
#endif
    }

    motorsSetTrackingActive(false); // ensure laser off when leaving tracking
    gHasLock = false;

    motorsReturnToNull();
    mode = MODE_WAIT;
    lastSatRequest = 0;
    return;
  }

  // HOME mode: return to null, then wait
  if (mode == MODE_HOME) {
    motorsSetTrackingActive(false);
    gHasLock = false;
    motorsReturnToNull();
    mode = MODE_WAIT;
    return;
  }
}