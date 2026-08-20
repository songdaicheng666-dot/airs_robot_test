#include "m4t_navigation_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define M4T_EARTH_RADIUS_M 6371008.8

static bool M4tNavigationCore_Fail(char *error, size_t errorSize, const char *message)
{
    if (error != NULL && errorSize > 0) {
        snprintf(error, errorSize, "%s", message);
    }
    return false;
}

void M4tNavigationCore_DefaultLimits(T_M4tNavigationSafetyLimits *limits)
{
    if (limits == NULL) {
        return;
    }
    *limits = (T_M4tNavigationSafetyLimits) {
        .minimumRouteAltitudeM = 10.0,
        .maximumHorizontalSpeedMps = 3,
        .maximumHomeRadiusM = 100.0,
        .minimumTargetHeightM = 5.0,
        .maximumTargetHeightM = 30.0,
        .minimumBatteryPercentage = 50,
        .minimumSatellites = 12,
        .maximumHorizontalAccuracyM = 2.0,
        .maximumVerticalAccuracyM = 3.0,
        .minimumRthAltitudeM = 20.0,
        .maximumRthAltitudeM = 120.0,
        .maximumHoverSpeedMps = 0.5,
        .arrivalHorizontalToleranceM = 3.0,
        .arrivalVerticalToleranceM = 3.0,
        .arrivalConsecutiveSamples = 3,
    };
}

double M4tNavigationCore_HorizontalDistanceM(double latitude1Deg, double longitude1Deg,
                                            double latitude2Deg, double longitude2Deg)
{
    double latitude1 = latitude1Deg * M_PI / 180.0;
    double latitude2 = latitude2Deg * M_PI / 180.0;
    double deltaLatitude = (latitude2Deg - latitude1Deg) * M_PI / 180.0;
    double deltaLongitude = (longitude2Deg - longitude1Deg) * M_PI / 180.0;
    double a = sin(deltaLatitude / 2.0) * sin(deltaLatitude / 2.0) +
               cos(latitude1) * cos(latitude2) *
               sin(deltaLongitude / 2.0) * sin(deltaLongitude / 2.0);
    return M4T_EARTH_RADIUS_M * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

bool M4tNavigationCore_ValidatePreflight(const T_M4tNavigationAircraftState *state,
                                        const T_M4tNavigationSafetyLimits *limits,
                                        char *error, size_t errorSize)
{
    if (state == NULL || limits == NULL) {
        return M4tNavigationCore_Fail(error, errorSize, "invalid preflight input");
    }
    if (!state->psdkConnected || !state->flightValid || !state->positionValid ||
        !state->gpsValid || !state->batteryValid || !state->velocityValid) {
        return M4tNavigationCore_Fail(error, errorSize, "required PSDK telemetry is unavailable");
    }
    if (state->gpsFixState != 3 && state->gpsFixState != 4) {
        return M4tNavigationCore_Fail(error, errorSize, "GPS fix must be 3D or GPS+DR");
    }
    if (state->satellitesUsed < limits->minimumSatellites) {
        return M4tNavigationCore_Fail(error, errorSize, "insufficient GPS satellites");
    }
    if (!isfinite(state->horizontalAccuracyM) ||
        state->horizontalAccuracyM > limits->maximumHorizontalAccuracyM) {
        return M4tNavigationCore_Fail(error, errorSize, "horizontal GPS accuracy is outside limit");
    }
    if (!isfinite(state->verticalAccuracyM) ||
        state->verticalAccuracyM > limits->maximumVerticalAccuracyM) {
        return M4tNavigationCore_Fail(error, errorSize, "vertical GPS accuracy is outside limit");
    }
    if (state->batteryPercentage < limits->minimumBatteryPercentage) {
        return M4tNavigationCore_Fail(error, errorSize, "battery is below navigation threshold");
    }
    if (!state->homeSet) {
        return M4tNavigationCore_Fail(error, errorSize, "aircraft Home point is not set");
    }
    if (!state->obstacleAvoidanceEnabled) {
        return M4tNavigationCore_Fail(error, errorSize, "obstacle avoidance is not enabled");
    }
    if (!state->rthAltitudeValid || state->rthAltitudeM < limits->minimumRthAltitudeM ||
        state->rthAltitudeM > limits->maximumRthAltitudeM) {
        return M4tNavigationCore_Fail(error, errorSize, "RTH altitude is outside 20-120m");
    }
    if (error != NULL && errorSize > 0) {
        error[0] = '\0';
    }
    return true;
}

bool M4tNavigationCore_ValidateTarget(const T_M4tNavigationTarget *target,
                                     const T_M4tNavigationTarget *originalHome,
                                     const T_M4tNavigationSafetyLimits *limits,
                                     char *error, size_t errorSize)
{
    double distance;
    double targetHeight;

    if (target == NULL || originalHome == NULL || limits == NULL ||
        !isfinite(target->latitudeDeg) || !isfinite(target->longitudeDeg) ||
        !isfinite(target->altitudeEllipsoidM) || target->latitudeDeg < -90.0 ||
        target->latitudeDeg > 90.0 || target->longitudeDeg < -180.0 ||
        target->longitudeDeg > 180.0) {
        return M4tNavigationCore_Fail(error, errorSize, "invalid geodetic target");
    }
    distance = M4tNavigationCore_HorizontalDistanceM(
        originalHome->latitudeDeg, originalHome->longitudeDeg,
        target->latitudeDeg, target->longitudeDeg);
    if (!isfinite(distance) || distance > limits->maximumHomeRadiusM) {
        return M4tNavigationCore_Fail(error, errorSize, "target is outside original Home radius");
    }
    targetHeight = target->altitudeEllipsoidM - originalHome->altitudeEllipsoidM;
    if (targetHeight < limits->minimumTargetHeightM ||
        targetHeight > limits->maximumTargetHeightM) {
        return M4tNavigationCore_Fail(error, errorSize, "target height is outside 5-30m");
    }
    if (error != NULL && errorSize > 0) {
        error[0] = '\0';
    }
    return true;
}

bool M4tNavigationCore_UpdateArrival(T_M4tNavigationArrivalTracker *tracker,
                                    bool psdkMissionIdle,
                                    const T_M4tNavigationAircraftState *state,
                                    const T_M4tNavigationTarget *target,
                                    const T_M4tNavigationSafetyLimits *limits)
{
    double horizontalError;
    double verticalError;

    if (tracker == NULL || state == NULL || target == NULL || limits == NULL ||
        !psdkMissionIdle || !state->positionValid) {
        if (tracker != NULL) {
            tracker->consecutiveSamples = 0;
        }
        return false;
    }
    horizontalError = M4tNavigationCore_HorizontalDistanceM(
        state->latitudeDeg, state->longitudeDeg, target->latitudeDeg, target->longitudeDeg);
    verticalError = fabs(state->altitudeEllipsoidM - target->altitudeEllipsoidM);
    if (horizontalError <= limits->arrivalHorizontalToleranceM &&
        verticalError <= limits->arrivalVerticalToleranceM) {
        tracker->consecutiveSamples++;
    } else {
        tracker->consecutiveSamples = 0;
    }
    return tracker->consecutiveSamples >= limits->arrivalConsecutiveSamples;
}
