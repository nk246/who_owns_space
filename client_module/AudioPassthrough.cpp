#include <Arduino.h>
#include <SD.h>
#include <ArduinoJson.h>
#include "AudioPassthrough.h"
#include "config.h"
#include "Motors.h"

// ---------- Runtime settings ----------
static uint8_t gVolume = AUDIO_FIXED_VOLUME;   // 0..255
static int gGateOpen = AUDIO_GATE_OPEN;        // 12-bit ADC counts
static int gGateClose = AUDIO_GATE_CLOSE;      // 12-bit ADC counts
static int gLimiter = AUDIO_LIMIT;             // 0..4095
static bool  gNoiseOn = INJECT_NOISE_WHEN_MOVING;
static float gNoiseMix = INJECT_NOISE_MIX;     // 0..1
static int   gNoiseFloor = INJECT_NOISE_FLOOR; // 0..4095
static bool  gNotchOn = NOTCH_ON;
static float gNotchHz = NOTCH_HZ;
static float gNotchQ  = NOTCH_Q;

// Beep params
static bool  gBeepStartOn = BEEP_ON_TRACK_START;
static bool  gBeepEndOn   = BEEP_ON_TRACK_END;
static float gBeepFreqHz  = BEEP_FREQ_HZ;
static int   gBeepDurMs   = BEEP_DUR_MS;
static int   gBeepEchoDelayMs = BEEP_ECHO_DELAY_MS;
static float gBeepEchoDecay   = BEEP_ECHO_DECAY;   // 0..1
static uint8_t gBeepVolume    = BEEP_VOLUME;       // 0..255

// ---------- State ----------
static bool gateOpen = false;
static uint32_t lfsr = 0xA5A5A5A5u;

// Biquad (notch) coefficients and DF2 state
static float b0=1, b1=0, b2=1, a1=0, a2=0;
static float z1=0, z2=0;  // Direct Form II states

static inline int16_t noise12bit() {
  lfsr ^= lfsr << 13; lfsr ^= lfsr >> 17; lfsr ^= lfsr << 5;
  return (int16_t)((lfsr >> 20) & 0x0FFF) - 2048; // -2048..+2047
}

static inline int softClip(int x) {
  const int a = 2048;
  long x3 = (long)x * x * x;
  long y  = (long)x - (x3 / (long)(a * a));
  return (int)y;
}

static void setupNotch(float fs, float f0, float Q) {
  float w0 = 2.0f * PI * f0 / fs;
  float alpha = sinf(w0) / (2.0f * Q);
  float cosw0 = cosf(w0);
  // Notch (biquad)
  float b0n =  1.0f;
  float b1n = -2.0f * cosw0;
  float b2n =  1.0f;
  float a0n =  1.0f + alpha;
  float a1n = -2.0f * cosw0;
  float a2n =  1.0f - alpha;
  b0 = b0n/a0n; b1 = b1n/a0n; b2 = b2n/a0n;
  a1 = a1n/a0n; a2 = a2n/a0n;
  z1 = z2 = 0;
}

void audioInit() {
  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
  if (gNotchOn) setupNotch(8000.0f, gNotchHz, gNotchQ);
  audioLoadSettingsFromSD(); // load saved, if any
}

void audioLoop() {
  int s = analogRead(MIC_PIN) - 2048;
  int absS = abs(s);

  // Keyed gate
  if (motorsIsMoving()) {
    gateOpen = true;
  } else {
    if (!gateOpen) { if (absS >= gGateOpen) gateOpen = true; }
    else           { if (absS <  gGateClose) gateOpen = false; }
  }
  if (!gateOpen) { dacWrite(DAC_PIN, 128); return; }

  // Gain
  s = (int)((long)s * (long)max<uint8_t>(gVolume,1) / 128L);

  // Synthetic noise when quiet & moving
  if (gNoiseOn && motorsIsMoving() && absS < gNoiseFloor) {
    int16_t n = noise12bit();
    float mix = constrain(gNoiseMix, 0.0f, 1.0f);
    s = (int)((1.0f - mix) * s + mix * n);
  }

  // Notch (Direct Form II)
  if (gNotchOn) {
    float x = (float)s;
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    s = (int)y;
  }

  // Limiter & DAC
  s = softClip(s);
  s = constrain(s + 2048, 0, gLimiter);
  dacWrite(DAC_PIN, (uint8_t)(s >> 4)); // 12-bit -> 8-bit DAC
}

// ---------- Control API ----------
void audioSetVolume(uint8_t vol) { gVolume = vol; }
void audioSetLimiter(int limit012) { gLimiter = constrain(limit012, 0, 4095); }
void audioSetGate(int open012, int close012) {
  gGateOpen  = max(0, min(open012, 4095));
  gGateClose = max(0, min(close012, gGateOpen));
}
void audioSetNoise(bool enable, float mix01, int floor012) {
  gNoiseOn = enable;
  gNoiseMix = constrain(mix01, 0.0f, 1.0f);
  gNoiseFloor = max(0, min(floor012, 4095));
}
void audioSetNotch(bool on, float hz, float q) {
  gNotchOn = on;
  gNotchHz = max(10.0f, hz);
  gNotchQ  = max(0.1f, q);
  if (gNotchOn) setupNotch(8000.0f, gNotchHz, gNotchQ);
}

void audioPrintStatus(Stream& s) {
  s.println(F("=== Audio Settings ==="));
  s.printf("VOL        : %u (0..255)\n", gVolume);
  s.printf("GATE       : open=%d  close=%d\n", gGateOpen, gGateClose);
  s.printf("LIMIT      : %d (0..4095)\n", gLimiter);
  s.printf("NOISE      : %s  mix=%.2f  floor=%d\n", gNoiseOn ? "ON":"OFF", gNoiseMix, gNoiseFloor);
  s.printf("NOTCH      : %s  f=%.1f Hz  Q=%.1f\n", gNotchOn ? "ON":"OFF", gNotchHz, gNotchQ);
  audioBeepPrint(s);
}

// ---------- Beep controls ----------
void audioBeepSetParams(float freqHz, int durMs, int echoDelayMs, float echoDecay01, uint8_t volume) {
  gBeepFreqHz = max(50.0f, freqHz);
  gBeepDurMs = max(10, durMs);
  gBeepEchoDelayMs = max(0, echoDelayMs);
  gBeepEchoDecay = constrain(echoDecay01, 0.0f, 1.0f);
  gBeepVolume = volume;
}
void audioBeepEnableStart(bool on) { gBeepStartOn = on; }
void audioBeepEnableEnd(bool on)   { gBeepEndOn   = on; }
void audioBeepPrint(Stream& s) {
  s.println(F("--- Beep Settings ---"));
  s.printf("BEEP START : %s\n", gBeepStartOn ? "ON":"OFF");
  s.printf("BEEP END   : %s\n", gBeepEndOn   ? "ON":"OFF");
  s.printf("FREQ       : %.1f Hz\n", gBeepFreqHz);
  s.printf("DUR        : %d ms\n", gBeepDurMs);
  s.printf("ECHO DELAY : %d ms\n", gBeepEchoDelayMs);
  s.printf("ECHO DECAY : %.2f\n", gBeepEchoDecay);
  s.printf("BEEP VOL   : %u (0..255)\n", gBeepVolume);
}

// Short blocking generator (OK for brief UI cue)
void audioBeepPlay() {
  const int sampleRate = 16000;
  const float twoPiF = 2.0f * PI * gBeepFreqHz;
  const float gain = (float)gBeepVolume / 255.0f * 0.7f; // headroom

  const int Nmain  = (gBeepDurMs * sampleRate) / 1000;
  const int Necho  = (gBeepDurMs * sampleRate) / 1000;
  const int Ndelay = (gBeepEchoDelayMs * sampleRate) / 1000;
  const int Ntotal = Nmain + Ndelay + Necho;

  for (int i = 0; i < Ntotal; i++) {
    float t = (float)i / sampleRate;
    float sm = 0.0f;
    if (i < Nmain) {
      sm += sinf(twoPiF * t);
    }
    if (i >= Ndelay && i < (Ndelay + Necho)) {
      float te = (float)(i - Ndelay) / sampleRate;
      sm += gBeepEchoDecay * sinf(twoPiF * te);
    }
    int v = (int)((sm * gain * 127.0f) + 128.0f); // 8-bit centered
    v = constrain(v, 0, 255);
    dacWrite(DAC_PIN, v);
    delayMicroseconds(1000000 / sampleRate);
  }
}

void audioBeepTest() { audioBeepPlay(); }

// ---------- Persist to SD ----------
bool audioSaveSettingsToSD() {
  StaticJsonDocument<640> doc;
  doc["vol"] = gVolume;
  doc["gate_open"] = gGateOpen;
  doc["gate_close"] = gGateClose;
  doc["limit"] = gLimiter;
  doc["noise_on"] = gNoiseOn;
  doc["noise_mix"] = gNoiseMix;
  doc["noise_floor"] = gNoiseFloor;
  doc["notch_on"] = gNotchOn;
  doc["notch_hz"] = gNotchHz;
  doc["notch_q"]  = gNotchQ;
  // Beep
  doc["beep_start_on"] = gBeepStartOn;
  doc["beep_end_on"]   = gBeepEndOn;
  doc["beep_freq_hz"]  = gBeepFreqHz;
  doc["beep_dur_ms"]   = gBeepDurMs;
  doc["beep_echo_delay_ms"] = gBeepEchoDelayMs;
  doc["beep_echo_decay"]    = gBeepEchoDecay;
  doc["beep_vol"]      = gBeepVolume;

  File f = SD.open("/audio_settings.json", FILE_WRITE);
  if (!f) return false;
  f.print(F(""));
  serializeJson(doc, f);
  f.close();
  return true;
}

bool audioLoadSettingsFromSD() {
  if (!SD.exists("/audio_settings.json")) return false;
  File f = SD.open("/audio_settings.json", FILE_READ);
  if (!f) return false;
  StaticJsonDocument<640> doc;
  auto err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  gVolume     = doc["vol"]         | gVolume;
  gGateOpen   = doc["gate_open"]   | gGateOpen;
  gGateClose  = doc["gate_close"]  | gGateClose;
  gLimiter    = doc["limit"]       | gLimiter;
  gNoiseOn    = doc["noise_on"]    | gNoiseOn;
  gNoiseMix   = doc["noise_mix"]   | gNoiseMix;
  gNoiseFloor = doc["noise_floor"] | gNoiseFloor;
  gNotchOn    = doc["notch_on"]    | gNotchOn;
  gNotchHz    = doc["notch_hz"]    | gNotchHz;
  gNotchQ     = doc["notch_q"]     | gNotchQ;

  gBeepStartOn = doc["beep_start_on"] | gBeepStartOn;
  gBeepEndOn   = doc["beep_end_on"]   | gBeepEndOn;
  gBeepFreqHz  = doc["beep_freq_hz"]  | gBeepFreqHz;
  gBeepDurMs   = doc["beep_dur_ms"]   | gBeepDurMs;
  gBeepEchoDelayMs = doc["beep_echo_delay_ms"] | gBeepEchoDelayMs;
  gBeepEchoDecay   = doc["beep_echo_decay"]    | gBeepEchoDecay;
  gBeepVolume  = doc["beep_vol"]      | gBeepVolume;

  if (gNotchOn) setupNotch(8000.0f, gNotchHz, gNotchQ);
  return true;
}

void audioResetToDefaults() {
  gVolume     = AUDIO_FIXED_VOLUME;
  gGateOpen   = AUDIO_GATE_OPEN;
  gGateClose  = AUDIO_GATE_CLOSE;
  gLimiter    = AUDIO_LIMIT;
  gNoiseOn    = INJECT_NOISE_WHEN_MOVING;
  gNoiseMix   = INJECT_NOISE_MIX;
  gNoiseFloor = INJECT_NOISE_FLOOR;
  gNotchOn    = NOTCH_ON;
  gNotchHz    = NOTCH_HZ;
  gNotchQ     = NOTCH_Q;

  gBeepStartOn = BEEP_ON_TRACK_START;
  gBeepEndOn   = BEEP_ON_TRACK_END;
  gBeepFreqHz  = BEEP_FREQ_HZ;
  gBeepDurMs   = BEEP_DUR_MS;
  gBeepEchoDelayMs = BEEP_ECHO_DELAY_MS;
  gBeepEchoDecay   = BEEP_ECHO_DECAY;
  gBeepVolume  = BEEP_VOLUME;

  if (gNotchOn) setupNotch(8000.0f, gNotchHz, gNotchQ);
  Serial.println(F("Audio settings reset to defaults (config.h)."));
}

bool audioDeleteSettingsFromSD() {
  if (SD.exists("/audio_settings.json")) return SD.remove("/audio_settings.json");
  return true;
}
