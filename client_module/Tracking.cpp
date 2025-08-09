#include "Tracking.h"
#include <Sgp4.h>

static Sgp4 sat;
static bool gReady = false;

static String tleName;
static String tle1;
static String tle2;

static double sLat=0.0, sLon=0.0, sAlt=0.0;

void trackingInit(const char* name, const char* l1, const char* l2, double lat, double lon, double alt) {
  trackingSetSite(lat, lon, alt);
  trackingSetTLE(name, l1, l2);
}

void trackingSetSite(double lat, double lon, double alt) {
  sLat = lat; sLon = lon; sAlt = alt;
  sat.site(lat * DEG_TO_RAD, lon * DEG_TO_RAD, alt); // SparkFun expects radians for lat/lon
}

void trackingSetTLE(const char* name, const char* l1, const char* l2) {
  tleName = name; tle1 = l1; tle2 = l2;
  char nm[40];  strncpy(nm, name, sizeof(nm));  nm[sizeof(nm)-1]=0;
  char L1[130]; strncpy(L1, l1, sizeof(L1));    L1[sizeof(L1)-1]=0;
  char L2[130]; strncpy(L2, l2, sizeof(L2));    L2[sizeof(L2)-1]=0;
  gReady = sat.init(nm, L1, L2);
}

bool trackingGetAzEl(unsigned long unixTime, float& azOut, float& elOut) {
  if (!gReady) return false;
  sat.findsat((unsigned long)unixTime);
  azOut = sat.satAz * RAD_TO_DEG;
  elOut = sat.satEl * RAD_TO_DEG;
  // Treat <= 0 deg as not visible
  if (elOut <= 0.0f) return false;
  return true;
}

bool trackingReady() { return gReady; }

void trackingGetCurrentSite(double& lat, double& lon, double& alt) {
  lat = sLat; lon = sLon; alt = sAlt;
}

void trackingGetCurrentTLE(String& name, String& l1, String& l2) {
  name = tleName; l1 = tle1; l2 = tle2;
}
