#pragma once
#include <Arduino.h>
#include "config.h"

void motorsInit();
void motorsSetTrackingActive(bool on);
bool motorsIsMoving();

void motorsManualStepAZ(int32_t steps);
void motorsManualStepEL(int32_t steps);

void motorsGotoAzDeg(float azDeg);
void motorsGotoElDeg(float elDeg);

// tracking move (rate-limited & smoothed)
void motorsTrackTo(float targetAzDeg, float targetElDeg);

// cable-safe return to null following backtrack
void motorsReturnToNull();
void motorsZeroHere();

void motorsSavePosition();
void motorsLoadPosition();

void motorsSetLaserMode(LaserMode m);
LaserMode motorsGetLaserMode();