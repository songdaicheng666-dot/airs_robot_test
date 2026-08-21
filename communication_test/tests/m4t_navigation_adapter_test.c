#include "m4t_navigation.h"
#include "m4t_telemetry.h"

#include <assert.h>
#include <dji_error.h>
#include <dji_fc_subscription.h>
#include <dji_logger.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static T_M4tTelemetrySnapshot s_snapshot;
static T_M4tTelemetryNavigationStatus s_status;
static FcCmderModeOpenMisEventCbFunc s_missionCallback;
static FcCmderModeCoreTrajEventCbFunc s_trajectoryCallback;
static int s_submissionMode;
static int s_newMissionCalls;
static int s_holdCalls;
static int s_rthCalls;
static int s_planningCalls;
static int s_velocityCalls;
static int s_heightCalls;
static T_DjiFlightControllerStartMissionReq s_lastRequest;

void DjiLogger_UserLogOutput(E_DjiLoggerConsoleLogLevel level, const char *format, ...)
{
    (void) level;
    (void) format;
}

T_DjiReturnCode M4tTelemetry_Start(void)
{
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

cJSON *M4tTelemetry_CreateJson(void)
{
    return cJSON_CreateObject();
}

void M4tTelemetry_GetSnapshot(T_M4tTelemetrySnapshot *snapshot)
{
    *snapshot = s_snapshot;
}

void M4tTelemetry_SetNavigationStatus(const T_M4tTelemetryNavigationStatus *status)
{
    s_status = *status;
}

void M4tTelemetry_GetNavigationStatus(T_M4tTelemetryNavigationStatus *status)
{
    *status = s_status;
}

void M4tTelemetry_GetAircraftState(T_M4tNavigationAircraftState *state)
{
    *state = (T_M4tNavigationAircraftState) {
        .psdkConnected = s_snapshot.psdkConnected,
        .flightValid = s_snapshot.flightValid,
        .positionValid = s_snapshot.positionValid,
        .gpsValid = s_snapshot.gpsValid,
        .batteryValid = s_snapshot.batteryValid,
        .velocityValid = s_snapshot.velocityValid,
        .homeSet = s_snapshot.homeSet && s_snapshot.homeValid,
        .obstacleAvoidanceEnabled = s_status.obstacleStatusValid &&
                                    s_status.horizontalVisualAvoidance &&
                                    s_status.upwardVisualAvoidance &&
                                    s_status.downwardVisualAvoidance,
        .rthAltitudeValid = s_status.rthAltitudeValid,
        .gpsFixState = s_snapshot.gpsFixState,
        .satellitesUsed = s_snapshot.satellitesUsed,
        .horizontalAccuracyM = s_snapshot.horizontalAccuracyM,
        .verticalAccuracyM = s_snapshot.verticalAccuracyM,
        .batteryPercentage = s_snapshot.batteryPercentage,
        .latitudeDeg = s_snapshot.latitudeDeg,
        .longitudeDeg = s_snapshot.longitudeDeg,
        .altitudeEllipsoidM = s_snapshot.altitudeEllipsoidM,
        .horizontalSpeedMps = s_snapshot.horizontalSpeedMps,
        .rthAltitudeM = s_status.rthAltitudeM,
    };
}

static T_DjiReturnCode FakeInit(T_DjiFlightControllerRidInfo ridInfo)
{
    assert(ridInfo.latitude == s_snapshot.latitudeDeg);
    assert(ridInfo.longitude == s_snapshot.longitudeDeg);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode FakeGeneralInfo(T_DjiFlightControllerGeneralInfo *info)
{
    snprintf(info->serialNum, sizeof(info->serialNum), "M4T-ADAPTER-TEST-SN");
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode FakeRegisterMission(FcCmderModeOpenMisEventCbFunc callback)
{
    s_missionCallback = callback;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode FakeRegisterTrajectory(FcCmderModeCoreTrajEventCbFunc callback)
{
    s_trajectoryCallback = callback;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode FakeSetPlanningAlgo(uint8_t algo)
{
    assert(algo == 1);
    s_planningCalls++;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode FakeSetMaxVelocity(uint8_t value)
{
    assert(value == 1);
    s_velocityCalls++;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode FakeSetMinFlightHeight(float value)
{
    assert(value == 2.0f);
    s_heightCalls++;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode FakeStartMission(T_DjiFlightControllerStartMissionReq request,
                                       T_DjiFlightControllerStartMissionRsp *response)
{
    T_DjiFlightControllerOpenMis event = {0};
    s_lastRequest = request;
    if (request.operation == 1) {
        s_holdCalls++;
        response->ret_code = 0;
        return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    }
    s_newMissionCalls++;
    event.mission_state_machine = 1;
    event.distance_remaining = 10;
    event.time_remaining = 5;
    s_missionCallback(event);
    if (s_submissionMode == 1) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT;
    }
    response->ret_code = 0;
    response->code_name = 7;
    if (s_submissionMode == 0) {
        s_snapshot.latitudeDeg = request.cmd_mode_point_info[0].lat;
        s_snapshot.longitudeDeg = request.cmd_mode_point_info[0].lon;
        s_snapshot.altitudeEllipsoidM = request.cmd_mode_point_info[0].alt;
        event.mission_state_machine = 0;
        event.distance_remaining = 0;
        event.time_remaining = 0;
        s_missionCallback(event);
    }
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode FakeStartGoHome(void)
{
    s_rthCalls++;
    s_snapshot.flightStatus = DJI_FC_SUBSCRIPTION_FLIGHT_STATUS_ON_GROUND;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode FakeEsc(E_DjiFlightControllerElectronicSpeedControllerStatus *status)
{
    *status = DJI_FLIGHT_CONTROLLER_NO_MOTOR_IN_SLOW_ROTATE_MODE;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode FakeRthAltitude(E_DjiFlightControllerGoHomeAltitude *altitude)
{
    *altitude = 50;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode FakeAvoidance(E_DjiFlightControllerObstacleAvoidanceEnableStatus *status)
{
    *status = DJI_FLIGHT_CONTROLLER_ENABLE_OBSTACLE_AVOIDANCE;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode FakeExitReason(uint16_t *reason)
{
    *reason = 42;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

/* Default adapter symbols are referenced by m4t_navigation.c even when a fake adapter is injected. */
T_DjiReturnCode DjiFlightController_Init(T_DjiFlightControllerRidInfo value) { return FakeInit(value); }
T_DjiReturnCode DjiFlightController_GetGeneralInfo(T_DjiFlightControllerGeneralInfo *value) { return FakeGeneralInfo(value); }
T_DjiReturnCode DjiFlightController_RegisterOpenMisInfoCallBack(FcCmderModeOpenMisEventCbFunc value) { return FakeRegisterMission(value); }
T_DjiReturnCode DjiFlightController_RegisterCoreTrajCallBack(FcCmderModeCoreTrajEventCbFunc value) { return FakeRegisterTrajectory(value); }
T_DjiReturnCode DjiFlightController_SetPlanningAlgo(uint8_t value) { return FakeSetPlanningAlgo(value); }
T_DjiReturnCode DjiFlightController_SetMaxVelocity(uint8_t value) { return FakeSetMaxVelocity(value); }
T_DjiReturnCode DjiFlightController_SetMinFlightHeight(float value) { return FakeSetMinFlightHeight(value); }
T_DjiReturnCode DjiFlightController_SetModeStartMission(T_DjiFlightControllerStartMissionReq value, T_DjiFlightControllerStartMissionRsp *response) { return FakeStartMission(value, response); }
T_DjiReturnCode DjiFlightController_StartGoHome(void) { return FakeStartGoHome(); }
T_DjiReturnCode DjiFlightController_GetElectronicSpeedControllerStatus(E_DjiFlightControllerElectronicSpeedControllerStatus *value) { return FakeEsc(value); }
T_DjiReturnCode DjiFlightController_GetGoHomeAltitude(E_DjiFlightControllerGoHomeAltitude *value) { return FakeRthAltitude(value); }
T_DjiReturnCode DjiFlightController_GetHorizontalVisualObstacleAvoidanceEnableStatus(E_DjiFlightControllerObstacleAvoidanceEnableStatus *value) { return FakeAvoidance(value); }
T_DjiReturnCode DjiFlightController_GetUpwardsVisualObstacleAvoidanceEnableStatus(E_DjiFlightControllerObstacleAvoidanceEnableStatus *value) { return FakeAvoidance(value); }
T_DjiReturnCode DjiFlightController_GetDownwardsVisualObstacleAvoidanceEnableStatus(E_DjiFlightControllerObstacleAvoidanceEnableStatus *value) { return FakeAvoidance(value); }
T_DjiReturnCode DjiFlightController_GetExitReason(uint16_t *value) { return FakeExitReason(value); }

static cJSON *Command(const char *payload)
{
    char text[1024];
    snprintf(text, sizeof(text),
             "{\"context\":{\"session_id\":\"%s\",\"aircraft_sn\":\"M4T-ADAPTER-TEST-SN\"},"
             "\"payload\":%s}", s_status.sessionId, payload);
    return cJSON_Parse(text);
}

static void CompleteStartup(void)
{
    cJSON *command = Command("{}");
    T_M4tNavigationOutcome outcome = M4tNavigation_ExecuteStartup(command);
    cJSON *originalHome;
    assert(outcome.terminal == M4T_NAVIGATION_TERMINAL_COMPLETED);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(outcome.result, "status")->valuestring, "ready") == 0);
    originalHome = cJSON_GetObjectItemCaseSensitive(outcome.result, "original_home");
    assert(cJSON_GetObjectItemCaseSensitive(originalHome, "altitude_ellipsoid_m")->valuedouble ==
           s_snapshot.altitudeEllipsoidM);
    cJSON_Delete(outcome.result);
    cJSON_Delete(command);
}

typedef struct {
    cJSON *command;
    T_M4tNavigationOutcome outcome;
} T_NavigationThread;

static void *RunNavigation(void *argument)
{
    T_NavigationThread *thread = argument;
    thread->outcome = M4tNavigation_ExecuteNavigate("cancel-command", thread->command, NULL, NULL);
    return NULL;
}

int main(int argc, char **argv)
{
    T_M4tNavigationConfig config = {0};
    T_M4tNavigationAdapter adapter = {
        .flightControllerInit = FakeInit,
        .getGeneralInfo = FakeGeneralInfo,
        .registerMissionCallback = FakeRegisterMission,
        .registerTrajectoryCallback = FakeRegisterTrajectory,
        .setPlanningAlgo = FakeSetPlanningAlgo,
        .setMaxVelocity = FakeSetMaxVelocity,
        .setMinFlightHeight = FakeSetMinFlightHeight,
        .setModeStartMission = FakeStartMission,
        .startGoHome = FakeStartGoHome,
        .getEscStatus = FakeEsc,
        .getGoHomeAltitude = FakeRthAltitude,
        .getHorizontalVisualAvoidance = FakeAvoidance,
        .getUpwardVisualAvoidance = FakeAvoidance,
        .getDownwardVisualAvoidance = FakeAvoidance,
        .getExitReason = FakeExitReason,
    };
    cJSON *command;
    T_M4tNavigationOutcome outcome;
    T_NavigationThread navigationThread = {0};
    pthread_t worker;

    assert(argc == 2);
    s_snapshot = (T_M4tTelemetrySnapshot) {
        .psdkConnected = true,
        .flightValid = true,
        .flightStatus = DJI_FC_SUBSCRIPTION_FLIGHT_STATUS_ON_GROUND,
        .positionValid = true,
        .latitudeDeg = 22.5,
        .longitudeDeg = 113.9,
        .altitudeEllipsoidM = 42,
        .velocityValid = true,
        .gpsValid = true,
        .gpsFixState = 3,
        .horizontalAccuracyM = 0.5,
        .verticalAccuracyM = 0.8,
        .satellitesUsed = 15,
        .batteryValid = true,
        .batteryPercentage = 80,
        .homeSet = true,
        .homeValid = true,
        .homeLatitudeDeg = 22.5,
        .homeLongitudeDeg = 113.9,
        .homeAltitudeBarometricM = 204,
    };
    config.navigationEnabled = true;
    config.coordinateUnitsVerified = true;
    config.ecsLossRthSeconds = 20;
    snprintf(config.expectedAircraftSn, sizeof(config.expectedAircraftSn), "M4T-ADAPTER-TEST-SN");
    snprintf(config.stateFilePath, sizeof(config.stateFilePath), "%s", argv[1]);
    assert(M4tNavigation_Init(&config, &adapter) == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS);
    assert(s_missionCallback != NULL && s_trajectoryCallback != NULL);

    CompleteStartup();
    assert(s_planningCalls == 1);
    assert(s_velocityCalls == 1);
    assert(s_heightCalls == 1);
    command = Command("{\"target\":{\"latitude_deg\":22.5001,\"longitude_deg\":113.9001,\"altitude_ellipsoid_m\":52}}");
    s_submissionMode = 0;
    outcome = M4tNavigation_ExecuteNavigate("arrival-command", command, NULL, NULL);
    assert(outcome.terminal == M4T_NAVIGATION_TERMINAL_COMPLETED);
    assert(s_newMissionCalls == 1);
    assert(s_lastRequest.version == 1);
    assert(s_lastRequest.operation == 0);
    assert(s_lastRequest.mea == 2.0f);
    assert(s_lastRequest.fly_vel == 1);
    assert(s_lastRequest.cmd_mode_point_info[0].lat == 22.5001);
    assert(s_lastRequest.cmd_mode_point_info[0].lon == 113.9001);
    cJSON_Delete(outcome.result);
    cJSON_Delete(command);

    s_snapshot.latitudeDeg = s_snapshot.homeLatitudeDeg;
    s_snapshot.longitudeDeg = s_snapshot.homeLongitudeDeg;
    s_snapshot.altitudeEllipsoidM = 42;
    CompleteStartup();
    command = Command("{\"target\":{\"latitude_deg\":22.5001,\"longitude_deg\":113.9001,\"altitude_ellipsoid_m\":52}}");
    s_submissionMode = 1;
    outcome = M4tNavigation_ExecuteNavigate("failed-command", command, NULL, NULL);
    assert(outcome.terminal == M4T_NAVIGATION_TERMINAL_FAILED);
    assert(s_holdCalls == 1);
    assert(s_lastRequest.version == 1);
    assert(s_lastRequest.operation == 1);
    cJSON_Delete(outcome.result);
    cJSON_Delete(command);

    CompleteStartup();
    s_submissionMode = 2;
    s_snapshot.flightStatus = DJI_FC_SUBSCRIPTION_FLIGHT_STATUS_IN_AIR;
    navigationThread.command = Command("{\"target\":{\"latitude_deg\":22.5001,\"longitude_deg\":113.9001,\"altitude_ellipsoid_m\":52}}");
    assert(pthread_create(&worker, NULL, RunNavigation, &navigationThread) == 0);
    sleep(1);
    outcome = M4tNavigation_ExecuteCancel("cancel-command");
    assert(outcome.terminal == M4T_NAVIGATION_TERMINAL_COMPLETED);
    cJSON_Delete(outcome.result);
    pthread_join(worker, NULL);
    assert(navigationThread.outcome.terminal == M4T_NAVIGATION_TERMINAL_CANCELLED);
    assert(s_rthCalls == 1);
    cJSON_Delete(navigationThread.outcome.result);
    cJSON_Delete(navigationThread.command);

    s_snapshot.flightStatus = DJI_FC_SUBSCRIPTION_FLIGHT_STATUS_ON_GROUND;
    s_snapshot.latitudeDeg = s_snapshot.homeLatitudeDeg;
    s_snapshot.longitudeDeg = s_snapshot.homeLongitudeDeg;
    s_snapshot.altitudeEllipsoidM = 42;
    s_snapshot.controlAuthorityValid = false;
    CompleteStartup();
    s_submissionMode = 2;
    navigationThread = (T_NavigationThread) {0};
    navigationThread.command = Command("{\"target\":{\"latitude_deg\":22.5001,\"longitude_deg\":113.9001,\"altitude_ellipsoid_m\":52}}");
    assert(pthread_create(&worker, NULL, RunNavigation, &navigationThread) == 0);
    sleep(1);
    s_snapshot.controlAuthorityValid = true;
    s_snapshot.controlAuthority = DJI_FC_SUBSCRIPTION_CONTROL_AUTHORITY_RC;
    s_snapshot.controlAuthorityChangeReason = DJI_FC_SUBSCRIPTION_AUTHORITY_CHANGE_REASON_RC_PAUSE_STOP;
    pthread_join(worker, NULL);
    assert(navigationThread.outcome.terminal == M4T_NAVIGATION_TERMINAL_CANCELLED);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(navigationThread.outcome.result, "status")->valuestring,
                  "pilot_takeover") == 0);
    assert(s_holdCalls == 1);
    assert(s_rthCalls == 1);
    cJSON_Delete(navigationThread.outcome.result);
    cJSON_Delete(navigationThread.command);

    assert(access(argv[1], F_OK) == 0);
    puts("m4t navigation adapter tests passed");
    return 0;
}
