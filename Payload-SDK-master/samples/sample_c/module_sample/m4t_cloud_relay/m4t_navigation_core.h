#ifndef M4T_NAVIGATION_CORE_H
#define M4T_NAVIGATION_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double latitudeDeg;
    double longitudeDeg;
    double altitudeEllipsoidM;
} T_M4tNavigationTarget;

typedef struct {
    bool psdkConnected;
    bool flightValid;
    bool positionValid;
    bool gpsValid;
    bool batteryValid;
    bool velocityValid;
    bool homeSet;
    bool obstacleAvoidanceEnabled;
    bool rthAltitudeValid;
    int gpsFixState;
    uint16_t satellitesUsed;
    double horizontalAccuracyM;
    double verticalAccuracyM;
    uint8_t batteryPercentage;
    double latitudeDeg;
    double longitudeDeg;
    double altitudeEllipsoidM;
    double horizontalSpeedMps;
    double rthAltitudeM;
} T_M4tNavigationAircraftState;

typedef struct {
    double minimumRouteAltitudeM;
    uint8_t maximumHorizontalSpeedMps;
    double maximumHomeRadiusM;
    double minimumTargetHeightM;
    double maximumTargetHeightM;
    uint8_t minimumBatteryPercentage;
    uint16_t minimumSatellites;
    double maximumHorizontalAccuracyM;
    double maximumVerticalAccuracyM;
    double minimumRthAltitudeM;
    double maximumRthAltitudeM;
    double maximumHoverSpeedMps;
    double arrivalHorizontalToleranceM;
    double arrivalVerticalToleranceM;
    unsigned int arrivalConsecutiveSamples;
} T_M4tNavigationSafetyLimits;

typedef struct {
    unsigned int consecutiveSamples;
} T_M4tNavigationArrivalTracker;

void M4tNavigationCore_DefaultLimits(T_M4tNavigationSafetyLimits *limits);
double M4tNavigationCore_HorizontalDistanceM(double latitude1Deg, double longitude1Deg,
                                            double latitude2Deg, double longitude2Deg);
bool M4tNavigationCore_ValidatePreflight(const T_M4tNavigationAircraftState *state,
                                        const T_M4tNavigationSafetyLimits *limits,
                                        char *error, size_t errorSize);
bool M4tNavigationCore_ValidateTarget(const T_M4tNavigationTarget *target,
                                     const T_M4tNavigationTarget *originalHome,
                                     const T_M4tNavigationSafetyLimits *limits,
                                     char *error, size_t errorSize);
bool M4tNavigationCore_UpdateArrival(T_M4tNavigationArrivalTracker *tracker,
                                    bool psdkMissionIdle,
                                    const T_M4tNavigationAircraftState *state,
                                    const T_M4tNavigationTarget *target,
                                    const T_M4tNavigationSafetyLimits *limits);

#ifdef __cplusplus
}
#endif

#endif
