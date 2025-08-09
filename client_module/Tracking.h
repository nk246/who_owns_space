#pragma once
#include <Arduino.h>

// Sgp4 interface
void   trackingInit(const char* name, const char* l1, const char* l2, double lat, double lon, double alt);
void   trackingSetSite(double lat, double lon, double alt);
void   trackingSetTLE(const char* name, const char* l1, const char* l2);
bool   trackingGetAzEl(unsigned long unixTime, float& azOut, float& elOut);
bool   trackingReady();
void   trackingGetCurrentSite(double& lat, double& lon, double& alt);
void   trackingGetCurrentTLE(String& name, String& l1, String& l2);
