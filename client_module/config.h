#pragma once

// ===== WiFi / Server =====
#define WIFI_SSID   "ESP32_Master_Network"
#define WIFI_PASS   "123456789"
#define MASTER_IP   "192.168.4.1"
#define TCP_PORT    80
#define UDP_PORT    4210
#define UDP_CMD_PORT 4212
#define SERVER_REG_PORT 4213

// ===== Debug =====
#define DEBUG 1

// ===== Pins =====
// A4988 (AZ)
#define AZ_STEP_PIN    14
#define AZ_DIR_PIN     27
#define AZ_ENABLE_PIN  12  // LOW = enable

// 28BYJ-48 ULN2003 (EL)
#define EL_IN1  32
#define EL_IN2  33
#define EL_IN3  5
#define EL_IN4  4   // if boot messages persist, move to 16 and rewire

// LASER (via NPN to 5V)
#define LASER_PIN  15

// SD (VSPI)
#define SD_CS    13
#define SD_SCK   18
#define SD_MISO  19
#define SD_MOSI  23

// Audio
#define MIC_PIN  34
#define DAC_PIN  26

// ===== Elevation limits =====
#define EL_MIN_DEG  0.0f
#define EL_MAX_DEG  180.0f

// ===== Laser mode =====
enum LaserMode : uint8_t { LASER_OFF=0, LASER_ON=1, LASER_TRACK=2 };
#define LASER_DEFAULT_MODE LASER_TRACK

// ===== Motion tuning =====
// --- Motor/drive params (AZ) ---
#define AZ_MOTOR_STEPS_PER_REV   200.0f   // NEMA-17
#define AZ_MICROSTEPS            16.0f    // set by your driver (jumpers/DIP/SPI)
#define AZ_GEAR_RATIO            1.0f     // >1.0 if reduction (e.g., 2.0 for 2:1)

// Auto-computed:
#define AZ_STEPS_PER_DEG  ((AZ_MOTOR_STEPS_PER_REV * AZ_MICROSTEPS * AZ_GEAR_RATIO) / 360.0f)
#define EL_STEPS_PER_DEG   10.6667f // ~ 2048 steps / 192 deg ≈ 10.667 steps/deg (28BYJ-48)
#define AZ_MAX_SPEED_DPS   90.0f    // deg per second max slew
#define EL_MAX_SPEED_DPS   45.0f    // deg per second max slew
#define AZ_STEP_DELAY_US   500      // microseconds per step at base speed
#define EL_STEP_DELAY_US   1200

// ===== Audio defaults =====
#define AUDIO_FIXED_VOLUME        180   // 0..255 passthrough base vol
#define AUDIO_LIMIT               3600  // 0..4095
#define INJECT_NOISE_WHEN_MOVING  true
#define INJECT_NOISE_MIX          0.12f // 0..1
#define INJECT_NOISE_FLOOR        180
#define NOTCH_ON                  false
#define NOTCH_HZ                  2500.0f
#define NOTCH_Q                   12.0f

// Beeps
#define BEEP_ON_TRACK_START   false
#define BEEP_ON_TRACK_END     false
#define BEEP_FREQ_HZ          1200.0f
#define BEEP_DUR_MS           180
#define BEEP_ECHO_DELAY_MS    60
#define BEEP_ECHO_DECAY       0.45f
#define BEEP_VOLUME           0     // start quieter Default 120

// Optional boot sanity beep
#define AUDIO_BOOT_TONE_TEST  1

// ===== enable test-run simulator =====
#define USE_TESTRUN 1