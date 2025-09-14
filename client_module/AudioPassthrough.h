#pragma once
#include <Arduino.h>
#include "config.h"

// init/pass-through loop
void audioInit();
void audioLoop();

// passthrough controls / status
void audioSetVolume(uint8_t vol);
void audioSetPTGain(float mult);
void audioSetLimiter(int lim);
void audioSetNoise(bool on, float mix, int floor);
void audioSetNotch(bool on, float hz, float q);
void audioSetADCAttenuation(int db); // 0,2,6,11 dB
void audioSetMuteWhenIdle(bool enable);
void audioResetToDefaults();
bool audioSaveSettingsToSD();
bool audioLoadSettingsFromSD();
bool audioDeleteSettingsFromSD();
void audioPrintStatus(Stream& s);

// beeps
void audioBeepEnableStart(bool on);
void audioBeepEnableEnd(bool on);
void audioBeepSetParams(float freqHz, int durMs, int echoDelayMs, float echoDecay, uint8_t vol);
void audioBeepSetVolume(uint8_t vol);
void audioBeepPlay();
void audioBeepTest();

// quick tone generator (wire test)
void audioToneTest(uint16_t freq = 1000, uint16_t ms = 800, uint8_t vol = 180, int dacPin = DAC_PIN);