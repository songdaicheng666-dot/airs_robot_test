#include "m4t_navigation_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static T_M4tNavigationAircraftState SafeState(void)
{
    return (T_M4tNavigationAircraftState) {
        .psdkConnected = true,
        .flightValid = true,
        .positionValid = true,
        .gpsValid = true,
        .batteryValid = true,
        .velocityValid = true,
        .homeSet = true,
        .obstacleAvoidanceEnabled = true,
        .rthAltitudeValid = true,
        .gpsFixState = 3,
        .satellitesUsed = 15,
        .horizontalAccuracyM = 0.5,
        .verticalAccuracyM = 0.8,
        .batteryPercentage = 80,
        .latitudeDeg = 22.5,
        .longitudeDeg = 113.9,
        .altitudeEllipsoidM = 52.0,
        .horizontalSpeedMps = 0.0,
        .rthAltitudeM = 50.0,
    };
}

int main(void)
{
    T_M4tNavigationSafetyLimits limits;
    T_M4tNavigationAircraftState state = SafeState();
    T_M4tNavigationTarget home = {22.5, 113.9, 42.0};
    T_M4tNavigationTarget target = {22.5001, 113.9001, 52.0};
    T_M4tNavigationArrivalTracker tracker = {0};
    char error[128];

    M4tNavigationCore_DefaultLimits(&limits);
    assert(limits.minimumRouteAltitudeM == 2.0);
    assert(limits.maximumHorizontalSpeedMps == 1);
    assert(limits.minimumTargetHeightM == 2.0);
    assert(limits.minimumBatteryPercentage == 10);
    assert(M4tNavigationCore_ValidatePreflight(&state, &limits, error, sizeof(error)));
    state.batteryPercentage = 9;
    assert(!M4tNavigationCore_ValidatePreflight(&state, &limits, error, sizeof(error)));
    assert(strstr(error, "battery") != NULL);
    state = SafeState();

    assert(M4tNavigationCore_ValidateTarget(&target, &home, &limits, error, sizeof(error)));
    target.altitudeEllipsoidM = 43.9;
    assert(!M4tNavigationCore_ValidateTarget(&target, &home, &limits, error, sizeof(error)));
    assert(strstr(error, "2-30m") != NULL);
    target.altitudeEllipsoidM = 44.0;
    assert(M4tNavigationCore_ValidateTarget(&target, &home, &limits, error, sizeof(error)));
    target.latitudeDeg = 22.502;
    assert(!M4tNavigationCore_ValidateTarget(&target, &home, &limits, error, sizeof(error)));
    target = (T_M4tNavigationTarget) {22.5001, 113.9001, 73.0};
    assert(!M4tNavigationCore_ValidateTarget(&target, &home, &limits, error, sizeof(error)));

    target = (T_M4tNavigationTarget) {22.5, 113.9, 52.0};
    assert(!M4tNavigationCore_UpdateArrival(&tracker, true, &state, &target, &limits));
    assert(!M4tNavigationCore_UpdateArrival(&tracker, true, &state, &target, &limits));
    assert(M4tNavigationCore_UpdateArrival(&tracker, true, &state, &target, &limits));
    assert(!M4tNavigationCore_UpdateArrival(&tracker, false, &state, &target, &limits));
    puts("m4t navigation core tests passed");
    return 0;
}
