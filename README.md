# WHO OWNS SPACE?

"Who owns space?" is an art project using satellite trackers to point on starlink satellites using a laser.

# Client Module (ESP32) – Satellite Tracker

Features:
- Send UTC time to Client Modules
- Assign satellites to Client Modules
- Debug and fine tune Client Modules through web interface

## Wiring (Server)
- DS3231(RTC): `SQW=14`, `SCL=27`, `SDA=12`
- SD (VSPI): `CS=5`, `SCK=18`, `MISO=19`, `MOSI=23`

## Access to Web Interface
- Connect phone, tablet or computer via WiFi to `ESP32_Master_Network`using the password `123456789`
- Open your web browser and open `192.168.4.1`
- Now you can play around with all the functions on the web interface. Commands are the same you can find further below in `Serial Commands`


# Client Module (ESP32) – Satellite Tracker

Features:
- Real tracking with SparkFun **Sgp4.h**
- **Test Run** mode (no TLE) to simulate passes
- STOP/START, STEP/GOTO, HOME/HOME SET, SAT NEW
- Laser modes: OFF, ON, TRACK (on only while tracking & within EL)
- Audio passthrough from MAX9814 or piezo → speaker (DAC), with gain, attenuation, notch, idle-mute
- Short configurable beep at start/end of tracking
- SD card init with retries and speed fallback
- Stores last position to `/pos.dat` (returns safely to null via backtrack)

## Wiring (client)
- A4988 (AZ): `STEP=14`, `DIR=27`, `ENABLE=12 (LOW=enable)`
- 28BYJ-48 ULN2003 (EL): `IN1=32`, `IN2=33`, `IN3=5`, `IN4=4` (half-step)
- Laser (via NPN like BC547 + resistor): `GPIO=15`
- SD (VSPI): `CS=13`, `SCK=18`, `MISO=19`, `MOSI=23`
- MAX9814: OUT → `ADC1 GPIO 34`
- DAC out → amp/speaker: `GPIO 26`

> If GPIO4 causes boot messages, move EL_IN4 to 16 and update `config.h`.

## Install
- ESP32 board package 3.2.0+
- Libraries: SparkFun SGP4 Arduino Library (`Sgp4.h`), ArduinoJson (6+)

## Usage
Power up; it joins AP `ESP32_Master_Network` and listens to UDP time from the server, then requests a satellite via TCP (port 80).

### Serial Commands
- `START` / `STOP`
- `HOME` — go to AZ=0 EL=0 via backtrack path
- `HOME SET` — define current as new AZ=0 EL=0 (saves)
- `STEP AZ <steps>` / `STEP EL <steps>`
- `GOTO AZ <deg>` / `GOTO EL <deg>`
- `LASER OFF|ON|TRACK`
- `SAT NEW` — ask server for next satellite

**Audio**
- `AUDIO VOL <0-255>`
- `AUDIO GAIN <mult>` (extra passthrough gain)
- `AUDIO LIMIT <0-4095>`
- `AUDIO IDLEMUTE ON|OFF`
- `AUDIO ADCATTN <0|2|6|11>`
- `AUDIO NOISE <ON|OFF> [mix 0..1] [floor 0..4095]`
- `AUDIO NOTCH <ON|OFF> [hz] [q]`
- `AUDIO RESET DEFAULTS`
- `AUDIO SAVE` / `AUDIO LOAD` / `AUDIO DELETE`

**Beep**
- `BEEP VOL <0-255>`
- `BEEP START <ON|OFF>`
- `BEEP END <ON|OFF>`
- `BEEP SET <freqHz> <durMs> <echoMs> <decay0-1> <vol0-255>`
- `BEEP TEST`

**Test Run (no TLE)**
- `TEST START <az0> <az1> <peakEl> <durSec> [holdSec] [LOOP]`
  - Example: `TEST START 220 320 45 180 10 LOOP`
- `TEST PRESET 1|2|3`
- `TEST STOP`
- `TEST STATUS`

## Files on SD
- `/pos.dat` — last az/el (CSV) for resume/home
- `/audio.cfg` — saved audio settings (created by `AUDIO SAVE`)

## Tuning
- Steps/deg and speeds in `config.h`
- AZ backtrack history avoids cable wrap on return to home
- EL limited to [0°, 180°], laser disabled outside or when not tracking
