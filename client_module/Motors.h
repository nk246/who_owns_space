#pragma once
#include <Arduino.h>
#include <SD.h>

enum LaserMode : uint8_t { LASER_OFF=0, LASER_ON=1, LASER_TRACK=2 };

void motorsInit();
void motorsMoveTo(float azDeg, float elDeg);
void motorsReturnToNull();
void motorsSavePosition();
void motorsLoadPosition();
bool motorsIsMoving();

// Laser/Tracking linkage controls
void motorsSetLaserMode(LaserMode m);
LaserMode motorsGetLaserMode();
void motorsSetTrackingActive(bool active);
