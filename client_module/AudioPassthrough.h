#pragma once
#include <Arduino.h>

void audioInit();
void audioLoop();

// --- runtime controls (Serial) ---
void audioSetVolume(uint8_t vol);                     // 0..255
void audioSetLimiter(int limit012);                   // 0..4095
void audioSetGate(int open012, int close012);         // 12-bit counts
void audioSetNoise(bool enable, float mix01, int floor012);
void audioSetNotch(bool on, float hz, float q);
void audioPrintStatus(Stream& s);
bool audioSaveSettingsToSD();                         // /audio_settings.json
bool audioLoadSettingsFromSD();
void audioResetToDefaults();
bool audioDeleteSettingsFromSD();

// --- beep/echo ---
void audioBeepSetParams(float freqHz, int durMs, int echoDelayMs, float echoDecay01, uint8_t volume);
void audioBeepEnableStart(bool on);
void audioBeepEnableEnd(bool on);
void audioBeepTest();
void audioBeepPrint(Stream& s);
void audioBeepPlay(); // blocking short beep (used on events)
