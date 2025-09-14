#include "Tracking.h"
#include <Sgp4.h>   // SparkFun SGP4
#include <math.h>

static Sgp4 sat;
static double sLat=0, sLon=0, sAlt=0;
static String sName, sL1, sL2;

void trackingInit(const char* name, const char* tle1, const char* tle2,
                  double lat, double lon, double alt) {
  sLat=lat; sLon=lon; sAlt=alt;
  sName = name; sL1=tle1; sL2=tle2;

  sat.site(lat * DEG_TO_RAD, lon * DEG_TO_RAD, alt); // SparkFun expects radians for site lat/lon
  char nm[32]; strncpy(nm, name, sizeof(nm)); nm[sizeof(nm)-1]=0;
  char l1[130]; strncpy(l1, tle1, sizeof(l1)); l1[sizeof(l1)-1]=0;
  char l2[130]; strncpy(l2, tle2, sizeof(l2)); l2[sizeof(l2)-1]=0;
  sat.init(nm, l1, l2);
}

bool trackingGetAzEl(unsigned long unixTime, float& azDeg, float& elDeg) {
  sat.findsat((unsigned long)unixTime);
  // SparkFun Sgp4 gives az/el in degrees already:
  azDeg = sat.satAz;
  elDeg = sat.satEl;
  return (elDeg >= 0.0f); // is visible above horizon
}

void trackingGetCurrentSite(double& lat, double& lon, double& alt) { lat=sLat; lon=sLon; alt=sAlt; }
void trackingGetCurrentTLE(String& name, String& l1, String& l2){ name=sName; l1=sL1; l2=sL2; }