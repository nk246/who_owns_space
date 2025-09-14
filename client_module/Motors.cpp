#include "Motors.h"
#include <SD.h>
#include <math.h>

// ==== State ====
static volatile bool sTrackingActive = false;

static float sAzDeg = 0.0f;   // current mechanical estimate (can be outside 0..360 but bounded)
static float sElDeg = 0.0f;
static LaserMode sLaserMode = LASER_DEFAULT_MODE;

// backtrack history for AZ: record signed step deltas
#define AZ_HIST_MAX 600
static int16_t azHist[AZ_HIST_MAX];
static int azHistLen = 0;

// EL driver half-step sequence
static const uint8_t EL_SEQ[8][4] = {
  {1,0,0,0},{1,1,0,0},{0,1,0,0},{0,1,1,0},
  {0,0,1,0},{0,0,1,1},{0,0,0,1},{1,0,0,1}
};
static int elPhase = 0;

// Keep any angle in [0..360)
static inline float norm360(float a){
  a = fmodf(a, 360.0f);
  if (a < 0) a += 360.0f;
  return a;
}

// Map 'target' (wrapped 0..360) to the nearest equivalent angle (... -720, -360, 0, 360, 720 ...)
// around the current reference 'ref' (unbounded). Result is unbounded but closest to 'ref'.
static inline float unwrapNearest(float targetWrappedDeg, float refDeg){
  float tw = norm360(targetWrappedDeg);
  float k  = roundf((refDeg - tw) / 360.0f);
  return tw + 360.0f * k;
}

// Keep sAzDeg bounded to [-AZ_STATE_LIMIT_DEG, +AZ_STATE_LIMIT_DEG]
#ifndef AZ_STATE_LIMIT_DEG
#define AZ_STATE_LIMIT_DEG 360.0f
#endif

static inline void clampAzState(){
  while (sAzDeg >  AZ_STATE_LIMIT_DEG) sAzDeg -= 360.0f;
  while (sAzDeg < -AZ_STATE_LIMIT_DEG) sAzDeg += 360.0f;
}

static inline void laserUpdateRuntime() {
  if (sLaserMode == LASER_OFF) { digitalWrite(LASER_PIN, LOW); return; }
  if (sLaserMode == LASER_ON)  { digitalWrite(LASER_PIN, HIGH); return; }
  // TRACK mode: only on when within EL limits and tracking active
  if (sTrackingActive && sElDeg >= EL_MIN_DEG && sElDeg <= EL_MAX_DEG) digitalWrite(LASER_PIN, HIGH);
  else digitalWrite(LASER_PIN, LOW);
}

void motorsSetLaserMode(LaserMode m){ sLaserMode=m; laserUpdateRuntime(); }
LaserMode motorsGetLaserMode(){ return sLaserMode; }

void motorsInit() {
  pinMode(AZ_STEP_PIN, OUTPUT);
  pinMode(AZ_DIR_PIN, OUTPUT);
  pinMode(AZ_ENABLE_PIN, OUTPUT);
  digitalWrite(AZ_ENABLE_PIN, LOW);

  pinMode(EL_IN1, OUTPUT);
  pinMode(EL_IN2, OUTPUT);
  pinMode(EL_IN3, OUTPUT);
  pinMode(EL_IN4, OUTPUT);

  pinMode(LASER_PIN, OUTPUT);
  digitalWrite(LASER_PIN, LOW);

  motorsLoadPosition();
  laserUpdateRuntime();
}

void motorsSetTrackingActive(bool on){ sTrackingActive = on; laserUpdateRuntime(); }
bool motorsIsMoving(){ return sTrackingActive; }

// ===== Low-level steppers =====
static void azStepSigned(int32_t steps, unsigned int usDelay) {
  if (steps == 0) return;
  digitalWrite(AZ_DIR_PIN, steps > 0 ? HIGH : LOW);
  int32_t n = abs(steps);
  for (int32_t i=0;i<n;i++) {
    digitalWrite(AZ_STEP_PIN, HIGH); delayMicroseconds(usDelay);
    digitalWrite(AZ_STEP_PIN, LOW);  delayMicroseconds(usDelay);
  }
  sAzDeg += (float)steps / AZ_STEPS_PER_DEG; // signed already
  clampAzState();                             // keep state within [-AZ_STATE_LIMIT_DEG..+AZ_STATE_LIMIT_DEG]
  if (azHistLen < AZ_HIST_MAX) azHist[azHistLen++] = (int16_t)steps;
}

static void elWritePhase(int ph) {
  digitalWrite(EL_IN1, EL_SEQ[ph][0]);
  digitalWrite(EL_IN2, EL_SEQ[ph][1]);
  digitalWrite(EL_IN3, EL_SEQ[ph][2]);
  digitalWrite(EL_IN4, EL_SEQ[ph][3]);
}

static void elStepSigned(int32_t steps, unsigned int usDelay) {
  if (steps == 0) return;
  int dir = (steps > 0) ? 1 : -1;
  int32_t n = abs(steps);
  for (int32_t i=0;i<n;i++) {
    elPhase = (elPhase + dir) & 7;
    elWritePhase(elPhase);
    delayMicroseconds(usDelay);
  }
  sElDeg += (float)steps / EL_STEPS_PER_DEG;
}

// ===== Manual =====
void motorsManualStepAZ(int32_t steps) { azStepSigned(steps, AZ_STEP_DELAY_US); motorsSavePosition(); }
void motorsManualStepEL(int32_t steps) {
  float next = sElDeg + (float)steps / EL_STEPS_PER_DEG;
  if (next < EL_MIN_DEG || next > EL_MAX_DEG) return;
  elStepSigned(steps, EL_STEP_DELAY_US);
  motorsSavePosition();
}

// ===== Absolute moves =====
void motorsGotoAzDeg(float azDeg) {
  // unwrap the target to the nearest equivalent angle around current state
  float target = unwrapNearest(azDeg, sAzDeg);
  float d = target - sAzDeg;
  int32_t steps = (int32_t)roundf(d * AZ_STEPS_PER_DEG);
  azStepSigned(steps, AZ_STEP_DELAY_US);
  motorsSavePosition();
}

void motorsGotoElDeg(float elDeg) {
  if (elDeg < EL_MIN_DEG) elDeg = EL_MIN_DEG;
  if (elDeg > EL_MAX_DEG) elDeg = EL_MAX_DEG;
  float d = elDeg - sElDeg;
  int32_t steps = (int32_t)roundf(d * EL_STEPS_PER_DEG);
  elStepSigned(steps, EL_STEP_DELAY_US);
  motorsSavePosition();
}

// ===== Tracking =====
void motorsTrackTo(float targetAzDeg, float targetElDeg) {
  sTrackingActive = true;

  // clamp EL
  if (targetElDeg < EL_MIN_DEG) targetElDeg = EL_MIN_DEG;
  if (targetElDeg > EL_MAX_DEG) targetElDeg = EL_MAX_DEG;

  // compute deltas limited by speed
  static unsigned long last = 0;
  unsigned long now = millis();
  float dt = (last==0)?0.02f:((now-last)/1000.0f);
  if (dt < 0.01f) dt = 0.01f;
  last = now;

  // AZ: unwrap target to the nearest turn around current state (prevents multi-rev chasing)
  float targetAzUnwrapped = unwrapNearest(targetAzDeg, sAzDeg);
  float dAz = targetAzUnwrapped - sAzDeg;

  float maxAzMove = AZ_MAX_SPEED_DPS * dt;
  if (dAz >  maxAzMove) dAz =  maxAzMove;
  if (dAz < -maxAzMove) dAz = -maxAzMove;

  float dEl = targetElDeg - sElDeg;
  float maxElMove = EL_MAX_SPEED_DPS * dt;
  if (dEl >  maxElMove) dEl =  maxElMove;
  if (dEl < -maxElMove) dEl = -maxElMove;

  int32_t azSteps = (int32_t)roundf(dAz * AZ_STEPS_PER_DEG);
  int32_t elSteps = (int32_t)roundf(dEl * EL_STEPS_PER_DEG);

  if (azSteps) azStepSigned(azSteps, AZ_STEP_DELAY_US);
  if (elSteps) elStepSigned(elSteps, EL_STEP_DELAY_US);

  laserUpdateRuntime();
}

void motorsReturnToNull() {
  // EL first to horizon (down/up to 0)
  motorsGotoElDeg(0.0f);

  // AZ backtrack reverse history
  for (int i = azHistLen - 1; i >= 0; --i) {
    int16_t st = azHist[i];
    if (st) azStepSigned(-st, AZ_STEP_DELAY_US);
  }
  azHistLen = 0;

  // normalize state
  sAzDeg = 0.0f; sElDeg = 0.0f;
  motorsSavePosition();
  laserUpdateRuntime();
}

void motorsZeroHere() {
  sAzDeg = 0.0f; sElDeg = 0.0f;
  azHistLen = 0;
  motorsSavePosition();
}

void motorsSavePosition() {
  clampAzState(); // keep stored value tidy/bounded
  File f = SD.open("/pos.dat", FILE_WRITE);
  if (!f) return;
  f.printf("%.4f,%.4f\n", sAzDeg, sElDeg);
  f.close();
}

void motorsLoadPosition() {
  File f = SD.open("/pos.dat", FILE_READ);
  if (!f) { sAzDeg=0; sElDeg=0; return; }
  String line = f.readStringUntil('\n');
  f.close();
  float az=0,el=0;
  if (sscanf(line.c_str(), "%f,%f", &az, &el)==2) {
    sAzDeg=az; sElDeg=el;
    clampAzState();
  } else { sAzDeg=0; sElDeg=0; }
}
