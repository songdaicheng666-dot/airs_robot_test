#ifndef M4T_TELEMETRY_H
#define M4T_TELEMETRY_H

#include "m4t_navigation_core.h"

#include <dji_typedef.h>
#include <utils/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t sequence;
    char recordedAt[32];
    bool psdkConnected;
    bool flightValid;
    uint8_t flightStatus;
    uint8_t displayMode;
    bool positionValid;
    double latitudeDeg;
    double longitudeDeg;
    float altitudeEllipsoidM;
    uint16_t visibleSatellites;
    bool velocityValid;
    float velocityXMps;
    float velocityYMps;
    float velocityZMps;
    float horizontalSpeedMps;
    bool gpsValid;
    int gpsFixState;
    float horizontalAccuracyM;
    float verticalAccuracyM;
    uint16_t satellitesUsed;
    bool rtkValid;
    bool rtkConnected;
    uint8_t rtkPositionSolution;
    bool batteryValid;
    uint8_t batteryPercentage;
    float batteryVoltageV;
    float batteryCurrentA;
    bool homeSet;
    bool homeValid;
    double homeLatitudeDeg;
    double homeLongitudeDeg;
    float homeAltitudeEllipsoidM;
} T_M4tTelemetrySnapshot;

typedef struct {
    char aircraftModel[32];
    char aircraftSn[64];
    char sessionId[64];
    bool navigationEnabled;
    bool coordinateUnitsVerified;
    char recoveryAction[64];
    bool rthAltitudeValid;
    float rthAltitudeM;
    bool rthActive;
    bool obstacleStatusValid;
    bool horizontalVisualAvoidance;
    bool upwardVisualAvoidance;
    bool downwardVisualAvoidance;
    bool missionActive;
    char missionCommandId[64];
    char missionPhase[32];
    int missionCodeName;
    int missionState;
    float distanceRemainingM;
    float timeRemainingS;
} T_M4tTelemetryNavigationStatus;

T_DjiReturnCode M4tTelemetry_Start(void);
cJSON *M4tTelemetry_CreateJson(void);
void M4tTelemetry_GetSnapshot(T_M4tTelemetrySnapshot *snapshot);
void M4tTelemetry_GetAircraftState(T_M4tNavigationAircraftState *state);
void M4tTelemetry_SetNavigationStatus(const T_M4tTelemetryNavigationStatus *status);
void M4tTelemetry_GetNavigationStatus(T_M4tTelemetryNavigationStatus *status);

#ifdef __cplusplus
}
#endif

#endif
