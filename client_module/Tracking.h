#pragma once
#include <Arduino.h>

void trackingInit(const char* name, const char* tle1, const char* tle2,
                  double lat, double lon, double alt);
bool trackingGetAzEl(unsigned long unixTime, float& azDeg, float& elDeg);

void trackingGetCurrentSite(double& lat, double& lon, double& alt);
void trackingGetCurrentTLE(String& name, String& l1, String& l2);