#ifndef M4T_NAVIGATION_H
#define M4T_NAVIGATION_H

#include "m4t_navigation_core.h"

#include <dji_flight_controller.h>
#include <dji_typedef.h>
#include <utils/cJSON.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M4T_NAVIGATION_MAX_ERROR 512

typedef struct {
    bool navigationEnabled;
    bool coordinateUnitsVerified;
    char expectedAircraftSn[64];
    char stateFilePath[512];
    unsigned int ecsLossRthSeconds;
} T_M4tNavigationConfig;

typedef struct {
    T_DjiReturnCode (*flightControllerInit)(T_DjiFlightControllerRidInfo ridInfo);
    T_DjiReturnCode (*getGeneralInfo)(T_DjiFlightControllerGeneralInfo *generalInfo);
    T_DjiReturnCode (*registerMissionCallback)(FcCmderModeOpenMisEventCbFunc callback);
    T_DjiReturnCode (*registerTrajectoryCallback)(FcCmderModeCoreTrajEventCbFunc callback);
    T_DjiReturnCode (*setModeStartMission)(T_DjiFlightControllerStartMissionReq request,
                                           T_DjiFlightControllerStartMissionRsp *response);
    T_DjiReturnCode (*startGoHome)(void);
    T_DjiReturnCode (*getEscStatus)(E_DjiFlightControllerElectronicSpeedControllerStatus *status);
    T_DjiReturnCode (*getGoHomeAltitude)(E_DjiFlightControllerGoHomeAltitude *altitude);
    T_DjiReturnCode (*getHorizontalVisualAvoidance)(
        E_DjiFlightControllerObstacleAvoidanceEnableStatus *status);
    T_DjiReturnCode (*getUpwardVisualAvoidance)(
        E_DjiFlightControllerObstacleAvoidanceEnableStatus *status);
    T_DjiReturnCode (*getDownwardVisualAvoidance)(
        E_DjiFlightControllerObstacleAvoidanceEnableStatus *status);
    T_DjiReturnCode (*getExitReason)(uint16_t *reason);
} T_M4tNavigationAdapter;

typedef enum {
    M4T_NAVIGATION_TERMINAL_COMPLETED = 0,
    M4T_NAVIGATION_TERMINAL_FAILED = 1,
    M4T_NAVIGATION_TERMINAL_CANCELLED = 2,
} E_M4tNavigationTerminal;

typedef struct {
    E_M4tNavigationTerminal terminal;
    cJSON *result;
    char error[M4T_NAVIGATION_MAX_ERROR];
} T_M4tNavigationOutcome;

typedef void (*M4tNavigationProgressCallback)(cJSON *progress, void *userData);

T_DjiReturnCode M4tNavigation_Init(const T_M4tNavigationConfig *config,
                                   const T_M4tNavigationAdapter *adapter);
void M4tNavigation_ReportEcsContact(bool successful);
T_M4tNavigationOutcome M4tNavigation_ExecuteStartup(cJSON *command);
T_M4tNavigationOutcome M4tNavigation_ExecuteNavigate(const char *commandId, cJSON *command,
                                                     M4tNavigationProgressCallback callback,
                                                     void *userData);
T_M4tNavigationOutcome M4tNavigation_ExecuteCancel(const char *navigationCommandId);
T_M4tNavigationOutcome M4tNavigation_ExecuteReturnHome(void);
bool M4tNavigation_IsActive(void);
const char *M4tNavigation_ReturnCodeName(T_DjiReturnCode code);

#ifdef __cplusplus
}
#endif

#endif
