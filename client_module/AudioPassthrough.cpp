#include "AudioPassthrough.h"
#include <SD.h>

// extern from Motors.cpp
extern bool motorsIsMoving();

static uint8_t gVolume = AUDIO_FIXED_VOLUME;
static float   gPTGain = 1.0f;
static int     gLimiter     = AUDIO_LIMIT;
static bool    gNoiseOn     = INJECT_NOISE_WHEN_MOVING;
static float   gNoiseMix    = INJECT_NOISE_MIX;
static int     gNoiseFloor  = INJECT_NOISE_FLOOR;
static bool    gNotchOn     = NOTCH_ON;
static float   gNotchHz     = NOTCH_HZ;
static float   gNotchQ      = NOTCH_Q;

static bool  gBeepStartOn = BEEP_ON_TRACK_START;
static bool  gBeepEndOn   = BEEP_ON_TRACK_END;
static float gBeepFreqHz  = BEEP_FREQ_HZ;
static int   gBeepDurMs   = BEEP_DUR_MS;
static int   gBeepEchoDelayMs = BEEP_ECHO_DELAY_MS;
static float gBeepEchoDecay   = BEEP_ECHO_DECAY;
static uint8_t gBeepVolume    = BEEP_VOLUME;

static bool gMuteWhenIdle = true;
static int  gADCAttnDb = 11;

// notch IIR
static float b0_=1, b1_=0, b2_=1, a1_=0, a2_=0, z1_=0, z2_=0;
static uint32_t lfsr = 0xA5A5A5A5u;

static inline int16_t noise12() {
  lfsr ^= lfsr << 13; lfsr ^= lfsr >> 17; lfsr ^= lfsr << 5;
  return (int16_t)((lfsr >> 20) & 0x0FFF) - 2048;
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
  float b0n =  1.0f;
  float b1n = -2.0f * cosw0;
  float b2n =  1.0f;
  float a0n =  1.0f + alpha;
  float a1n = -2.0f * cosw0;
  float a2n =  1.0f - alpha;
  b0_ = b0n/a0n; b1_ = b1n/a0n; b2_ = b2n/a0n;
  a1_ = a1n/a0n; a2_ = a2n/a0n;
  z1_ = z2_ = 0;
}

void audioSetADCAttenuation(int db) {
  gADCAttnDb = db;
  if (db == 0)      analogSetPinAttenuation(MIC_PIN, ADC_0db);
  else if (db == 2) analogSetPinAttenuation(MIC_PIN, ADC_2_5db);
  else if (db == 6) analogSetPinAttenuation(MIC_PIN, ADC_6db);
  else              analogSetPinAttenuation(MIC_PIN, ADC_11db);
}

void audioInit() {
  pinMode(DAC_PIN, OUTPUT);
  dacWrite(DAC_PIN, 128);

  analogReadResolution(12);
  audioSetADCAttenuation(gADCAttnDb);

  if (gNotchOn) setupNotch(8000.0f, gNotchHz, gNotchQ);

#if AUDIO_BOOT_TONE_TEST
  audioToneTest(1000, 180, 120, DAC_PIN);
#endif
}

void audioLoop() {
  if (gMuteWhenIdle && !motorsIsMoving()) {
    dacWrite(DAC_PIN, 128);
    return;
  }

  int s = analogRead(MIC_PIN) - 2048;

  float g = (float)max<uint8_t>(gVolume,1) / 128.0f;
  g *= (gPTGain <= 0 ? 1.0f : gPTGain);
  s = (int)roundf((float)s * g);

  if (gNoiseOn && abs(s) < gNoiseFloor) {
    int16_t n = noise12();
    float mix = (gNoiseMix<0)?0:((gNoiseMix>1)?1:gNoiseMix);
    s = (int)((1.0f - mix)*s + mix*n);
  }

  if (gNotchOn) {
    float x = (float)s;
    float y = b0_*x + z1_;
    z1_ = b1_*x - a1_*y + z2_;
    z2_ = b2_*x - a2_*y;
    s = (int)y;
  }

  s = softClip(s);
  s = constrain(s + 2048, 0, gLimiter);
  dacWrite(DAC_PIN, (uint8_t)(s >> 4));
}

void audioSetVolume(uint8_t vol) { gVolume = vol; }
void audioSetPTGain(float mult)  { gPTGain = constrain(mult, 0.1f, 12.0f); }
void audioSetLimiter(int lim)    { gLimiter = constrain(lim, 0, 4095); }
void audioSetNoise(bool on, float mix, int floor) {
  gNoiseOn = on; gNoiseMix = constrain(mix, 0.0f, 1.0f); gNoiseFloor = constrain(floor, 0, 4095);
  if (gNoiseOn && gNoiseMix < 0.01f) gNoiseMix = 0.01f;
}
void audioSetNotch(bool on, float hz, float q) {
  gNotchOn = on; gNotchHz = hz; gNotchQ = q;
  if (gNotchOn) setupNotch(8000.0f, gNotchHz, gNotchQ);
}
void audioSetMuteWhenIdle(bool enable) { gMuteWhenIdle = enable; }

void audioResetToDefaults() {
  gVolume = AUDIO_FIXED_VOLUME;
  gPTGain = 1.0f;
  gLimiter = AUDIO_LIMIT;
  gNoiseOn = INJECT_NOISE_WHEN_MOVING;
  gNoiseMix = INJECT_NOISE_MIX;
  gNoiseFloor = INJECT_NOISE_FLOOR;
  gNotchOn = NOTCH_ON;
  gNotchHz = NOTCH_HZ;
  gNotchQ  = NOTCH_Q;
  audioSetADCAttenuation(11);
}

bool audioSaveSettingsToSD() {
  File f = SD.open("/audio.cfg", FILE_WRITE);
  if (!f) return false;
  f.printf("%u,%.3f,%d,%d,%.3f,%d,%d,%f,%f,%d,%u\n",
           gVolume, gPTGain, gLimiter, (int)gNoiseOn, gNoiseMix, gNoiseFloor,
           (int)gNotchOn, gNotchHz, gNotchQ, gADCAttnDb, gBeepVolume);
  f.close();
  return true;
}
bool audioLoadSettingsFromSD() {
  File f = SD.open("/audio.cfg", FILE_READ);
  if (!f) return false;
  String line = f.readStringUntil('\n'); f.close();
  int noiseOn=0, notchOn=0;
  int lim, floor, attn, beepV;
  int vol;
  float mix, hz, q, ptg;
  if (sscanf(line.c_str(), "%d,%f,%d,%d,%f,%d,%d,%f,%f,%d,%d",
             &vol, &ptg, &lim, &noiseOn, &mix, &floor, &notchOn, &hz, &q, &attn, &beepV) == 11) {
    gVolume = constrain(vol,0,255);
    gPTGain = constrain(ptg, 0.1f, 12.0f);
    gLimiter = constrain(lim,0,4095);
    gNoiseOn = (noiseOn!=0);
    gNoiseMix = constrain(mix,0.0f,1.0f);
    gNoiseFloor = constrain(floor,0,4095);
    gNotchOn = (notchOn!=0);
    gNotchHz = hz; gNotchQ=q;
    gBeepVolume = (uint8_t)constrain(beepV,0,255);
    audioSetADCAttenuation(attn);
    if (gNotchOn) setupNotch(8000.0f, gNotchHz, gNotchQ);
    return true;
  }
  return false;
}
bool audioDeleteSettingsFromSD() { return SD.remove("/audio.cfg"); }

void audioPrintStatus(Stream& s) {
  s.printf("Audio: vol=%u ptGain=%.2f limit=%d noise=%s mix=%.2f floor=%d notch=%s f=%.1fHz Q=%.1f idleMute=%s attn=%ddB beepVol=%u\n",
           gVolume, gPTGain, gLimiter, gNoiseOn?"ON":"OFF", gNoiseMix, gNoiseFloor,
           gNotchOn?"ON":"OFF", gNotchHz, gNotchQ, gMuteWhenIdle?"ON":"OFF", gADCAttnDb, gBeepVolume);
}

// Beeps
void audioBeepEnableStart(bool on){ gBeepStartOn = on; }
void audioBeepEnableEnd(bool on)  { gBeepEndOn   = on; }
void audioBeepSetParams(float f, int d, int ed, float dec, uint8_t v) {
  gBeepFreqHz = max(50.0f,f);
  gBeepDurMs  = max(10,d);
  gBeepEchoDelayMs = max(0,ed);
  gBeepEchoDecay   = constrain(dec,0.0f,1.0f);
  gBeepVolume      = v;
}
void audioBeepSetVolume(uint8_t v) { gBeepVolume = v; }

void audioToneTest(uint16_t freq, uint16_t ms, uint8_t vol, int dacPin) {
  const int fs = 16000;
  const float w = 2.0f * PI * freq;
  for (int i=0; i < (int)((ms/1000.0f)*fs); i++) {
    float t = (float)i/fs;
    int v = 128 + (int)(sinf(w*t) * (vol/255.0f) * 120.0f);
    v = constrain(v, 0, 255);
    dacWrite(dacPin, v);
    delayMicroseconds(1000000 / fs);
  }
  dacWrite(DAC_PIN, 128);
}

static void playBeep(bool enabled) {
  if (!enabled) return;
  const int fs = 16000;
  const float w = 2.0f * PI * gBeepFreqHz;
  const float gain = (float)gBeepVolume / 255.0f * 0.7f;

  const int Nmain  = (gBeepDurMs * fs)/1000;
  const int Necho  = (gBeepDurMs * fs)/1000;
  const int Ndelay = (gBeepEchoDelayMs * fs)/1000;
  const int Ntotal = Nmain + Ndelay + Necho;

  for (int i=0;i<Ntotal;i++) {
    float t = (float)i / fs;
    float sm = 0.0f;
    if (i < Nmain) sm += sinf(w * t);
    if (i >= Ndelay && i < (Ndelay+Necho)) {
      float te = (float)(i - Ndelay) / fs;
      sm += gBeepEchoDecay * sinf(w * te);
    }
    int v = 128 + (int)(sm*gain*127.0f);
    v = constrain(v, 0, 255);
    dacWrite(DAC_PIN, v);
    delayMicroseconds(1000000 / fs);
  }
  dacWrite(DAC_PIN, 128);
}

void audioBeepPlay() { playBeep(true); }
void audioBeepTest() { playBeep(true); }