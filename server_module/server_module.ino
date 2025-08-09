#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <RTClib.h>

RTC_DS3231 rtc;

const char* ssid = "ESP32_Master_Network";
const char* password = "123456789";

#define SD_CS    5
#define SD_SCK   18
#define SD_MISO  19
#define SD_MOSI  23

SPIClass spiSD(VSPI);

// Assignment/data server
WiFiServer server(80);

// ---- Command server ----
const uint16_t CMD_PORT = 8080;
WiFiServer cmdServer(CMD_PORT);

// UDP time broadcast
WiFiUDP udp;
const int udpPort = 4210;
const IPAddress broadcastIP(192, 168, 4, 255);

const char* jsonFilename = "/satellite_data.json";
const char* locationFilename = "/location.json";
StaticJsonDocument<8192> doc;
int satelliteIndex = 0;

unsigned long lastBroadcast = 0;
const unsigned long broadcastInterval = 5000;

double currentLat = 0.0;
double currentLon = 0.0;

// ===== Command registry =====
struct CmdClient {
  WiFiClient cli;
  int id = -1; // -1 until HELLO <id>
  bool inUse = false;
};
static const int MAX_CMD_CLIENTS = 10;
static CmdClient registry[MAX_CMD_CLIENTS];

static void addCmdClient(WiFiClient&& c) {
  for (int i=0;i<MAX_CMD_CLIENTS;i++) {
    if (!registry[i].inUse || !registry[i].cli.connected()) {
      if (registry[i].inUse && registry[i].cli.connected()) registry[i].cli.stop();
      registry[i].cli = std::move(c);
      registry[i].id = -1;
      registry[i].inUse = true;
      return;
    }
  }
  c.stop();
}

static void pruneCmdClients() {
  for (int i=0;i<MAX_CMD_CLIENTS;i++) {
    if (registry[i].inUse && !registry[i].cli.connected()) {
      registry[i].cli.stop();
      registry[i].inUse = false;
      registry[i].id = -1;
    }
  }
}

static void pumpCmdClients() {
  // read HELLO <id>
  for (int i=0;i<MAX_CMD_CLIENTS;i++) {
    if (!registry[i].inUse) continue;
    auto &cli = registry[i].cli;
    while (cli.available()) {
      String line = cli.readStringUntil('\n');
      line.trim();
      if (line.startsWith("HELLO ")) {
        registry[i].id = line.substring(6).toInt();
        Serial.printf("[CMD] Client registered id=%d, slot=%d\n", registry[i].id, i);
      }
    }
  }
}

static void sendToAll(const String& cmd) {
  for (int i=0;i<MAX_CMD_CLIENTS;i++) {
    if (registry[i].inUse && registry[i].cli.connected()) {
      registry[i].cli.println("TO ALL " + cmd);
    }
  }
}
static bool sendToId(int id, const String& cmd) {
  bool any=false;
  for (int i=0;i<MAX_CMD_CLIENTS;i++) {
    if (registry[i].inUse && registry[i].cli.connected() && registry[i].id == id) {
      registry[i].cli.println("TO " + String(id) + " " + cmd);
      any=true;
    }
  }
  return any;
}

// ===== existing helpers =====
DateTime parseDateTime(const char* isoTime) {
  int y, M, d, h, m, s;
  sscanf(isoTime, "%d-%d-%dT%d:%d:%dZ", &y, &M, &d, &h, &m, &s);
  return DateTime(y, M, d, h, m, s);
}
const long MAX_TLE_AGE_SECONDS = 3600 * 6;  // 6 hours

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

void setup() {
  Serial.begin(115200);
  delay(2000);
  Wire.begin(21, 22);
  rtc.begin();

  WiFi.mode(WIFI_OFF); delay(100);
  WiFi.mode(WIFI_AP);  delay(100);

  if (WiFi.softAP(ssid, password, 1)) {
    Serial.println("✅ Access Point Started");
    Serial.print("IP Address: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("❌ Failed to start Access Point");
    return;
  }

  server.begin();
  cmdServer.begin(); // start command server
  udp.begin(udpPort);

  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, spiSD)) { Serial.println("❌ SD card init failed"); return; }
  Serial.println("✅ SD card initialized");

  // Load satellites
  File jsonFile = SD.open(jsonFilename);
  if (!jsonFile) { Serial.println("❌ open satellite_data.json"); return; }
  String jsonRaw; while (jsonFile.available()) jsonRaw += (char)jsonFile.read();
  jsonFile.close();
  if (deserializeJson(doc, jsonRaw)) {
    Serial.print("✅ Satellites loaded: "); Serial.println(doc.size());
  } else { Serial.println("❌ JSON parse error"); return; }

  // Load location
  File locFile = SD.open(locationFilename);
  if (!locFile) { Serial.println("❌ open location.json"); return; }
  String locRaw; while (locFile.available()) locRaw += (char)locFile.read();
  locFile.close();
  StaticJsonDocument<512> locDoc;
  if (!deserializeJson(locDoc, locRaw)) { Serial.println("❌ Location JSON error"); return; }
  currentLat = locDoc["latitude"] | 0.0;
  currentLon = locDoc["longitude"] | 0.0;
  Serial.println("✅ Loaded observer location:");
  Serial.println("Latitude: " + String(currentLat));
  Serial.println("Longitude: " + String(currentLon));
}

void handleAssignmentServer() {
  WiFiClient client = server.available();
  if (!client) return;

  if (client.available()) {
    String request = client.readStringUntil('\n');
    request.trim();
    if (request.indexOf("\"status\":\"complete\"") != -1) {
      if (satelliteIndex >= doc.size()) satelliteIndex = 0;
      JsonObject sat = doc[satelliteIndex];

      DateTime tleTime = parseDateTime(sat["datetime_utc"].as<const char*>());
      DateTime now = rtc.now();
      TimeSpan age = now - tleTime;

      if (age.totalseconds() > MAX_TLE_AGE_SECONDS) {
        Serial.print("⏭️ Skipped outdated satellite #"); Serial.println(satelliteIndex);
        satelliteIndex++;
        return;
      }

      String payload = createSatellitePayload(sat);
      client.println(payload);
      client.stop();
      Serial.print("✅ Re-assigned satellite #"); Serial.print(satelliteIndex);
      Serial.print(" after mission complete: ");
      Serial.println(sat["name"].as<String>());
      satelliteIndex++;
      return;
    }
  }

  // Initial assignment
  if (satelliteIndex >= doc.size()) satelliteIndex = 0;
  JsonObject sat = doc[satelliteIndex];
  String payload = createSatellitePayload(sat);
  client.println(payload);
  client.stop();
  Serial.print("📡 Assigned satellite #"); Serial.print(satelliteIndex);
  Serial.print(": "); Serial.println(sat["name"].as<String>());
  satelliteIndex++;
}

void handleCmdServer() {
  // Accept new connections
  WiFiClient incoming = cmdServer.available();
  if (incoming) addCmdClient(std::move(incoming));

  pruneCmdClients();
  pumpCmdClients();

  // Serial console
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c=='\r') continue;
    if (c=='\n') {
      String s = line; line = "";
      s.trim();
      if (s.length()==0) return;

      String up = s; up.toUpperCase();
      if (up == "LIST") {
        Serial.println(F("ID  SLOT  CONNECTED"));
        for (int i=0;i<MAX_CMD_CLIENTS;i++) {
          Serial.printf("%2d  %4d  %s\n", registry[i].id, i, (registry[i].inUse && registry[i].cli.connected())?"YES":"NO");
        }
        return;
      }
      if (up.startsWith("SEND ALL ")) {
        String cmd = s.substring(9);
        sendToAll(cmd);
        Serial.println(F("OK sent to ALL"));
        return;
      }
      if (up.startsWith("SEND ")) {
        // SEND <id> <cmd...>
        int sp = s.indexOf(' ', 5);
        if (sp > 5) {
          int id = s.substring(5, sp).toInt();
          String cmd = s.substring(sp+1);
          if (sendToId(id, cmd)) Serial.printf("OK sent to %d\n", id);
          else Serial.printf("ERR no client %d\n", id);
        } else {
          Serial.println(F("ERR usage: SEND <id> <command...>"));
        }
        return;
      }

      Serial.println(F("Commands: LIST | SEND ALL <command> | SEND <id> <command>"));
    } else {
      line += c;
      if (line.length() > 512) line.remove(0, 256);
    }
  }
}

void loop() {
  handleAssignmentServer();
  handleCmdServer();

  if (millis() - lastBroadcast > broadcastInterval) {
    String timePayload = buildTimePayload();
    udp.beginPacket(broadcastIP, udpPort);
    udp.print(timePayload);
    udp.endPacket();
    Serial.print("⏰ Broadcasted time: "); Serial.println(timePayload);
    lastBroadcast = millis();
  }
}
