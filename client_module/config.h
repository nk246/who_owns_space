#pragma once
#include <Arduino.h>

// ------------------- WiFi Settings -------------------
inline constexpr char WIFI_SSID[] = "ESP32_Master_Network";
inline constexpr char WIFI_PASS[] = "123456789";
inline constexpr char MASTER_IP[] = "192.168.4.1";
inline constexpr uint16_t TCP_PORT = 80;     // satellite assignment
inline constexpr uint16_t UDP_PORT = 4210;   // time broadcast

// ------------------- Command Channel -------------------
inline constexpr uint16_t CMD_PORT = 8080;   // master command server port
inline constexpr uint8_t  CLIENT_ID = 1;     // <-- set a unique ID per client (1,2,3,...)

// ------------------- Retry Settings -------------------
inline constexpr unsigned long SAT_REQUEST_INTERVAL_MS = 10000; // Request every 10s if none

// ------------------- Motor Pins -------------------
// NEMA17 + A4988 (Azimuth)
inline constexpr uint8_t AZ_STEP_PIN   = 14;
inline constexpr uint8_t AZ_DIR_PIN    = 27;
inline constexpr uint8_t AZ_ENABLE_PIN = 12;  // enable (LOW = on)

// 28BYJ-48 + ULN2003 (Elevation)
inline constexpr uint8_t EL_IN1 = 32;
inline constexpr uint8_t EL_IN2 = 33;
inline constexpr uint8_t EL_IN3 = 25;
inline constexpr uint8_t EL_IN4 = 4;

// ------------------- Laser Pin & Mode -------------------
inline constexpr uint8_t LASER_PIN = 2;
// Laser modes: 0=OFF, 1=ON, 2=TRACK (only on while tracking & EL within range)
inline constexpr uint8_t LASER_DEFAULT_MODE = 2;

// ------------------- SD Card Pins -------------------
inline constexpr uint8_t SD_CS   = 5;
inline constexpr uint8_t SD_SCK  = 18;
inline constexpr uint8_t SD_MISO = 19;
inline constexpr uint8_t SD_MOSI = 23;

// ------------------- Audio -------------------
inline constexpr uint8_t MIC_PIN = 34;   // MAX9814 to ADC1
inline constexpr uint8_t DAC_PIN = 26;   // ESP32 DAC pin 26 -> speaker amp

// ------------------- Elevation Limits -------------------
inline constexpr float EL_MIN_DEG = 0.0f;
inline constexpr float EL_MAX_DEG = 180.0f;

// ------------------- Audio controls (defaults) -------------------
inline constexpr bool   AUDIO_USE_POT     = false;
inline constexpr uint8_t VOLUME_POT_PIN   = 35;        // ignored if no pot
inline constexpr uint8_t AUDIO_FIXED_VOLUME = 180;     // 0..255

// Noise gate thresholds (12-bit ADC counts)
inline constexpr int AUDIO_GATE_OPEN  = 250;
inline constexpr int AUDIO_GATE_CLOSE = 180;

// Limiter threshold (post-gain), 0..4095
inline constexpr int AUDIO_LIMIT = 3600;

// Synthetic noise when motors running (to keep sound present)
inline constexpr bool  INJECT_NOISE_WHEN_MOVING = true;
inline constexpr float INJECT_NOISE_MIX   = 0.10f;     // 0..1
inline constexpr int   INJECT_NOISE_FLOOR = 180;       // 0..4095

// Optional narrow notch to tame squeal
inline constexpr bool  NOTCH_ON  = false;
inline constexpr float NOTCH_HZ  = 2600.0f;
inline constexpr float NOTCH_Q   = 12.0f;

// ------------------- Beep/Echo defaults -------------------
inline constexpr bool  BEEP_ON_TRACK_START = true;
inline constexpr bool  BEEP_ON_TRACK_END   = true;
inline constexpr float BEEP_FREQ_HZ        = 800.0f;
inline constexpr int   BEEP_DUR_MS         = 150;      // main tone duration
inline constexpr int   BEEP_ECHO_DELAY_MS  = 120;      // ms after main tone
inline constexpr float BEEP_ECHO_DECAY     = 0.5f;     // 0..1
inline constexpr uint8_t BEEP_VOLUME       = 180;      // 0..255 loudness

// ------------------- Debug -------------------
inline constexpr bool DEBUG = true;
