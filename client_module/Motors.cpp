#include "Motors.h"
#include "config.h"
#include <Arduino.h>
#include <SD.h>

// ---------- Tuning ----------
static constexpr float AZ_STEPS_PER_REV = 200.0f * 16.0f; // 1.8° motor, 16x microsteps
static constexpr float EL_STEPS_PER_REV = 4096.0f;        // 28BYJ-48
static constexpr int   AZ_PULSE_US      = 700;
static constexpr int   EL_STEP_DELAY_MS = 2;

// ---------- State ----------
static float currentAz = 0.0f;
static float currentEl = 0.0f;
static volatile bool sMoving = false;

static LaserMode gLaserMode = (LaserMode)LASER_DEFAULT_MODE;
static bool gTrackingActive = false;

// ---------- 28BYJ-48 Half-step sequence ----------
static const uint8_t SEQ[8][4] = {
  {1,0,0,0},{1,1,0,0},{0,1,0,0},{0,1,1,0},
  {0,0,1,0},{0,0,1,1},{0,0,0,1},{1,0,0,1}
};

static inline void elWritePhase(uint8_t i) {
  digitalWrite(EL_IN1, SEQ[i][0]);
  digitalWrite(EL_IN2, SEQ[i][1]);
  digitalWrite(EL_IN3, SEQ[i][2]);
  digitalWrite(EL_IN4, SEQ[i][3]);
}

static void applyLaser(float elDeg) {
  bool on = false;
  switch (gLaserMode) {
    case LASER_OFF:   on = false; break;
    case LASER_ON:    on = true;  break;
    case LASER_TRACK: on = (gTrackingActive && elDeg > EL_MIN_DEG && elDeg < EL_MAX_DEG); break;
  }
  digitalWrite(LASER_PIN, on ? HIGH : LOW);
}

static void motorsStepAZ(bool dir, uint32_t steps) {
  sMoving = true;
  digitalWrite(AZ_ENABLE_PIN, LOW); // enable driver
  digitalWrite(AZ_DIR_PIN, dir ? HIGH : LOW);
  for (uint32_t i = 0; i < steps; i++) {
    digitalWrite(AZ_STEP_PIN, HIGH);
    delayMicroseconds(AZ_PULSE_US);
    digitalWrite(AZ_STEP_PIN, LOW);
    delayMicroseconds(AZ_PULSE_US);
  }
  sMoving = false;
}

static void motorsStepEL(int32_t steps) {
  sMoving = true;
  int8_t dir = (steps >= 0) ? +1 : -1;
  uint32_t n = (steps >= 0) ? steps : -steps;
  uint8_t phase = 0;
  for (uint32_t i = 0; i < n; i++) {
    phase = (phase + (dir > 0 ? 1 : 7)) & 0x07;
    elWritePhase(phase);
    delay(EL_STEP_DELAY_MS);
  }
  sMoving = false;
}

bool motorsIsMoving() { return sMoving; }

void motorsSavePosition() {
  File file = SD.open("/pos.txt", FILE_WRITE);
  if (file) {
    file.printf("%.2f,%.2f\n", currentAz, currentEl);
    file.close();
    if (DEBUG) Serial.println(F("[SD] Position saved"));
  }
}

void motorsLoadPosition() {
  File file = SD.open("/pos.txt", FILE_READ);
  if (!file) return;
  if (file.available()) {
    String line = file.readStringUntil('\n');
    sscanf(line.c_str(), "%f,%f", &currentAz, &currentEl);
    if (DEBUG) Serial.printf("[SD] Loaded pos: AZ=%.2f  EL=%.2f\n", currentAz, currentEl);
  }
  file.close();
}

void motorsInit() {
  pinMode(AZ_STEP_PIN, OUTPUT);
  pinMode(AZ_DIR_PIN, OUTPUT);
  pinMode(AZ_ENABLE_PIN, OUTPUT);

  pinMode(EL_IN1, OUTPUT);
  pinMode(EL_IN2, OUTPUT);
  pinMode(EL_IN3, OUTPUT);
  pinMode(EL_IN4, OUTPUT);

  pinMode(LASER_PIN, OUTPUT);
  digitalWrite(LASER_PIN, LOW);
  digitalWrite(AZ_ENABLE_PIN, LOW);

  motorsLoadPosition();
  applyLaser(currentEl);
}

void motorsMoveTo(float azDeg, float elDeg) {
  // Clamp elevation
  if (elDeg < EL_MIN_DEG) elDeg = EL_MIN_DEG;
  if (elDeg > EL_MAX_DEG) elDeg = EL_MAX_DEG;

  // ---------- AZ ----------
  float deltaAz = azDeg - currentAz;
  if (deltaAz > 360.0f)  deltaAz -= 360.0f;
  if (deltaAz < -360.0f) deltaAz += 360.0f;
  uint32_t azSteps = (uint32_t) roundf(fabs(deltaAz) * (AZ_STEPS_PER_REV / 360.0f));
  bool azDir = (deltaAz >= 0.0f);
  if (azSteps > 0) {
    if (DEBUG) Serial.printf("[AZ] Move %.2f° -> %u steps (%s)\n", deltaAz, azSteps, azDir ? "CW" : "CCW");
    motorsStepAZ(azDir, azSteps);
    currentAz = azDeg;
  }

  // ---------- EL ----------
  float deltaEl = elDeg - currentEl;
  int32_t elSteps = (int32_t) llroundf(deltaEl * (EL_STEPS_PER_REV / 360.0f));
  if (elSteps != 0) {
    if (DEBUG) Serial.printf("[EL] Move %.2f° -> %ld steps (%s)\n", deltaEl, (long)elSteps, (elSteps > 0) ? "UP" : "DOWN");
    motorsStepEL(elSteps);
    currentEl = elDeg;
  }

  applyLaser(currentEl);
  motorsSavePosition();
}

void motorsReturnToNull() {
  if (DEBUG) Serial.println(F("[HOME] Returning to null (AZ=0°, EL=0°)"));

  // EL down first
  float targetEl = EL_MIN_DEG;
  float deltaEl = targetEl - currentEl;
  int32_t elSteps = (int32_t) llroundf(deltaEl * (EL_STEPS_PER_REV / 360.0f));
  if (elSteps != 0) motorsStepEL(elSteps);
  currentEl = targetEl;

  // Laser off while homing (explicit)
  digitalWrite(LASER_PIN, LOW);

  // AZ to zero
  float targetAz = 0.0f;
  float deltaAz = targetAz - currentAz;
  uint32_t azSteps = (uint32_t) roundf(fabs(deltaAz) * (AZ_STEPS_PER_REV / 360.0f));
  bool azDir = (deltaAz >= 0.0f);
  if (azSteps > 0) motorsStepAZ(azDir, azSteps);
  currentAz = targetAz;

  motorsSavePosition();
  if (DEBUG) Serial.println(F("[HOME] Done."));
}

void motorsSetLaserMode(LaserMode m) {
  gLaserMode = m;
  applyLaser(currentEl);
}

LaserMode motorsGetLaserMode() {
  return gLaserMode;
}

void motorsSetTrackingActive(bool active) {
  gTrackingActive = active;
  applyLaser(currentEl);
}
