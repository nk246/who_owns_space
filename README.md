# ESP32 Satellite Tracker — Server/Client

## Overview
- **Server** (ESP32 AP): assigns satellites from SD (`/satellite_data.json`), broadcasts UTC time via UDP, and now sends **control commands** to clients over a dedicated command channel.
- **Client** (ESP32 STA): tracks satellites with SparkFun **Sgp4**, drives **NEMA17 (AZ)** and **28BYJ-48 (EL)**, manages **laser modes**, records MAX9814 mic → DAC audio with **gate/limiter/notch/noise**, plays **beeps** on start/end, saves **position** and **audio settings** to SD, and listens for server commands.

## Wiring (client)
- A4988 (AZ): `STEP=14`, `DIR=27`, `ENABLE=12 (LOW=enable)`
- 28BYJ-48 ULN2003 (EL): `IN1=32`, `IN2=33`, `IN3=25`, `IN4=4` (half-step)
- Laser (via NPN like BC547 + resistor): `GPIO=15`
- SD (VSPI): `CS=13`, `SCK=18`, `MISO=19`, `MOSI=23`
- MAX9814: OUT → `ADC1 GPIO 34`
- DAC out → amp/speaker: `GPIO 26`

Set Wi-Fi SSID/PASS, pins, and defaults in `config.h`.

---

## Server Console (Serial @ 115200)

Registering: clients auto-connect to server `CMD_PORT=8080` and send `HELLO <id>`. Give each client a unique `CLIENT_ID` in its `config.h`.

### Commands
- `LIST`  
  Show connected clients and IDs.
- `SEND ALL <command...>`  
  Broadcast a serial-style command to all clients.  
  Example: `SEND ALL LASER MODE TRACK`
- `SEND <id> <command...>`  
  Send to a specific client.  
  Examples:
  SEND 2 BEEP TEST
  SEND 3 SWEEP AZ 45 135 3 PINGPONG
  SEND 1 NEXTSAT
  SEND 4 VOL 160
  
The `<command>` is exactly what you’d type into a client’s USB Serial.

---

## Client Serial Commands

### Tracking / Motion
- `NEXTSAT` — request next satellite from server now  
- `TRACK ON` / `TRACK OFF` — enable/disable tracking  
- `HOME` — reverse path to AZ=0, EL=0 and save pos  
- `SETSITE <lat> <lon> [alt]` — set observer site  
- Example: `SETSITE 52.5200 13.4050 35`
- `SETTIME YYYY-MM-DDTHH:MM:SSZ` — manual UTC time  
- Example: `SETTIME 2025-08-09T12:34:56Z`
- `TIME NOW` — return to UDP time from server  
- `LOADTLE name|line1|line2` — load TLE  
- `TESTSAT n` — load a preset demo TLE: `1=ISS`, `2=NOAA18`, `3=HST`  
- `MOVE AZEL <az> <el>` — move both axes  
- `MOVE AZ <deg>` / `MOVE EL <deg>` — single-axis jog  
- `STATUSTRK` — print tracking readiness/mode/site/TLE/time source

### Demo / Test Motion
- `SWEEP AZ <start> <end> <deg_per_sec> [PINGPONG]`  
Example: `SWEEP AZ 45 135 3 PINGPONG`
- `SWEEP EL <start> <end> <deg_per_sec> [PINGPONG]`  
Example: `SWEEP EL 10 170 2`
- `SWEEP STOP` — stop sweep  
- `DEMO ORBIT <az_deg_per_sec> <el_center> <el_amp> <el_rate_hz>`  
Example: `DEMO ORBIT 8 45 25 0.05`
- `DEMO STOP` — stop demo

### Audio
- `VOL n` — volume 0..255 (default 180)  
Example: `VOL 160`
- `GATE open close` — gate thresholds (0..4095)  
Example: `GATE 300 220`
- `LIMIT n` — limiter ceiling (0..4095)  
Example: `LIMIT 3800`
- `NOISE ON mix floor` / `NOISE OFF` — synthetic motor noise  
Example: `NOISE ON 0.15 220`
- `NOTCH ON hz q` / `NOTCH OFF` — notch filter  
Example: `NOTCH ON 2600 12`
- `STATUS` — print audio & beep settings  
- `SAVESET` / `LOADSET` — save/load `/audio_settings.json`  
- `RESETSET` — reset to `config.h` defaults  
- `FACTORYRESET` — delete `/audio_settings.json` and reset

### Beep / Echo
- `BEEP SET <freqHz> <durMs> <echoDelayMs> <echoDecay0..1> <vol0..255>`  
Example: `BEEP SET 900 140 110 0.45 200`
- `BEEP START ON|OFF` — beep when tracking starts  
- `BEEP END ON|OFF` — beep when tracking ends  
- `BEEP TEST` — play once

### Laser
- `LASER MODE OFF` — always off  
- `LASER MODE ON` — always on  
- `LASER MODE TRACK` — only while **tracking** and **EL in 0..180°**  
- `LASER STATUS` — print current mode

---

## Behavior Notes
- On boot, clients restore last AZ/EL from SD (`/pos.txt`).
- After each satellite, client returns to HOME via **reverse path** and saves position.
- If no satellite is available, the client waits and retries every `SAT_REQUEST_INTERVAL_MS`.
- Laser behavior is tied to **laser mode** and tracking state.

---

## Build Tips
- Library: **SparkFun SGP4 Arduino Library** (`#include <Sgp4.h>`)
- Board package: **ESP32** (use stable v3.x)
- If you run multiple clients: set unique `CLIENT_ID` in each `client_module/config.h`.
