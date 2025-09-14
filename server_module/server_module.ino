/**************************************************************
  Satellite Tracker – Client
**************************************************************/
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <RTClib.h>

RTC_DS3231 rtc;

// Network credentials
const char* ssid     = "ESP32_Master_Network";
const char* password = "123456789";

//SD pins
#define SD_CS    5
#define SD_SCK   18
#define SD_MISO  19
#define SD_MOSI  23
SPIClass spiSD(VSPI);

// Network
WiFiServer httpSrv(80);        // client modules & web UI
WiFiUDP    udpTime;            // time broadcaster
WiFiUDP    udpCmd;             // command relay to clients
WiFiUDP    udpReg;             // client HELLO/PING registrar

const int  UDP_TIME_PORT = 4210;             // time broadcast
const int  UDP_CMD_PORT  = 4212;             // commands to clients
const int  UDP_REG_PORT  = 4213;             // HELLO/PING from clients
const IPAddress broadcastIP(192,168,4,255);

//Data
static StaticJsonDocument<8192> doc; // one file at a time (keep each file modest, e.g., 50–150 sats)
int   satIndex = 0;
double currentLat = 0.0, currentLon = 0.0, currentAlt = 0.0;

unsigned long lastBroadcast = 0;
const unsigned long broadcastInterval = 100; // interval of sending time to clients in ms
const long MAX_TLE_AGE_SECONDS = 3600L * 6;  // 6 hours max

//Multi-file list (auto-detected)
struct SatFile {
  String name;   // "/sat_data_1.json" etx.
  int    index;  // extracted number (1,2,3…)
  uint32_t size;
  int count;     // count of satellites in file (filled on load)
};
SatFile satFiles[24];
int satFilesCount = 0;
int satFileCursor = 0; // which file we’re serving (0-based into satFiles[])

// Track connected clients for UI targeting & assignment display
struct ClientInfo {
  IPAddress ip;
  unsigned long lastSeenMs;
  String name;

  // assignment display
  String lastSat;
  int    lastSatIndex = -1;
  int    lastFileCursor = -1;
  unsigned long lastAssignMs = 0;
};
ClientInfo clients[16];
int clientsCount = 0;

DateTime parseDateTime(const char* isoTime) {
  int y, M, d, h, m, s;
  if (!isoTime || strlen(isoTime) < 19) return DateTime((uint32_t)0);
  sscanf(isoTime, "%d-%d-%dT%d:%d:%dZ", &y, &M, &d, &h, &m, &s);
  return DateTime(y, M, d, h, m, s);
}

void upsertClient(IPAddress ip, const String& name = String()) {
  for (int i=0;i<clientsCount;i++) {
    if (clients[i].ip == ip) {
      clients[i].lastSeenMs = millis();
      if (name.length()) clients[i].name = name;
      return;
    }
  }
  if (clientsCount < (int)(sizeof(clients)/sizeof(clients[0]))) {
    clients[clientsCount].ip = ip;
    clients[clientsCount].lastSeenMs = millis();
    clients[clientsCount].name = name;
    clients[clientsCount].lastSat = "";
    clients[clientsCount].lastSatIndex = -1;
    clients[clientsCount].lastFileCursor = -1;
    clients[clientsCount].lastAssignMs = 0;
    clientsCount++;
  }
}

void recordAssignmentFor(IPAddress ip, const String& satName, int satIdx, int fileCursor) {
  for (int i=0;i<clientsCount;i++){
    if (clients[i].ip == ip) {
      clients[i].lastSat = satName;
      clients[i].lastSatIndex = satIdx;
      clients[i].lastFileCursor = fileCursor;
      clients[i].lastAssignMs = millis();
      return;
    }
  }
}

String buildTimePayload() {
  DateTime now = rtc.now();
  char buffer[25];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());
  return String("{\"current_time_utc\":\"") + buffer + "\"}";
}

String createSatellitePayload(JsonObject& sat) {
  DateTime now = rtc.now();
  char timestamp[25];
  snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());

  String s = "{";
  s += "\"current_time_utc\":\"" + String(timestamp) + "\",";
  s += "\"id\":" + String(sat["id"].as<int>()) + ",";
  s += "\"name\":\"" + String(sat["name"].as<String>()) + "\",";
  s += "\"latitude\":" + String(currentLat) + ",";
  s += "\"longitude\":" + String(currentLon) + ",";
  s += "\"distance_km\":" + String(sat["distance_km"].as<double>()) + ",";
  s += "\"elevation_deg\":" + String(sat["elevation_deg"].as<double>()) + ",";
  s += "\"datetime_utc\":\"" + String(sat["datetime_utc"].as<String>()) + "\",";
  s += "\"tle\":{";
  s += "\"line-1\":\"" + String(sat["tle"]["line-1"].as<String>()) + "\",";
  s += "\"line-2\":\"" + String(sat["tle"]["line-2"].as<String>()) + "\"";
  s += "}}";
  return s;
}

//JSON handling
void sendJson(WiFiClient& c, const String& bodyJson) {
  c.print("HTTP/1.1 200 OK\r\n"
          "Content-Type: application/json\r\n"
          "Cache-Control: no-cache\r\n"
          "Connection: close\r\n\r\n");
  c.print(bodyJson);
}

void sendText(WiFiClient& c, const String& body) {
  c.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
  c.print(body);
}

String buildClientsJson() {
  String s = "[";
  bool first = true;
  unsigned long now = millis();
  for (int i=0;i<clientsCount;i++){
    if (!first) s += ",";
    first=false;
    s += "{\"ip\":\"" + clients[i].ip.toString() + "\",";
    s += "\"name\":\"" + clients[i].name + "\",";
    s += "\"secs\":" + String((now - clients[i].lastSeenMs)/1000) + ",";
    s += "\"lastSat\":\"" + clients[i].lastSat + "\",";
    s += "\"lastSatIndex\":" + String(clients[i].lastSatIndex) + ",";
    s += "\"lastFileCursor\":" + String(clients[i].lastFileCursor) + ",";
    long as = clients[i].lastAssignMs ? (long)((now - clients[i].lastAssignMs)/1000) : -1;
    s += "\"lastAssignSecs\":" + String(as) + "}";
  }
  s += "]";
  return s;
}

int extractIndex(const String& name) {
  int us = name.lastIndexOf('_');
  int dot = name.lastIndexOf('.');
  if (us < 0 || dot < 0 || dot <= us) return -1;
  String num = name.substring(us+1, dot);
  for (size_t i=0;i<num.length();i++) if (!isDigit(num[i])) return -1;
  return num.toInt();
}

void scanSatelliteFiles() {
  satFilesCount = 0;

  File root = SD.open("/");
  if (!root) { Serial.println("❌ SD root open failed"); return; }

  File f = root.openNextFile();
  while (f) {
    String nm = f.name();
    uint32_t sz = f.size();
    f.close();
    if (!nm.startsWith("/")) nm = "/" + nm;
    if (nm.startsWith("/sat_data_") && nm.endsWith(".json")) {
      int idx = extractIndex(nm);
      if (idx > 0 && satFilesCount < (int)(sizeof(satFiles)/sizeof(satFiles[0]))) {
        satFiles[satFilesCount].name = nm;
        satFiles[satFilesCount].index = idx;
        satFiles[satFilesCount].size = sz;
        satFiles[satFilesCount].count = -1;
        satFilesCount++;
      }
    }
    f = root.openNextFile();
  }
  root.close();

  // sort by idx asc
  for (int i=0;i<satFilesCount-1;i++) {
    for (int j=i+1;j<satFilesCount;j++) {
      if (satFiles[j].index < satFiles[i].index) {
        SatFile tmp = satFiles[i]; satFiles[i]=satFiles[j]; satFiles[j]=tmp;
      }
    }
  }

  Serial.printf("📁 Found %d satellite JSON files:\n", satFilesCount);
  for (int i=0;i<satFilesCount;i++) {
    Serial.printf("  %2d: %s (idx %d, %lu B)\n", i+1, satFiles[i].name.c_str(), satFiles[i].index, (unsigned long)satFiles[i].size);
  }
}

bool loadSatelliteFileByCursor() {
  if (satFilesCount == 0) { doc.clear(); satIndex=0; return true; }
  if (satFileCursor < 0 || satFileCursor >= satFilesCount) satFileCursor = 0;

  String filename = satFiles[satFileCursor].name;
  File file = SD.open(filename);
  if (!file) {
    Serial.printf("❌ Could not open %s\n", filename.c_str());
    return false;
  }

  doc.clear();
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    Serial.printf("❌ JSON parse error in %s: %s\n", filename.c_str(), err.c_str());
    return false;
  }

  satFiles[satFileCursor].count = (int)doc.size();
  Serial.printf("✅ Loaded %s (%d satellites)\n", filename.c_str(), satFiles[satFileCursor].count);
  satIndex = 0;
  return true;
}

bool advanceToNextFile() {
  if (satFilesCount == 0) return false;
  satFileCursor++;
  if (satFileCursor >= satFilesCount) satFileCursor = 0;
  return loadSatelliteFileByCursor();
}

//  Web Interface
String htmlIndex() {
  String h;
  h += F("HTTP/1.1 200 OK\r\n");
  h += F("Content-Type: text/html; charset=utf-8\r\n");
  h += F("Connection: close\r\n\r\n");

  h += F("<!doctype html>");
  h += F("<html><head>");
  h += F("<meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<style>");
  h += F("body{font-family:system-ui,Arial;margin:16px}");
  h += F("button,input,select{font-size:16px;padding:10px 14px;margin:6px}");
  h += F(".card{border:1px solid #ccc;border-radius:12px;padding:12px;margin:12px 0}");
  h += F(".row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}");
  h += F("code{background:#f4f4f4;padding:2px 6px;border-radius:6px}");
  h += F(".statusline{margin-left:8px;color:#333;font-size:14px}");
  h += F("table{border-collapse:collapse;width:100%}");
  h += F("th,td{border:1px solid #ddd;padding:8px;text-align:left}");
  h += F("th{background:#fafafa}");
  h += F(".pill{border:1px solid #ddd;padding:6px 10px;border-radius:999px;margin:4px;display:inline-block}");
  h += F(".hold{user-select:none;-webkit-user-select:none}");
  h += F("</style>");
  h += F("</head><body>");

  h += F("<h2>ESP32 Satellite Server</h2>");

  // Time card
  h += F("<div class=card><h3>Time (RTC)</h3>"
         "<div class=row>"
         "<input id=iso value='' placeholder='YYYY-MM-DDTHH:MM:SSZ' style='min-width:260px'>"
         "<button id=btnSetTime>Set RTC</button>"
         "<span id=timeMsg class=statusline></span>"
         "</div>"
         "<small>Note: UTC only (append 'Z').</small>"
         "</div>");

  // Files + cursor info
  h += F("<div class=card><h3>Satellite files</h3><div id=files>Loading…</div>"
         "<div class=row>"
         "<button id=btnRescan>Rescan SD</button>"
         "<button id=btnNextFile>Next file</button>"
         "<button id=btnReload>Reload current</button>"
         "<input id=gotoIndex type=number min=1 style='width:90px' placeholder='index'>"
         "<button id=btnGoto>Goto</button>"
         "<span id=filesMsg class=statusline></span></div></div>");

  // Clients
  h += F("<div class=card><h3>Clients</h3><div id=clients>Loading…</div>"
         "<button id=btnRefresh>Refresh</button></div>");

  // Commands
  h += F("<div class=card><h3>Send Command</h3>"
         "<div class=row><label>Target:</label>"
         "<select id=target><option value='ALL'>All</option></select></div>"
         "<div class=row><input id=cmd size=36 placeholder='e.g. START / STOP / HOME / SAT NEW'>"
         "<button id=btnSend>Send</button><span id=sendMsg class=statusline></span></div>");

  // Jog controls with press-and-hold
  h += F("<div class=card><h3>Manual Jog (press and hold)</h3>"
         "<div class=row>"
         "<span class=pill>Step size:</span>"
         "<select id=stepSize>"
         "<option value='1'>1</option>"
         "<option value='5' selected>5</option>"
         "<option value='10'>10</option>"
         "<option value='25'>25</option>"
         "<option value='100'>100</option>"
         "</select>"
         "</div>"
         "<div class=row>"
         "<span class=pill>AZ:</span>"
         "<button class='hold' id=azLeft>&larr; AZ</button>"
         "<button class='hold' id=azRight>AZ &rarr;</button>"
         "</div>"
         "<div class=row>"
         "<span class=pill>EL:</span>"
         "<button class='hold' id=elDown>&darr; EL</button>"
         "<button class='hold' id=elUp>EL &uarr;</button>"
         "<span id=jogMsg class=statusline></span>"
         "</div>"
         "</div>");
  
  h += F("<div class=card><h3>Manual STEP limits</h3>"
       "<div class=row>"
       "<label class=pill>AZ Limit:</label>"
       "<select id=azLimit><option>ON</option><option selected>OFF</option></select>"
       "<label class=pill>AZ min</label><input id=azMin type=number step=1 value='-180' style='width:90px'>"
       "<label class=pill>AZ max</label><input id=azMax type=number step=1 value='540' style='width:90px'>"
       "</div>"
       "<div class=row>"
       "<label class=pill>EL Limit:</label>"
       "<select id=elLimit><option selected>ON</option><option>OFF</option></select>"
       "<label class=pill>EL min</label><input id=elMin type=number step=1 value='0' style='width:90px'>"
       "<label class=pill>EL max</label><input id=elMax type=number step=1 value='180' style='width:90px'>"
       "</div>"
       "<div class=row>"
       "<button id=btnStepLimitApply>Apply to Target</button>"
       "<button id=btnStepLimitStatus>Query Status</button>"
       "<span id=limitMsg class=statusline></span>"
       "</div></div>");

  // Shortcuts
  h += F("<div class=card><h3>Shortcuts</h3>"
         "<div class=row>"
         "<span class=pill>Session:</span>"
         "<button class=quick data-cmd='START'>START</button>"
         "<button class=quick data-cmd='STOP'>STOP</button>"
         "<button class=quick data-cmd='HOME'>HOME</button>"
         "<button class=quick data-cmd='HOME SET'>HOME SET</button>"
         "<button class=quick data-cmd='SAT NEW'>SAT NEW</button>"
         "</div>"
         "<div class=row>"
         "<span class=pill>Laser:</span>"
         "<button class=quick data-cmd='LASER OFF'>OFF</button>"
         "<button class=quick data-cmd='LASER ON'>ON</button>"
         "<button class=quick data-cmd='LASER TRACK'>TRACK</button>"
         "</div>"
         "<div class=row>"
         "<span class=pill>Step AZ:</span>"
         "<button class=quick data-cmd='STEP AZ -100'>AZ -100</button>"
         "<button class=quick data-cmd='STEP AZ -10'>AZ -10</button>"
         "<button class=quick data-cmd='STEP AZ 10'>AZ +10</button>"
         "<button class=quick data-cmd='STEP AZ 100'>AZ +100</button>"
         "</div>"
         "<div class=row>"
         "<span class=pill>Step EL:</span>"
         "<button class=quick data-cmd='STEP EL -100'>EL -100</button>"
         "<button class=quick data-cmd='STEP EL -10'>EL -10</button>"
         "<button class=quick data-cmd='STEP EL 10'>EL +10</button>"
         "<button class=quick data-cmd='STEP EL 100'>EL +100</button>"
         "</div>"
         "<div class=row>"
         "<span class=pill>Audio:</span>"
         "<button class=quick data-cmd='AUDIO IDLEMUTE ON'>IdleMute ON</button>"
         "<button class=quick data-cmd='AUDIO IDLEMUTE OFF'>IdleMute OFF</button>"
         "<button class=quick data-cmd='BEEP TEST'>BEEP TEST</button>"
         "</div>"
         "</div>");

  // Status (bulk)
  h += F("<div class=card><h3>Assignments</h3>"
         "<div id=assignments>Loading…</div></div>");

  // Script (no '+' concat inside F())
  h += F("<script>");
  h += F("function _(q){return document.querySelector(q)};");
  h += F("function send(target,cmd,cb){fetch('/send?targets='+encodeURIComponent(target)+'&cmd='+encodeURIComponent(cmd)).then(r=>r.text()).then(t=>cb&&cb(t)).catch(e=>cb&&cb('ERR '+e));}");
  h += F("function refresh(){fetch('/clients').then(r=>r.json()).then(list=>{let s=_('#target'); s.innerHTML='<option value=ALL>All</option>'; let div=_('#clients'); let html=''; list.forEach(c=>{s.innerHTML+=`<option value='${c.ip}'>${c.name||c.ip}</option>`; html+=`<div>${c.name||c.ip} <small>(${c.ip})</small> <small>(seen ${c.secs}s)</small></div>`}); if(!list.length) html='<i>No clients yet. They appear after HELLO/PING.</i>'; div.innerHTML=html;});}");
  h += F("function files(){fetch('/files').then(r=>r.json()).then(info=>{let div=_('#files'); let html=`<div>Files: ${info.length}</div>`; html+='<ul>'; info.forEach(f=>{let cs=(f.count>=0?f.count:('- '+(f.size||0)+' B')); html+=`<li>#${f.idx}: ${f.name} — ${cs}</li>`}); html+='</ul>'; div.innerHTML=html;});}");
  h += F("function renderAssignments(){fetch('/clients').then(r=>r.json()).then(list=>{if(list.length===0){_('#assignments').innerHTML='<i>No clients yet.</i>';return;} let rows=''; list.forEach(c=>{let sat=c.lastSat||'-'; let idx=(c.lastSatIndex>=0?c.lastSatIndex:'-'); let file=(c.lastFileCursor>=0?(c.lastFileCursor+1):'-'); let when=(c.lastAssignSecs>=0?c.lastAssignSecs+'s':'-'); rows+=`<tr><td>${c.name||c.ip}</td><td>${c.ip}</td><td>${sat}</td><td>${idx}</td><td>${file}</td><td>${when}</td></tr>`;}); _('#assignments').innerHTML=`<table><thead><tr><th>Module</th><th>IP</th><th>Satellite</th><th>SatIdx</th><th>File#</th><th>Assigned</th></tr></thead><tbody>${rows}</tbody></table>`;});}");
  h += F("document.addEventListener('DOMContentLoaded',()=>{_('#btnSend').addEventListener('click',()=>{let t=_('#target').value; let c=_('#cmd').value; if(!c){_('#sendMsg').textContent='Enter a command';return;} send(t,c,(m)=>_('#sendMsg').textContent=m);}); _('#btnRefresh').addEventListener('click',refresh); _('#btnNextFile').addEventListener('click',()=>{fetch('/nextfile').then(r=>r.text()).then(t=>{_('#filesMsg').textContent=t; files();});}); _('#btnReload').addEventListener('click',()=>{fetch('/reload').then(r=>r.text()).then(t=>{_('#filesMsg').textContent=t; files();});}); _('#btnRescan').addEventListener('click',()=>{fetch('/rescan').then(r=>r.text()).then(t=>{_('#filesMsg').textContent=t; files();});}); _('#btnGoto').addEventListener('click',()=>{let i=parseInt(_('#gotoIndex').value||'0',10); if(!i){_('#filesMsg').textContent='Enter index';return;} fetch('/goto?index='+i).then(r=>r.text()).then(t=>{_('#filesMsg').textContent=t; files();});}); document.querySelectorAll('.quick').forEach(b=>{b.addEventListener('click',()=>{let t=_('#target').value; let c=b.getAttribute('data-cmd'); send(t,c,(m)=>_('#sendMsg').textContent=m);});}); let jogTimer=null; function jogStart(cmd){let t=_('#target').value; let rate=80; if(jogTimer) clearInterval(jogTimer); _('#jogMsg').textContent='jogging…'; jogTimer=setInterval(()=>{send(t,cmd,(m)=>{_('#sendMsg').textContent=m;});},rate);} function jogStop(){ if(jogTimer){clearInterval(jogTimer); jogTimer=null; _('#jogMsg').textContent='';}} function bindHold(btn, cmdBuilder){['mousedown','touchstart'].forEach(ev=>btn.addEventListener(ev,(e)=>{e.preventDefault(); jogStart(cmdBuilder());})); ['mouseup','mouseleave','touchend','touchcancel'].forEach(ev=>btn.addEventListener(ev,(e)=>{e.preventDefault(); jogStop();})); } bindHold(_('#azLeft'), ()=>{let n=_('#stepSize').value; return 'STEP AZ -'+n;}); bindHold(_('#azRight'),()=>{let n=_('#stepSize').value; return 'STEP AZ '+n;}); bindHold(_('#elDown'),()=>{let n=_('#stepSize').value; return 'STEP EL -'+n;}); bindHold(_('#elUp'),  ()=>{let n=_('#stepSize').value; return 'STEP EL '+n;}); _('#btnSetTime').addEventListener('click',()=>{let iso=_('#iso').value.trim(); if(!iso){_('#timeMsg').textContent='Enter ISO UTC time';return;} fetch('/settime?iso='+encodeURIComponent(iso)).then(r=>r.text()).then(t=>{_('#timeMsg').textContent=t;}).catch(e=>_('#timeMsg').textContent='ERR '+e);}); refresh(); files(); renderAssignments(); setInterval(()=>{refresh(); files(); renderAssignments();}, 3000);});");
  h += F("_('#btnStepLimitApply').addEventListener('click',()=>{"
  "let t=_('#target').value;"
  "let azL=_('#azLimit').value==='ON';"
  "let elL=_('#elLimit').value==='ON';"
  "let azMin=_('#azMin').value, azMax=_('#azMax').value;"
  "let elMin=_('#elMin').value, elMax=_('#elMax').value;"

  // order matters; set ranges first, then toggles
  "send(t,`STEP LIMIT AZ ${azMin} ${azMax}`,m=>{_('#limitMsg').textContent=m;});"
  "send(t,`STEP LIMIT EL ${elMin} ${elMax}`,m=>{_('#limitMsg').textContent=m;});"
  "send(t,`STEP LIMITS AZ ${azL?'ON':'OFF'}`,(m)=>{_('#limitMsg').textContent=m;});"
  "send(t,`STEP LIMITS EL ${elL?'ON':'OFF'}`,(m)=>{_('#limitMsg').textContent=m;});"
"});"

"_('#btnStepLimitStatus').addEventListener('click',()=>{"
  "let t=_('#target').value; send(t,'STEP LIMIT STATUS',m=>{_('#limitMsg').textContent=m;});"
"});");
  h += F("</script>");

  h += F("</body></html>");
  return h;
}

// HTTP router
void handleHttp(WiFiClient& c, const String& reqLine) {
  if (reqLine.startsWith("GET / ")) {
    c.print(htmlIndex());
    return;
  }

  // list files (JSON array)
  if (reqLine.startsWith("GET /files")) {
    String out = "[";
    for (int i=0;i<satFilesCount;i++){
      if (i) out += ",";
      out += "{";
      out += "\"idx\":" + String(i+1) + ",";
      out += "\"name\":\"" + satFiles[i].name + "\",";
      out += "\"size\":" + String(satFiles[i].size) + ",";
      out += "\"count\":" + String(satFiles[i].count);
      out += "}";
    }
    out += "]";
    sendJson(c, out);
    return;
  }

  if (reqLine.startsWith("GET /clients")) {
    sendJson(c, buildClientsJson());
    return;
  }

  if (reqLine.startsWith("GET /nextfile")) {
    bool ok = advanceToNextFile();
    sendText(c, ok ? "OK switched to next file" : "ERR no files");
    return;
  }

  if (reqLine.startsWith("GET /reload")) {
    bool ok = loadSatelliteFileByCursor();
    sendText(c, ok ? "OK reloaded" : "ERR reload failed");
    return;
  }

  if (reqLine.startsWith("GET /rescan")) {
    scanSatelliteFiles();
    bool ok = (satFilesCount > 0) ? loadSatelliteFileByCursor() : true;
    sendText(c, ok ? "OK rescan complete" : "ERR rescan failed");
    return;
  }

  if (reqLine.startsWith("GET /goto?")) {
    String q = reqLine.substring(10);
    int sp = q.indexOf(' ');
    if (sp>0) q = q.substring(0, sp);
    int a = q.indexOf("index=");
    int target = -1;
    if (a >= 0) target = q.substring(a+6).toInt();
    if (target >= 1 && target <= satFilesCount) {
      satFileCursor = target - 1;
      bool ok = loadSatelliteFileByCursor();
      sendText(c, ok ? "OK goto loaded" : "ERR goto failed");
    } else {
      sendText(c, "ERR invalid index");
    }
    return;
  }

  if (reqLine.startsWith("GET /satindex?")) {
    String q = reqLine.substring(14);
    int sp = q.indexOf(' ');
    if (sp>0) q = q.substring(0, sp);
    int a = q.indexOf("set=");
    int v = -1;
    if (a >= 0) v = q.substring(a+4).toInt();
    if (v >= 0 && v < (int)doc.size()) {
      satIndex = v;
      sendText(c, "OK satIndex set");
    } else {
      sendText(c, "ERR invalid satIndex");
    }
    return;
  }

  // Set RTC time manually
  if (reqLine.startsWith("GET /settime?")) {
    int q = reqLine.indexOf('?');
    String qy = (q >= 0) ? reqLine.substring(q + 1) : "";
    int sp = qy.indexOf(' ');
    if (sp > 0) qy = qy.substring(0, sp);

    int a = qy.indexOf("iso=");
    if (a < 0) { sendText(c, "ERR missing iso"); return; }
    String iso = qy.substring(a + 4);
    iso.replace("%3A", ":");
    iso.replace("%2D", "-");
    iso.replace("%2F", "/");
    iso.replace("%20", " ");
    iso.replace("+", " ");
    iso.trim();

    if (!iso.endsWith("Z") || iso.length() < 20) {
      sendText(c, "ERR use UTC like 2025-08-11T19:00:00Z");
      return;
    }

    DateTime dt = parseDateTime(iso.c_str());
    if (dt.unixtime() == 0) {
      sendText(c, "ERR bad ISO time");
      return;
    }
    rtc.adjust(dt);
    sendText(c, String("OK RTC set to ") + iso);
    return;
  }

  if (reqLine.startsWith("GET /send?")) {
    String q = reqLine.substring(10);
    int sp = q.indexOf(' ');
    if (sp>0) q = q.substring(0, sp);
    String targets, cmd;

    int a = q.indexOf("targets=");
    int b = q.indexOf("&cmd=");
    if (a>=0 && b> a) {
      targets = q.substring(a+8, b);
      cmd     = q.substring(b+5);
      cmd.replace("%20"," "); cmd.replace("+"," ");
      targets.replace("%20"," ");
    }

    if (!cmd.length()) { sendText(c, "ERR missing cmd"); return; }

    int sent = 0;
    if (targets == "ALL") {
      for (int i=0;i<clientsCount;i++){
        udpCmd.beginPacket(clients[i].ip, UDP_CMD_PORT);
        udpCmd.print(cmd);
        udpCmd.endPacket();
        sent++;
      }
    } else {
      IPAddress ip; ip.fromString(targets);
      if (!ip) { sendText(c, "ERR invalid IP"); return; }
      udpCmd.beginPacket(ip, UDP_CMD_PORT);
      udpCmd.print(cmd);
      udpCmd.endPacket();
      sent=1;
    }
    sendText(c, String("OK sent to ")+sent);
    return;
  }

  if (reqLine.startsWith("GET /ping")) {
    sendJson(c, "{\"ok\":true}");
    return;
  }

  sendText(c, "OK");
}

//Setup / Loop 
void setup() {
  Serial.begin(115200);
  delay(1200);
  Wire.begin(21, 22);
  rtc.begin();

  WiFi.mode(WIFI_OFF); delay(100);
  WiFi.mode(WIFI_AP); delay(100);

  if (WiFi.softAP(ssid, password, 1)) {
    Serial.println("✅ Access Point Started");
    Serial.print("IP Address: "); Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("❌ Failed to start Access Point");
    return;
  }

  httpSrv.begin();
  udpTime.begin(UDP_TIME_PORT);
  udpCmd.begin(0);                // sender only
  udpReg.begin(UDP_REG_PORT);     // listen HELLO/PING
  Serial.printf("🌐 Web UI: http://%s/\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("⏱️  Time UDP:%d  🎛️ Cmd UDP:%d  👋 Reg UDP:%d\n", UDP_TIME_PORT, UDP_CMD_PORT, UDP_REG_PORT);

  // SD init
  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  bool sdOK = false;
  const uint32_t speeds[] = { 25000000U, 20000000U, 10000000U, 4000000U };
  for (int i=0;i<4 && !sdOK;i++) {
    sdOK = SD.begin(SD_CS, spiSD, speeds[i]);
    if (!sdOK) { Serial.printf("SD init failed @%lu Hz, retry...\n", (unsigned long)speeds[i]); delay(150); }
  }
  if (!sdOK) { Serial.println("❌ SD card initialization failed"); }
  else {
    Serial.println("✅ SD card initialized");
  }

  // Load location
  {
    File locFile = SD.open("/location.json");
    if (!locFile) {
      Serial.println("❌ Failed to open location.json");
    } else {
      String locRaw; while (locFile.available()) locRaw += (char)locFile.read(); locFile.close();
      StaticJsonDocument<512> locDoc;
      DeserializationError e = deserializeJson(locDoc, locRaw);
      if (!e) {
        currentLat = locDoc["latitude"]  | 0.0;
        currentLon = locDoc["longitude"] | 0.0;
        currentAlt = locDoc["altitude_m"]| 0.0;
        Serial.printf("✅ Location: lat=%.6f lon=%.6f alt=%.1f\n", currentLat, currentLon, currentAlt);
      } else {
        Serial.println("❌ Location JSON error");
      }
    }
  }

  // Find & load satellite files
  scanSatelliteFiles();
  if (satFilesCount == 0) {
    Serial.println("⚠️ No /sat_data_*.json files found. Clients will get a built-in TEST-SAT.");
  } else {
    if (!loadSatelliteFileByCursor()) {
      Serial.println("❌ Initial satellite file load failed");
    }
  }
}

void loop() {
  // HELLO/PING from Clietn
  {
    int sz = udpReg.parsePacket();
    if (sz > 0) {
      char buf[128];
      int n = udpReg.read(buf, sizeof(buf)-1);
      if (n > 0) {
        buf[n] = 0;
        String msg(buf); msg.trim(); msg.replace("\r",""); msg.replace("\n","");
        String cmd, name;
        int sp = msg.indexOf(' ');
        if (sp > 0) { cmd = msg.substring(0,sp); name = msg.substring(sp+1); } else { cmd = msg; }
        IPAddress rip = udpReg.remoteIP();
        if (cmd.equalsIgnoreCase("HELLO") || cmd.equalsIgnoreCase("PING")) {
          upsertClient(rip, name);
          Serial.printf("👋 %s from %s (%s)\n", cmd.c_str(), rip.toString().c_str(), name.c_str());
        }
      }
    }
  }

  // HTTP / Client TCP accept
  WiFiClient c = httpSrv.available();
  if (c) {
    // Wait briefly to detect HTTP vs silent module
    unsigned long start = millis();
    while (!c.available() && millis()-start < 150) delay(1);

    bool handledHTTP = false;
    if (c.available()) {
      String line = c.readStringUntil('\n'); line.trim();
      if (line.startsWith("GET ") || line.startsWith("POST ")) {
        handleHttp(c, line);
        c.stop();
        handledHTTP = true;
      }
    }

    if (!handledHTTP) {
      // Client module asking for satellite
      upsertClient(c.remoteIP());

      if (satFilesCount == 0 || doc.size() == 0) {
        String tp = buildTimePayload(); // {"current_time_utc":"..."}
        String payload = String("{")
          + "\"current_time_utc\":" + tp.substring(1) + ","
          + "\"id\":25544,"
          + "\"name\":\"TEST-SAT\","
          + "\"latitude\":" + String(currentLat) + ","
          + "\"longitude\":" + String(currentLon) + ","
          + "\"distance_km\":500.0,"
          + "\"elevation_deg\":45.0,"
          + "\"datetime_utc\":\"2025-01-01T00:00:00Z\","
          + "\"tle\":{\"line-1\":\"1 TEST ...\",\"line-2\":\"2 TEST ...\"}"
          + "}";
        c.println(payload);  // newline important
        c.stop();
        Serial.println("📡 Assigned built-in TEST-SAT");
      } else {
        if (satIndex >= (int)doc.size()) {
          if (!advanceToNextFile()) {
            loadSatelliteFileByCursor();
          }
        }
        if (doc.size() == 0) {
          c.println("{\"error\":\"no satellites\"}");
          c.stop();
        } else {
          JsonObject sat = doc[satIndex];

          DateTime tleTime = parseDateTime(sat["datetime_utc"].as<const char*>());
          DateTime now = rtc.now();
          if ((now - tleTime).totalseconds() > MAX_TLE_AGE_SECONDS) {
            Serial.printf("⏭️ Skip outdated sat #%d (file %d/%d)\n", satIndex, satFileCursor+1, satFilesCount);
            satIndex++;
            c.stop();
          } else {
            String payload = createSatellitePayload(sat);
            c.println(payload); 
            IPAddress rip = c.remoteIP();
            recordAssignmentFor(rip, sat["name"].as<const char*>(), satIndex, satFileCursor);
            c.stop();
            Serial.printf("📡 Assigned sat #%d (file %d/%d): %s\n",
                          satIndex, satFileCursor+1, satFilesCount,
                          sat["name"].as<const char*>());
            satIndex++;
          }
        }
      }
    }
  }

//Serial time setter (command: HOSTTIME YYYY-MM-DDTHH:MM:SSZ)
if (Serial.available()) {
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.startsWith("HOSTTIME ")) {
    String iso = line.substring(9);
    iso.trim();
    if (!iso.endsWith("Z")) {
      Serial.println("❌ Expect UTC with trailing Z, e.g. 2025-08-19T15:10:05Z");
    } else {
      DateTime dt = parseDateTime(iso.c_str());
      if (dt.unixtime() == 0) {
        Serial.printf("❌ Parse failed for '%s'\n", iso.c_str());
      } else {
        rtc.adjust(dt);
        Serial.printf("✅ RTC set to %s (epoch %lu)\n", iso.c_str(), (unsigned long)dt.unixtime());
      }
    }
  }
}


  //Broadcast time
const unsigned long broadcastInterval = 200; // 5 Hz (every 200 ms)

if (millis() - lastBroadcast > broadcastInterval) {
    String timePayload = buildTimePayload();
    udpTime.beginPacket(broadcastIP, UDP_TIME_PORT);
    udpTime.print(timePayload);
    udpTime.endPacket();

    static unsigned long lastPrint = 0;
    if (millis() - lastPrint >= 1000) { // logs once per second
        Serial.print("⏰ Broadcasted time: "); 
        Serial.println(timePayload);
        lastPrint = millis();
    }

    lastBroadcast = millis();
  }
}