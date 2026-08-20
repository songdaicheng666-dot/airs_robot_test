#include "m4t_navigation.h"

#include "m4t_telemetry.h"

#include <dji_error.h>
#include <dji_fc_subscription.h>
#include <dji_logger.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#define M4T_NAVIGATION_READY_SECONDS 300
#define M4T_NAVIGATION_MISSION_TIMEOUT_SECONDS 1800
#define M4T_NAVIGATION_RTH_TIMEOUT_SECONDS 600
#define M4T_NAVIGATION_STATE_VERSION 1

typedef struct {
    int version;
    char sessionId[64];
    bool ready;
    time_t readyUntil;
    bool readyConsumed;
    bool homeValid;
    T_M4tNavigationTarget originalHome;
    bool active;
    bool terminal;
    char activeCommandId[64];
    char phase[32];
    char submissionState[32];
    int codeName;
    T_M4tNavigationTarget target;
    char safetyAction[64];
} T_M4tNavigationJournal;

typedef struct {
    uint8_t state;
    float distanceRemainingM;
    float timeRemainingS;
    bool received;
    bool seenActive;
} T_M4tMissionCallbackState;

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static T_M4tNavigationConfig s_config;
static T_M4tNavigationSafetyLimits s_limits;
static T_M4tNavigationAdapter s_adapter;
static T_M4tNavigationJournal s_journal;
static T_M4tMissionCallbackState s_mission;
static bool s_initialized;
static bool s_controllerReady;
static bool s_callbacksReady;
static bool s_stateWritable;
static bool s_stateIntegrityValid = true;
static bool s_cancelRequested;
static bool s_safetyRth;
static bool s_rthStarted;
static bool s_recoveryPending;
static bool s_recoveryRthInProgress;
static T_DjiReturnCode s_controllerInitCode = DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
static T_DjiReturnCode s_callbackInitCode = DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
static time_t s_lastEcsContact;
static pthread_t s_monitorThread;

static T_DjiReturnCode M4tNavigation_DefaultFlightControllerInit(T_DjiFlightControllerRidInfo value)
{
    return DjiFlightController_Init(value);
}

static T_DjiReturnCode M4tNavigation_DefaultGetGeneralInfo(T_DjiFlightControllerGeneralInfo *value)
{
    return DjiFlightController_GetGeneralInfo(value);
}

static T_DjiReturnCode M4tNavigation_DefaultRegisterMission(FcCmderModeOpenMisEventCbFunc callback)
{
    return DjiFlightController_RegisterOpenMisInfoCallBack(callback);
}

static T_DjiReturnCode M4tNavigation_DefaultRegisterTrajectory(FcCmderModeCoreTrajEventCbFunc callback)
{
    return DjiFlightController_RegisterCoreTrajCallBack(callback);
}

static T_DjiReturnCode M4tNavigation_DefaultStartMission(T_DjiFlightControllerStartMissionReq request,
                                                        T_DjiFlightControllerStartMissionRsp *response)
{
    return DjiFlightController_SetModeStartMission(request, response);
}

static T_DjiReturnCode M4tNavigation_DefaultStartGoHome(void)
{
    return DjiFlightController_StartGoHome();
}

static T_DjiReturnCode M4tNavigation_DefaultGetEsc(
    E_DjiFlightControllerElectronicSpeedControllerStatus *status)
{
    return DjiFlightController_GetElectronicSpeedControllerStatus(status);
}

static T_DjiReturnCode M4tNavigation_DefaultGetRth(E_DjiFlightControllerGoHomeAltitude *altitude)
{
    return DjiFlightController_GetGoHomeAltitude(altitude);
}

static T_DjiReturnCode M4tNavigation_DefaultGetHorizontalAvoidance(
    E_DjiFlightControllerObstacleAvoidanceEnableStatus *status)
{
    return DjiFlightController_GetHorizontalVisualObstacleAvoidanceEnableStatus(status);
}

static T_DjiReturnCode M4tNavigation_DefaultGetUpwardAvoidance(
    E_DjiFlightControllerObstacleAvoidanceEnableStatus *status)
{
    return DjiFlightController_GetUpwardsVisualObstacleAvoidanceEnableStatus(status);
}

static T_DjiReturnCode M4tNavigation_DefaultGetDownwardAvoidance(
    E_DjiFlightControllerObstacleAvoidanceEnableStatus *status)
{
    return DjiFlightController_GetDownwardsVisualObstacleAvoidanceEnableStatus(status);
}

static T_DjiReturnCode M4tNavigation_DefaultGetExitReason(uint16_t *reason)
{
    return DjiFlightController_GetExitReason(reason);
}

static void M4tNavigation_LoadDefaultAdapter(T_M4tNavigationAdapter *adapter)
{
    *adapter = (T_M4tNavigationAdapter) {
        .flightControllerInit = M4tNavigation_DefaultFlightControllerInit,
        .getGeneralInfo = M4tNavigation_DefaultGetGeneralInfo,
        .registerMissionCallback = M4tNavigation_DefaultRegisterMission,
        .registerTrajectoryCallback = M4tNavigation_DefaultRegisterTrajectory,
        .setModeStartMission = M4tNavigation_DefaultStartMission,
        .startGoHome = M4tNavigation_DefaultStartGoHome,
        .getEscStatus = M4tNavigation_DefaultGetEsc,
        .getGoHomeAltitude = M4tNavigation_DefaultGetRth,
        .getHorizontalVisualAvoidance = M4tNavigation_DefaultGetHorizontalAvoidance,
        .getUpwardVisualAvoidance = M4tNavigation_DefaultGetUpwardAvoidance,
        .getDownwardVisualAvoidance = M4tNavigation_DefaultGetDownwardAvoidance,
        .getExitReason = M4tNavigation_DefaultGetExitReason,
    };
}

const char *M4tNavigation_ReturnCodeName(T_DjiReturnCode code)
{
    if (code == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        return "SUCCESS";
    }
    if (code == DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT) {
        return "NONSUPPORT";
    }
    if (code == DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT_IN_CURRENT_STATE) {
        return "NONSUPPORT_IN_CURRENT_STATE";
    }
    if (code == DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT) {
        return "TIMEOUT";
    }
    if (code == DJI_ERROR_SYSTEM_MODULE_CODE_INSUFFICIENT_ELECTRICITY) {
        return "INSUFFICIENT_ELECTRICITY";
    }
    return "PSDK_ERROR";
}

static void M4tNavigation_SetError(T_M4tNavigationOutcome *outcome, const char *format, ...)
{
    va_list arguments;

    outcome->terminal = M4T_NAVIGATION_TERMINAL_FAILED;
    va_start(arguments, format);
    vsnprintf(outcome->error, sizeof(outcome->error), format, arguments);
    va_end(arguments);
}

static T_M4tNavigationOutcome M4tNavigation_NewOutcome(void)
{
    T_M4tNavigationOutcome outcome = {0};
    outcome.terminal = M4T_NAVIGATION_TERMINAL_FAILED;
    return outcome;
}

static void M4tNavigation_CopyString(char *destination, size_t size, const char *source)
{
    if (size == 0) {
        return;
    }
    snprintf(destination, size, "%s", source != NULL ? source : "");
}

static void M4tNavigation_GenerateSessionId(char *buffer, size_t bufferSize)
{
    struct timespec now = {0};
    unsigned int randomPart = 0;
    int randomFile = open("/dev/urandom", O_RDONLY);
    ssize_t randomBytes = -1;

    clock_gettime(CLOCK_REALTIME, &now);
    if (randomFile >= 0) {
        do {
            randomBytes = read(randomFile, &randomPart, sizeof(randomPart));
        } while (randomBytes < 0 && errno == EINTR);
        close(randomFile);
    }
    if (randomBytes != (ssize_t) sizeof(randomPart)) {
        randomPart = (unsigned int) now.tv_nsec ^ (unsigned int) getpid();
    }
    snprintf(buffer, bufferSize, "%08lx-%08lx-%08x", (unsigned long) now.tv_sec,
             (unsigned long) now.tv_nsec, randomPart);
}

static bool M4tNavigation_FsyncParent(const char *path)
{
    char directory[512];
    char *separator;
    int descriptor;
    bool success;

    M4tNavigation_CopyString(directory, sizeof(directory), path);
    separator = strrchr(directory, '/');
    if (separator == NULL) {
        M4tNavigation_CopyString(directory, sizeof(directory), ".");
    } else if (separator == directory) {
        separator[1] = '\0';
    } else {
        *separator = '\0';
    }
    descriptor = open(directory, O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) {
        return false;
    }
    success = fsync(descriptor) == 0;
    close(descriptor);
    return success;
}

static cJSON *M4tNavigation_JournalJson(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *home = cJSON_CreateObject();
    cJSON *target = cJSON_CreateObject();

    if (root == NULL || home == NULL || target == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(home);
        cJSON_Delete(target);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "version", s_journal.version);
    cJSON_AddStringToObject(root, "session_id", s_journal.sessionId);
    cJSON_AddBoolToObject(root, "ready", s_journal.ready);
    cJSON_AddNumberToObject(root, "ready_until", (double) s_journal.readyUntil);
    cJSON_AddBoolToObject(root, "ready_consumed", s_journal.readyConsumed);
    cJSON_AddBoolToObject(root, "home_valid", s_journal.homeValid);
    cJSON_AddNumberToObject(home, "latitude_deg", s_journal.originalHome.latitudeDeg);
    cJSON_AddNumberToObject(home, "longitude_deg", s_journal.originalHome.longitudeDeg);
    cJSON_AddNumberToObject(home, "altitude_ellipsoid_m", s_journal.originalHome.altitudeEllipsoidM);
    cJSON_AddItemToObject(root, "original_home", home);
    cJSON_AddBoolToObject(root, "active", s_journal.active);
    cJSON_AddBoolToObject(root, "terminal", s_journal.terminal);
    cJSON_AddStringToObject(root, "active_command_id", s_journal.activeCommandId);
    cJSON_AddStringToObject(root, "phase", s_journal.phase);
    cJSON_AddStringToObject(root, "submission_state", s_journal.submissionState);
    cJSON_AddNumberToObject(root, "code_name", s_journal.codeName);
    cJSON_AddNumberToObject(target, "latitude_deg", s_journal.target.latitudeDeg);
    cJSON_AddNumberToObject(target, "longitude_deg", s_journal.target.longitudeDeg);
    cJSON_AddNumberToObject(target, "altitude_ellipsoid_m", s_journal.target.altitudeEllipsoidM);
    cJSON_AddItemToObject(root, "target", target);
    cJSON_AddStringToObject(root, "safety_action", s_journal.safetyAction);
    return root;
}

static bool M4tNavigation_SaveJournalLocked(void)
{
    cJSON *root;
    char *json;
    char temporaryPath[640];
    int descriptor;
    size_t length;
    ssize_t written;
    bool success = false;
    bool closed = false;

    if (!s_config.navigationEnabled || s_config.stateFilePath[0] == '\0') {
        return false;
    }
    root = M4tNavigation_JournalJson();
    json = root != NULL ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (json == NULL || snprintf(temporaryPath, sizeof(temporaryPath), "%s.tmp.%ld",
                                 s_config.stateFilePath, (long) getpid()) >= (int) sizeof(temporaryPath)) {
        cJSON_free(json);
        return false;
    }
    descriptor = open(temporaryPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) {
        cJSON_free(json);
        return false;
    }
    length = strlen(json);
    written = write(descriptor, json, length);
    if (written == (ssize_t) length && write(descriptor, "\n", 1) == 1 && fsync(descriptor) == 0 &&
        close(descriptor) == 0) {
        closed = true;
        success = rename(temporaryPath, s_config.stateFilePath) == 0 &&
                  M4tNavigation_FsyncParent(s_config.stateFilePath);
    }
    if (!closed) {
        close(descriptor);
    }
    if (!success) {
        unlink(temporaryPath);
    }
    cJSON_free(json);
    return success;
}

static bool M4tNavigation_JsonNumber(cJSON *object, const char *name, double *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
        return false;
    }
    *value = item->valuedouble;
    return true;
}

static bool M4tNavigation_LoadJournal(void)
{
    FILE *file;
    long length;
    char *text;
    cJSON *root;
    cJSON *item;
    cJSON *home;
    cJSON *target;
    bool success = false;

    file = fopen(s_config.stateFilePath, "rb");
    if (file == NULL) {
        return errno == ENOENT;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 || length > 65536 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    text = calloc((size_t) length + 1, 1);
    if (text == NULL || fread(text, 1, (size_t) length, file) != (size_t) length) {
        free(text);
        fclose(file);
        return false;
    }
    fclose(file);
    root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(item) || item->valueint != M4T_NAVIGATION_STATE_VERSION) {
        goto done;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (!cJSON_IsString(item) || item->valuestring[0] == '\0') {
        goto done;
    }
    M4tNavigation_CopyString(s_journal.sessionId, sizeof(s_journal.sessionId), item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(root, "ready");
    s_journal.ready = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(root, "ready_until");
    s_journal.readyUntil = cJSON_IsNumber(item) ? (time_t) item->valuedouble : 0;
    s_journal.readyConsumed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ready_consumed"));
    s_journal.homeValid = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "home_valid"));
    home = cJSON_GetObjectItemCaseSensitive(root, "original_home");
    target = cJSON_GetObjectItemCaseSensitive(root, "target");
    if (!cJSON_IsObject(home) || !cJSON_IsObject(target) ||
        !M4tNavigation_JsonNumber(home, "latitude_deg", &s_journal.originalHome.latitudeDeg) ||
        !M4tNavigation_JsonNumber(home, "longitude_deg", &s_journal.originalHome.longitudeDeg) ||
        !M4tNavigation_JsonNumber(home, "altitude_ellipsoid_m", &s_journal.originalHome.altitudeEllipsoidM) ||
        !M4tNavigation_JsonNumber(target, "latitude_deg", &s_journal.target.latitudeDeg) ||
        !M4tNavigation_JsonNumber(target, "longitude_deg", &s_journal.target.longitudeDeg) ||
        !M4tNavigation_JsonNumber(target, "altitude_ellipsoid_m", &s_journal.target.altitudeEllipsoidM)) {
        goto done;
    }
    s_journal.active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "active"));
    s_journal.terminal = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "terminal"));
    item = cJSON_GetObjectItemCaseSensitive(root, "active_command_id");
    M4tNavigation_CopyString(s_journal.activeCommandId, sizeof(s_journal.activeCommandId),
                             cJSON_IsString(item) ? item->valuestring : "");
    item = cJSON_GetObjectItemCaseSensitive(root, "phase");
    M4tNavigation_CopyString(s_journal.phase, sizeof(s_journal.phase),
                             cJSON_IsString(item) ? item->valuestring : "idle");
    item = cJSON_GetObjectItemCaseSensitive(root, "submission_state");
    M4tNavigation_CopyString(s_journal.submissionState, sizeof(s_journal.submissionState),
                             cJSON_IsString(item) ? item->valuestring : "none");
    item = cJSON_GetObjectItemCaseSensitive(root, "code_name");
    s_journal.codeName = cJSON_IsNumber(item) ? item->valueint : -1;
    item = cJSON_GetObjectItemCaseSensitive(root, "safety_action");
    M4tNavigation_CopyString(s_journal.safetyAction, sizeof(s_journal.safetyAction),
                             cJSON_IsString(item) ? item->valuestring : "");
    success = true;
done:
    cJSON_Delete(root);
    return success;
}

static T_DjiReturnCode M4tNavigation_MissionCallback(T_DjiFlightControllerOpenMis eventData)
{
    pthread_mutex_lock(&s_mutex);
    s_mission.state = eventData.mission_state_machine;
    s_mission.distanceRemainingM = eventData.distance_remaining;
    s_mission.timeRemainingS = eventData.time_remaining;
    s_mission.received = true;
    if (eventData.mission_state_machine != 0) {
        s_mission.seenActive = true;
    }
    pthread_mutex_unlock(&s_mutex);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode M4tNavigation_TrajectoryCallback(T_DjiFlightControllerCoreTraj eventData)
{
    pthread_mutex_lock(&s_mutex);
    if (s_journal.active && s_journal.codeName < 0) {
        s_journal.codeName = eventData.code_name;
    }
    pthread_mutex_unlock(&s_mutex);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static void M4tNavigation_UpdateTelemetrySettings(void)
{
    T_M4tTelemetryNavigationStatus status;
    E_DjiFlightControllerGoHomeAltitude rthAltitude = 0;
    E_DjiFlightControllerObstacleAvoidanceEnableStatus horizontal = 0;
    E_DjiFlightControllerObstacleAvoidanceEnableStatus upward = 0;
    E_DjiFlightControllerObstacleAvoidanceEnableStatus downward = 0;
    bool obstacleValid;

    M4tTelemetry_GetNavigationStatus(&status);
    if (s_controllerReady && s_adapter.getGoHomeAltitude(&rthAltitude) == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        status.rthAltitudeValid = true;
        status.rthAltitudeM = rthAltitude;
    } else {
        status.rthAltitudeValid = false;
    }
    obstacleValid = s_controllerReady &&
                    s_adapter.getHorizontalVisualAvoidance(&horizontal) == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS &&
                    s_adapter.getUpwardVisualAvoidance(&upward) == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS &&
                    s_adapter.getDownwardVisualAvoidance(&downward) == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    status.obstacleStatusValid = obstacleValid;
    if (obstacleValid) {
        status.horizontalVisualAvoidance = horizontal == DJI_FLIGHT_CONTROLLER_ENABLE_OBSTACLE_AVOIDANCE;
        status.upwardVisualAvoidance = upward == DJI_FLIGHT_CONTROLLER_ENABLE_OBSTACLE_AVOIDANCE;
        status.downwardVisualAvoidance = downward == DJI_FLIGHT_CONTROLLER_ENABLE_OBSTACLE_AVOIDANCE;
    }
    M4tTelemetry_SetNavigationStatus(&status);
}

static bool M4tNavigation_IsAirborne(const T_M4tTelemetrySnapshot *snapshot)
{
    return snapshot->flightValid && snapshot->flightStatus == DJI_FC_SUBSCRIPTION_FLIGHT_STATUS_IN_AIR;
}

static bool M4tNavigation_IsLandedAndStopped(void)
{
    T_M4tTelemetrySnapshot snapshot;
    E_DjiFlightControllerElectronicSpeedControllerStatus escStatus =
        DJI_FLIGHT_CONTROLLER_ALL_MOTOR_IN_SLOW_ROTATE_MODE;

    M4tTelemetry_GetSnapshot(&snapshot);
    if (!snapshot.flightValid || M4tNavigation_IsAirborne(&snapshot)) {
        return false;
    }
    return s_adapter.getEscStatus(&escStatus) == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS &&
           escStatus == DJI_FLIGHT_CONTROLLER_NO_MOTOR_IN_SLOW_ROTATE_MODE;
}

static bool M4tNavigation_StartRthLocked(const char *action, T_DjiReturnCode *code)
{
    T_M4tTelemetryNavigationStatus status;

    if (s_rthStarted) {
        *code = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
        return true;
    }
    *code = s_adapter.startGoHome();
    if (*code != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        return false;
    }
    s_rthStarted = true;
    M4tNavigation_CopyString(s_journal.safetyAction, sizeof(s_journal.safetyAction), action);
    M4tNavigation_CopyString(s_journal.phase, sizeof(s_journal.phase), "returning_home");
    (void) M4tNavigation_SaveJournalLocked();
    M4tTelemetry_GetNavigationStatus(&status);
    status.rthActive = true;
    M4tNavigation_CopyString(status.recoveryAction, sizeof(status.recoveryAction), action);
    M4tNavigation_CopyString(status.missionPhase, sizeof(status.missionPhase), "returning_home");
    M4tTelemetry_SetNavigationStatus(&status);
    return true;
}

static bool M4tNavigation_WaitForLanding(unsigned int timeoutSeconds)
{
    unsigned int elapsed;
    for (elapsed = 0; elapsed < timeoutSeconds; ++elapsed) {
        if (M4tNavigation_IsLandedAndStopped()) {
            return true;
        }
        sleep(1);
    }
    return false;
}

static void *M4tNavigation_MonitorTask(void *argument)
{
    (void) argument;
    while (true) {
        T_M4tTelemetrySnapshot snapshot;
        time_t now = time(NULL);
        T_DjiReturnCode code = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
        bool shouldRth = false;
        bool recoveryLanded = false;
        const char *action = NULL;

        M4tNavigation_UpdateTelemetrySettings();
        M4tTelemetry_GetSnapshot(&snapshot);
        pthread_mutex_lock(&s_mutex);
        if (M4tNavigation_IsAirborne(&snapshot) && s_recoveryPending) {
            shouldRth = true;
            action = "process_recovery_rth";
            s_safetyRth = true;
            s_recoveryPending = false;
            s_recoveryRthInProgress = true;
        } else if (M4tNavigation_IsAirborne(&snapshot) && s_config.ecsLossRthSeconds > 0 &&
                   now - s_lastEcsContact >= (time_t) s_config.ecsLossRthSeconds) {
            shouldRth = true;
            action = "ecs_link_loss_rth";
            s_safetyRth = true;
        } else if (!M4tNavigation_IsAirborne(&snapshot) && s_recoveryPending) {
            s_journal.active = false;
            s_journal.terminal = true;
            M4tNavigation_CopyString(s_journal.phase, sizeof(s_journal.phase), "recovered_on_ground");
            (void) M4tNavigation_SaveJournalLocked();
            s_recoveryPending = false;
        } else if (!M4tNavigation_IsAirborne(&snapshot) && s_recoveryRthInProgress &&
                   M4tNavigation_IsLandedAndStopped()) {
            s_journal.active = false;
            s_journal.terminal = true;
            M4tNavigation_CopyString(s_journal.phase, sizeof(s_journal.phase), "recovery_rth_landed");
            M4tNavigation_CopyString(s_journal.safetyAction, sizeof(s_journal.safetyAction),
                                     "process_recovery_rth");
            (void) M4tNavigation_SaveJournalLocked();
            s_recoveryRthInProgress = false;
            s_rthStarted = false;
            recoveryLanded = true;
        }
        if (shouldRth) {
            (void) M4tNavigation_StartRthLocked(action, &code);
        }
        pthread_mutex_unlock(&s_mutex);
        if (recoveryLanded) {
            T_M4tTelemetryNavigationStatus status;
            M4tTelemetry_GetNavigationStatus(&status);
            status.missionActive = false;
            status.rthActive = false;
            M4tNavigation_CopyString(status.missionPhase, sizeof(status.missionPhase),
                                     "recovery_rth_landed");
            M4tTelemetry_SetNavigationStatus(&status);
        }
        sleep(1);
    }
    return NULL;
}

static bool M4tNavigation_CommandBindingMatches(cJSON *command, char *error, size_t errorSize)
{
    cJSON *context = cJSON_GetObjectItemCaseSensitive(command, "context");
    cJSON *session = cJSON_GetObjectItemCaseSensitive(context, "session_id");
    cJSON *sn = cJSON_GetObjectItemCaseSensitive(context, "aircraft_sn");
    T_M4tTelemetryNavigationStatus status;

    M4tTelemetry_GetNavigationStatus(&status);
    if (!cJSON_IsObject(context) || !cJSON_IsString(session) || !cJSON_IsString(sn) ||
        strcmp(session->valuestring, status.sessionId) != 0 ||
        strcmp(sn->valuestring, status.aircraftSn) != 0) {
        snprintf(error, errorSize, "command identity/session binding does not match current aircraft");
        return false;
    }
    return true;
}

static cJSON *M4tNavigation_CreatePositionJson(const T_M4tNavigationAircraftState *state)
{
    cJSON *position = cJSON_CreateObject();
    if (position != NULL) {
        cJSON_AddNumberToObject(position, "latitude_deg", state->latitudeDeg);
        cJSON_AddNumberToObject(position, "longitude_deg", state->longitudeDeg);
        cJSON_AddNumberToObject(position, "altitude_ellipsoid_m", state->altitudeEllipsoidM);
    }
    return position;
}

static cJSON *M4tNavigation_CreateTargetJson(const T_M4tNavigationTarget *target)
{
    cJSON *value = cJSON_CreateObject();
    if (value != NULL) {
        cJSON_AddNumberToObject(value, "latitude_deg", target->latitudeDeg);
        cJSON_AddNumberToObject(value, "longitude_deg", target->longitudeDeg);
        cJSON_AddNumberToObject(value, "altitude_ellipsoid_m", target->altitudeEllipsoidM);
    }
    return value;
}

static void M4tNavigation_PublishProgress(const char *phase, T_DjiReturnCode psdkCode,
                                         M4tNavigationProgressCallback callback, void *userData)
{
    cJSON *progress;
    T_M4tNavigationAircraftState aircraft;
    T_M4tMissionCallbackState mission;

    if (callback == NULL) {
        return;
    }
    M4tTelemetry_GetAircraftState(&aircraft);
    pthread_mutex_lock(&s_mutex);
    mission = s_mission;
    pthread_mutex_unlock(&s_mutex);
    progress = cJSON_CreateObject();
    if (progress == NULL) {
        return;
    }
    cJSON_AddStringToObject(progress, "phase", phase);
    cJSON_AddNumberToObject(progress, "remaining_distance_m", mission.distanceRemainingM);
    cJSON_AddNumberToObject(progress, "remaining_time_s", mission.timeRemainingS);
    cJSON_AddNumberToObject(progress, "psdk_error_code", (double) psdkCode);
    cJSON_AddStringToObject(progress, "psdk_error_name", M4tNavigation_ReturnCodeName(psdkCode));
    cJSON_AddItemToObject(progress, "position", M4tNavigation_CreatePositionJson(&aircraft));
    callback(progress, userData);
    cJSON_Delete(progress);
}

static bool M4tNavigation_ParseTarget(cJSON *command, T_M4tNavigationTarget *target,
                                     char *error, size_t errorSize)
{
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(command, "payload");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(payload, "target");

    if (!cJSON_IsObject(payload) || cJSON_GetArraySize(payload) != 1 || !cJSON_IsObject(value) ||
        cJSON_GetArraySize(value) != 3 ||
        !M4tNavigation_JsonNumber(value, "latitude_deg", &target->latitudeDeg) ||
        !M4tNavigation_JsonNumber(value, "longitude_deg", &target->longitudeDeg) ||
        !M4tNavigation_JsonNumber(value, "altitude_ellipsoid_m", &target->altitudeEllipsoidM)) {
        snprintf(error, errorSize, "NAVIGATE requires only a geodetic target");
        return false;
    }
    return true;
}

static bool M4tNavigation_CheckStartupCapability(T_M4tNavigationOutcome *outcome)
{
    T_M4tTelemetryNavigationStatus status;

    if (!s_initialized || !s_config.navigationEnabled) {
        M4tNavigation_SetError(outcome, "M4T navigation is disabled by private configuration");
        return false;
    }
    if (!s_config.coordinateUnitsVerified) {
        M4tNavigation_SetError(outcome, "M4T coordinate units are not verified in DJI Assistant simulator");
        return false;
    }
    if (!s_stateWritable) {
        M4tNavigation_SetError(outcome, "M4T navigation state file is not atomically writable");
        return false;
    }
    if (!s_controllerReady) {
        M4tNavigation_SetError(outcome, "flight controller init failed: %s (0x%08llX)",
                               M4tNavigation_ReturnCodeName(s_controllerInitCode),
                               (unsigned long long) s_controllerInitCode);
        return false;
    }
    if (!s_callbacksReady) {
        M4tNavigation_SetError(outcome, "advanced Flight Control callback permission failed: %s (0x%08llX)",
                               M4tNavigation_ReturnCodeName(s_callbackInitCode),
                               (unsigned long long) s_callbackInitCode);
        return false;
    }
    M4tTelemetry_GetNavigationStatus(&status);
    if (s_config.expectedAircraftSn[0] == '\0' ||
        strcmp(status.aircraftSn, s_config.expectedAircraftSn) != 0) {
        M4tNavigation_SetError(outcome, "connected aircraft SN does not match private configuration");
        return false;
    }
    return true;
}

T_DjiReturnCode M4tNavigation_Init(const T_M4tNavigationConfig *config,
                                   const T_M4tNavigationAdapter *adapter)
{
    T_M4tTelemetrySnapshot snapshot;
    T_M4tTelemetryNavigationStatus status = {0};
    T_DjiFlightControllerRidInfo ridInfo = {0};
    T_DjiFlightControllerGeneralInfo generalInfo = {0};
    unsigned int waitSeconds;

    if (config == NULL) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    memset(&s_journal, 0, sizeof(s_journal));
    memset(&s_mission, 0, sizeof(s_mission));
    s_config = *config;
    if (s_config.ecsLossRthSeconds == 0) {
        s_config.ecsLossRthSeconds = 20;
    }
    M4tNavigationCore_DefaultLimits(&s_limits);
    M4tNavigation_LoadDefaultAdapter(&s_adapter);
    if (adapter != NULL) {
        s_adapter = *adapter;
    }
    s_journal.version = M4T_NAVIGATION_STATE_VERSION;
    s_journal.codeName = -1;
    M4tNavigation_CopyString(s_journal.phase, sizeof(s_journal.phase), "idle");
    M4tNavigation_CopyString(s_journal.submissionState, sizeof(s_journal.submissionState), "none");
    if (s_config.navigationEnabled && s_config.stateFilePath[0] != '\0') {
        if (!M4tNavigation_LoadJournal()) {
            s_stateIntegrityValid = false;
            USER_LOG_ERROR("M4T navigation state journal is invalid: %s", s_config.stateFilePath);
        }
    }
    if (s_journal.sessionId[0] == '\0') {
        M4tNavigation_GenerateSessionId(s_journal.sessionId, sizeof(s_journal.sessionId));
    }
    if (s_journal.active && !s_journal.terminal) {
        s_recoveryPending = true;
        s_journal.ready = false;
        s_journal.readyConsumed = true;
    }

    for (waitSeconds = 0; waitSeconds < 10; ++waitSeconds) {
        M4tTelemetry_GetSnapshot(&snapshot);
        if (snapshot.positionValid) {
            break;
        }
        sleep(1);
    }
    M4tTelemetry_GetSnapshot(&snapshot);
    if (snapshot.positionValid) {
        ridInfo.latitude = snapshot.latitudeDeg;
        ridInfo.longitude = snapshot.longitudeDeg;
        ridInfo.altitude = snapshot.altitudeEllipsoidM > 0 && snapshot.altitudeEllipsoidM < 65535
                               ? (uint16_t) snapshot.altitudeEllipsoidM
                               : 0;
        s_controllerInitCode = s_adapter.flightControllerInit(ridInfo);
        s_controllerReady = s_controllerInitCode == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    } else {
        s_controllerInitCode = DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT;
    }
    if (s_controllerReady && s_adapter.getGeneralInfo(&generalInfo) == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        M4tNavigation_CopyString(status.aircraftSn, sizeof(status.aircraftSn), generalInfo.serialNum);
    }
    M4tNavigation_CopyString(status.aircraftModel, sizeof(status.aircraftModel), "M4T");
    M4tNavigation_CopyString(status.sessionId, sizeof(status.sessionId), s_journal.sessionId);
    status.navigationEnabled = s_config.navigationEnabled;
    status.coordinateUnitsVerified = s_config.coordinateUnitsVerified;
    status.missionCodeName = -1;
    status.missionState = -1;
    status.distanceRemainingM = -1;
    status.timeRemainingS = -1;
    if (s_recoveryPending) {
        M4tNavigation_CopyString(status.recoveryAction, sizeof(status.recoveryAction), "recovery_pending");
    }
    M4tTelemetry_SetNavigationStatus(&status);

    if (s_controllerReady) {
        s_callbackInitCode = s_adapter.registerMissionCallback(M4tNavigation_MissionCallback);
        if (s_callbackInitCode == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            s_callbackInitCode = s_adapter.registerTrajectoryCallback(M4tNavigation_TrajectoryCallback);
        }
        s_callbacksReady = s_callbackInitCode == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    }
    M4tNavigation_UpdateTelemetrySettings();

    if (s_config.navigationEnabled && s_stateIntegrityValid) {
        pthread_mutex_lock(&s_mutex);
        s_stateWritable = M4tNavigation_SaveJournalLocked();
        pthread_mutex_unlock(&s_mutex);
    }
    s_lastEcsContact = time(NULL);
    s_initialized = true;
    if (pthread_create(&s_monitorThread, NULL, M4tNavigation_MonitorTask, NULL) != 0) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    pthread_detach(s_monitorThread);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

void M4tNavigation_ReportEcsContact(bool successful)
{
    if (successful) {
        pthread_mutex_lock(&s_mutex);
        s_lastEcsContact = time(NULL);
        pthread_mutex_unlock(&s_mutex);
    }
}

T_M4tNavigationOutcome M4tNavigation_ExecuteStartup(cJSON *command)
{
    T_M4tNavigationOutcome outcome = M4tNavigation_NewOutcome();
    T_M4tNavigationAircraftState aircraft;
    T_M4tTelemetrySnapshot snapshot;
    T_M4tTelemetryNavigationStatus status;
    char validationError[256];
    unsigned int stableSamples = 0;
    unsigned int sample;

    if (!M4tNavigation_CheckStartupCapability(&outcome) ||
        !M4tNavigation_CommandBindingMatches(command, outcome.error, sizeof(outcome.error))) {
        return outcome;
    }
    M4tNavigation_UpdateTelemetrySettings();
    M4tTelemetry_GetAircraftState(&aircraft);
    if (!M4tNavigationCore_ValidatePreflight(&aircraft, &s_limits,
                                            validationError, sizeof(validationError))) {
        M4tNavigation_SetError(&outcome, "preflight rejected: %s", validationError);
        return outcome;
    }
    M4tTelemetry_GetSnapshot(&snapshot);
    pthread_mutex_lock(&s_mutex);
    if (s_journal.active) {
        pthread_mutex_unlock(&s_mutex);
        M4tNavigation_SetError(&outcome, "a flight operation is already active");
        return outcome;
    }
    pthread_mutex_unlock(&s_mutex);

    if (M4tNavigation_IsAirborne(&snapshot)) {
        for (sample = 0; sample < 3; ++sample) {
            M4tTelemetry_GetAircraftState(&aircraft);
            if (aircraft.velocityValid && aircraft.horizontalSpeedMps <= s_limits.maximumHoverSpeedMps) {
                stableSamples++;
            } else {
                stableSamples = 0;
            }
            sleep(1);
        }
        if (stableSamples < 3) {
            M4tNavigation_SetError(&outcome, "airborne STARTUP requires 3 seconds of stable hover");
            return outcome;
        }
        pthread_mutex_lock(&s_mutex);
        if (!s_journal.homeValid) {
            pthread_mutex_unlock(&s_mutex);
            M4tNavigation_SetError(&outcome, "airborne STARTUP requires the persisted original Home session");
            return outcome;
        }
        pthread_mutex_unlock(&s_mutex);
    } else if (!snapshot.homeSet || !snapshot.homeValid) {
        M4tNavigation_SetError(&outcome, "ground STARTUP requires a valid aircraft Home point");
        return outcome;
    }

    pthread_mutex_lock(&s_mutex);
    if (!M4tNavigation_IsAirborne(&snapshot)) {
        s_journal.originalHome = (T_M4tNavigationTarget) {
            snapshot.homeLatitudeDeg,
            snapshot.homeLongitudeDeg,
            snapshot.homeAltitudeEllipsoidM,
        };
        s_journal.homeValid = true;
    }
    s_journal.ready = true;
    s_journal.readyConsumed = false;
    s_journal.readyUntil = time(NULL) + M4T_NAVIGATION_READY_SECONDS;
    s_journal.terminal = false;
    M4tNavigation_CopyString(s_journal.phase, sizeof(s_journal.phase), "ready");
    if (!M4tNavigation_SaveJournalLocked()) {
        s_journal.ready = false;
        pthread_mutex_unlock(&s_mutex);
        M4tNavigation_SetError(&outcome, "could not persist STARTUP ready state atomically");
        return outcome;
    }
    pthread_mutex_unlock(&s_mutex);

    M4tTelemetry_GetNavigationStatus(&status);
    outcome.result = cJSON_CreateObject();
    if (outcome.result == NULL) {
        M4tNavigation_SetError(&outcome, "could not allocate STARTUP result");
        return outcome;
    }
    cJSON_AddStringToObject(outcome.result, "status", "ready");
    cJSON_AddStringToObject(outcome.result, "session_id", status.sessionId);
    cJSON_AddStringToObject(outcome.result, "aircraft_sn", status.aircraftSn);
    cJSON_AddNumberToObject(outcome.result, "valid_for_seconds", M4T_NAVIGATION_READY_SECONDS);
    cJSON_AddItemToObject(outcome.result, "original_home",
                          M4tNavigation_CreateTargetJson(&s_journal.originalHome));
    outcome.terminal = M4T_NAVIGATION_TERMINAL_COMPLETED;
    return outcome;
}

static bool M4tNavigation_HoldOrRth(T_DjiReturnCode *holdCode, const char **action)
{
    T_M4tNavigationAircraftState aircraft;
    T_M4tMissionCallbackState mission;
    T_DjiFlightControllerStartMissionReq request = {0};
    T_DjiFlightControllerStartMissionRsp response = {0};
    unsigned int sample;
    unsigned int stable = 0;
    T_DjiReturnCode rthCode = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;

    pthread_mutex_lock(&s_mutex);
    mission = s_mission;
    pthread_mutex_unlock(&s_mutex);
    if (mission.state != 0) {
        M4tTelemetry_GetAircraftState(&aircraft);
        request.version = 0;
        request.operation = 1;
        request.mea = (float) s_limits.minimumRouteAltitudeM;
        request.fly_vel = s_limits.maximumHorizontalSpeedMps;
        request.goal_num = 1;
        request.cmd_mode_point_info[0].lat = aircraft.latitudeDeg;
        request.cmd_mode_point_info[0].lon = aircraft.longitudeDeg;
        request.cmd_mode_point_info[0].alt = (float) aircraft.altitudeEllipsoidM;
        *holdCode = s_adapter.setModeStartMission(request, &response);
        if (*holdCode == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS && response.ret_code == 0) {
            *action = "target_updated_to_current_position";
            return true;
        }
    } else {
        for (sample = 0; sample < 3; ++sample) {
            M4tTelemetry_GetAircraftState(&aircraft);
            stable = aircraft.velocityValid && aircraft.horizontalSpeedMps <= s_limits.maximumHoverSpeedMps
                         ? stable + 1
                         : 0;
            sleep(1);
        }
        if (stable >= 3) {
            *holdCode = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
            *action = "hover_verified";
            return true;
        }
    }
    pthread_mutex_lock(&s_mutex);
    if (!M4tNavigation_StartRthLocked("hold_failed_rth", &rthCode)) {
        pthread_mutex_unlock(&s_mutex);
        *action = "hold_and_rth_failed";
        return false;
    }
    pthread_mutex_unlock(&s_mutex);
    *action = "hold_failed_rth";
    (void) M4tNavigation_WaitForLanding(M4T_NAVIGATION_RTH_TIMEOUT_SECONDS);
    return false;
}

static void M4tNavigation_FinishJournal(const char *phase, const char *safetyAction)
{
    T_M4tTelemetryNavigationStatus status;

    pthread_mutex_lock(&s_mutex);
    s_journal.active = false;
    s_journal.terminal = true;
    M4tNavigation_CopyString(s_journal.phase, sizeof(s_journal.phase), phase);
    if (safetyAction != NULL) {
        M4tNavigation_CopyString(s_journal.safetyAction, sizeof(s_journal.safetyAction), safetyAction);
    }
    (void) M4tNavigation_SaveJournalLocked();
    pthread_mutex_unlock(&s_mutex);
    M4tTelemetry_GetNavigationStatus(&status);
    status.missionActive = false;
    status.rthActive = false;
    M4tNavigation_CopyString(status.missionPhase, sizeof(status.missionPhase), phase);
    M4tTelemetry_SetNavigationStatus(&status);
}

T_M4tNavigationOutcome M4tNavigation_ExecuteNavigate(const char *commandId, cJSON *command,
                                                     M4tNavigationProgressCallback callback,
                                                     void *userData)
{
    T_M4tNavigationOutcome outcome = M4tNavigation_NewOutcome();
    T_M4tNavigationTarget target = {0};
    T_M4tNavigationAircraftState aircraft;
    T_M4tTelemetrySnapshot snapshot;
    T_M4tTelemetryNavigationStatus telemetryStatus;
    T_DjiFlightControllerStartMissionReq request = {0};
    T_DjiFlightControllerStartMissionRsp response = {0};
    T_DjiReturnCode psdkCode = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    T_M4tNavigationArrivalTracker arrival = {0};
    char validationError[256];
    time_t startedAt;
    unsigned int idleSamples = 0;
    const char *phase = "preflight";
    const char *holdAction = "none";
    uint16_t exitReason = 0;
    bool arrived = false;
    bool cancelled = false;
    bool safetyRth = false;
    bool missionSeenActive;

    if (!M4tNavigation_CheckStartupCapability(&outcome) || commandId == NULL ||
        !M4tNavigation_CommandBindingMatches(command, outcome.error, sizeof(outcome.error)) ||
        !M4tNavigation_ParseTarget(command, &target, outcome.error, sizeof(outcome.error))) {
        return outcome;
    }
    M4tNavigation_UpdateTelemetrySettings();
    M4tTelemetry_GetAircraftState(&aircraft);
    if (!M4tNavigationCore_ValidatePreflight(&aircraft, &s_limits,
                                            validationError, sizeof(validationError))) {
        M4tNavigation_SetError(&outcome, "preflight rejected: %s", validationError);
        return outcome;
    }

    pthread_mutex_lock(&s_mutex);
    if (!s_journal.ready || s_journal.readyConsumed || s_journal.readyUntil < time(NULL) ||
        !s_journal.homeValid || s_journal.active) {
        pthread_mutex_unlock(&s_mutex);
        M4tNavigation_SetError(&outcome, "a current, unused STARTUP ready state is required");
        return outcome;
    }
    if (!M4tNavigationCore_ValidateTarget(&target, &s_journal.originalHome, &s_limits,
                                         validationError, sizeof(validationError))) {
        pthread_mutex_unlock(&s_mutex);
        M4tNavigation_SetError(&outcome, "target rejected: %s", validationError);
        return outcome;
    }
    s_journal.readyConsumed = true;
    s_journal.active = true;
    s_journal.terminal = false;
    s_journal.target = target;
    s_journal.codeName = -1;
    M4tNavigation_CopyString(s_journal.activeCommandId, sizeof(s_journal.activeCommandId), commandId);
    M4tNavigation_CopyString(s_journal.phase, sizeof(s_journal.phase), "preflight");
    M4tNavigation_CopyString(s_journal.submissionState, sizeof(s_journal.submissionState), "prepared");
    s_journal.safetyAction[0] = '\0';
    s_cancelRequested = false;
    s_safetyRth = false;
    s_rthStarted = false;
    memset(&s_mission, 0, sizeof(s_mission));
    if (!M4tNavigation_SaveJournalLocked()) {
        s_journal.active = false;
        pthread_mutex_unlock(&s_mutex);
        M4tNavigation_SetError(&outcome, "could not persist prepared mission; mission was not submitted");
        return outcome;
    }
    pthread_mutex_unlock(&s_mutex);

    M4tTelemetry_GetNavigationStatus(&telemetryStatus);
    telemetryStatus.missionActive = true;
    M4tNavigation_CopyString(telemetryStatus.missionCommandId,
                             sizeof(telemetryStatus.missionCommandId), commandId);
    M4tNavigation_CopyString(telemetryStatus.missionPhase,
                             sizeof(telemetryStatus.missionPhase), "preflight");
    M4tTelemetry_SetNavigationStatus(&telemetryStatus);
    M4tNavigation_PublishProgress("preflight", psdkCode, callback, userData);

    request.version = 0;
    request.operation = 0;
    request.mea = (float) s_limits.minimumRouteAltitudeM;
    request.fly_vel = s_limits.maximumHorizontalSpeedMps;
    request.goal_num = 1;
    /* PSDK 3.16's official M4T sample passes degrees despite the header saying radians. */
    request.cmd_mode_point_info[0].lat = target.latitudeDeg;
    request.cmd_mode_point_info[0].lon = target.longitudeDeg;
    request.cmd_mode_point_info[0].alt = (float) target.altitudeEllipsoidM;
    M4tNavigation_PublishProgress("submitting", psdkCode, callback, userData);
    psdkCode = s_adapter.setModeStartMission(request, &response);
    if (psdkCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS || response.ret_code != 0) {
        T_DjiReturnCode submissionCode = psdkCode;
        T_DjiReturnCode holdCode = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
        (void) M4tNavigation_HoldOrRth(&holdCode, &holdAction);
        M4tNavigation_FinishJournal("failed", holdAction);
        M4tNavigation_SetError(&outcome,
                               "mission submission failed: %s (0x%08llX), route_error=%u, action=%s",
                               M4tNavigation_ReturnCodeName(submissionCode),
                               (unsigned long long) submissionCode,
                               response.error_code, holdAction);
        return outcome;
    }

    pthread_mutex_lock(&s_mutex);
    s_journal.codeName = response.code_name;
    M4tNavigation_CopyString(s_journal.submissionState, sizeof(s_journal.submissionState), "submitted");
    M4tNavigation_CopyString(s_journal.phase, sizeof(s_journal.phase), "takeoff");
    (void) M4tNavigation_SaveJournalLocked();
    pthread_mutex_unlock(&s_mutex);
    startedAt = time(NULL);

    while (time(NULL) - startedAt < M4T_NAVIGATION_MISSION_TIMEOUT_SECONDS) {
        T_M4tMissionCallbackState mission;
        M4tTelemetry_GetSnapshot(&snapshot);
        M4tTelemetry_GetAircraftState(&aircraft);
        pthread_mutex_lock(&s_mutex);
        mission = s_mission;
        cancelled = s_cancelRequested;
        safetyRth = s_safetyRth;
        pthread_mutex_unlock(&s_mutex);
        missionSeenActive = mission.seenActive;
        if (cancelled || safetyRth) {
            break;
        }
        phase = M4tNavigation_IsAirborne(&snapshot) ? "enroute" : "takeoff";
        if (M4tNavigationCore_UpdateArrival(&arrival, mission.received && mission.state == 0,
                                            &aircraft, &target, &s_limits)) {
            arrived = true;
            break;
        }
        if (mission.received && mission.state == 0 && missionSeenActive) {
            idleSamples++;
            if (idleSamples >= s_limits.arrivalConsecutiveSamples) {
                break;
            }
        } else {
            idleSamples = 0;
        }
        M4tTelemetry_GetNavigationStatus(&telemetryStatus);
        M4tNavigation_CopyString(telemetryStatus.missionPhase,
                                 sizeof(telemetryStatus.missionPhase), phase);
        telemetryStatus.missionCodeName = response.code_name;
        telemetryStatus.missionState = mission.state;
        telemetryStatus.distanceRemainingM = mission.distanceRemainingM;
        telemetryStatus.timeRemainingS = mission.timeRemainingS;
        M4tTelemetry_SetNavigationStatus(&telemetryStatus);
        M4tNavigation_PublishProgress(phase, psdkCode, callback, userData);
        sleep(1);
    }

    if (arrived) {
        M4tNavigation_FinishJournal("arrived", "hover");
        M4tTelemetry_GetAircraftState(&aircraft);
        outcome.result = cJSON_CreateObject();
        if (outcome.result != NULL) {
            cJSON_AddStringToObject(outcome.result, "status", "arrived");
            cJSON_AddNumberToObject(outcome.result, "code_name", response.code_name);
            cJSON_AddItemToObject(outcome.result, "target", M4tNavigation_CreateTargetJson(&target));
            cJSON_AddItemToObject(outcome.result, "position", M4tNavigation_CreatePositionJson(&aircraft));
        }
        outcome.terminal = M4T_NAVIGATION_TERMINAL_COMPLETED;
        M4tNavigation_PublishProgress("arrived", psdkCode, callback, userData);
        return outcome;
    }

    if (cancelled || safetyRth) {
        if (!M4tNavigation_WaitForLanding(M4T_NAVIGATION_RTH_TIMEOUT_SECONDS)) {
            M4tNavigation_FinishJournal("rth_unconfirmed", safetyRth ? "safety_rth" : "cancel_rth");
            M4tNavigation_SetError(&outcome, "RTH did not reach landed and motors-stopped state");
            return outcome;
        }
        M4tNavigation_FinishJournal("landed", safetyRth ? "safety_rth" : "cancel_rth");
        outcome.result = cJSON_CreateObject();
        if (outcome.result != NULL) {
            cJSON_AddStringToObject(outcome.result, "status", "landed");
            cJSON_AddStringToObject(outcome.result, "safety_action",
                                   safetyRth ? "safety_rth" : "cancel_rth");
        }
        outcome.terminal = safetyRth ? M4T_NAVIGATION_TERMINAL_FAILED
                                     : M4T_NAVIGATION_TERMINAL_CANCELLED;
        if (safetyRth) {
            M4tNavigation_SetError(&outcome, "navigation aborted by automatic safety RTH");
        }
        return outcome;
    }

    (void) s_adapter.getExitReason(&exitReason);
    (void) M4tNavigation_HoldOrRth(&psdkCode, &holdAction);
    M4tNavigation_FinishJournal("failed", holdAction);
    M4tNavigation_SetError(&outcome, "mission ended before arrival: exit_reason=%u, action=%s",
                           exitReason, holdAction);
    return outcome;
}

T_M4tNavigationOutcome M4tNavigation_ExecuteCancel(const char *navigationCommandId)
{
    T_M4tNavigationOutcome outcome = M4tNavigation_NewOutcome();
    T_DjiReturnCode code = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;

    pthread_mutex_lock(&s_mutex);
    if (!s_journal.active || navigationCommandId == NULL ||
        strcmp(s_journal.activeCommandId, navigationCommandId) != 0) {
        pthread_mutex_unlock(&s_mutex);
        M4tNavigation_SetError(&outcome, "CANCEL_NAVIGATION does not match the active mission");
        return outcome;
    }
    s_cancelRequested = true;
    if (!M4tNavigation_StartRthLocked("cancel_rth", &code)) {
        pthread_mutex_unlock(&s_mutex);
        M4tNavigation_SetError(&outcome, "RTH start failed: %s (0x%08llX)",
                               M4tNavigation_ReturnCodeName(code), (unsigned long long) code);
        return outcome;
    }
    pthread_mutex_unlock(&s_mutex);
    if (!M4tNavigation_WaitForLanding(M4T_NAVIGATION_RTH_TIMEOUT_SECONDS)) {
        M4tNavigation_SetError(&outcome, "cancel RTH did not reach landed and motors-stopped state");
        return outcome;
    }
    outcome.result = cJSON_CreateObject();
    if (outcome.result != NULL) {
        cJSON_AddStringToObject(outcome.result, "status", "landed");
        cJSON_AddStringToObject(outcome.result, "action", "cancel_rth");
    }
    outcome.terminal = M4T_NAVIGATION_TERMINAL_COMPLETED;
    return outcome;
}

T_M4tNavigationOutcome M4tNavigation_ExecuteReturnHome(void)
{
    T_M4tNavigationOutcome outcome = M4tNavigation_NewOutcome();
    T_DjiReturnCode code = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;

    if (!s_initialized || !s_controllerReady) {
        M4tNavigation_SetError(&outcome, "flight controller is unavailable");
        return outcome;
    }
    pthread_mutex_lock(&s_mutex);
    if (s_journal.active) {
        pthread_mutex_unlock(&s_mutex);
        M4tNavigation_SetError(&outcome, "navigation is active; use CANCEL_NAVIGATION");
        return outcome;
    }
    s_rthStarted = false;
    if (!M4tNavigation_StartRthLocked("explicit_return_home", &code)) {
        pthread_mutex_unlock(&s_mutex);
        M4tNavigation_SetError(&outcome, "RTH start failed: %s (0x%08llX)",
                               M4tNavigation_ReturnCodeName(code), (unsigned long long) code);
        return outcome;
    }
    pthread_mutex_unlock(&s_mutex);
    if (!M4tNavigation_WaitForLanding(M4T_NAVIGATION_RTH_TIMEOUT_SECONDS)) {
        M4tNavigation_SetError(&outcome, "RTH did not reach landed and motors-stopped state");
        return outcome;
    }
    M4tNavigation_FinishJournal("landed", "explicit_return_home");
    outcome.result = cJSON_CreateObject();
    if (outcome.result != NULL) {
        cJSON_AddStringToObject(outcome.result, "status", "landed");
        cJSON_AddStringToObject(outcome.result, "action", "explicit_return_home");
    }
    outcome.terminal = M4T_NAVIGATION_TERMINAL_COMPLETED;
    return outcome;
}

bool M4tNavigation_IsActive(void)
{
    bool active;
    pthread_mutex_lock(&s_mutex);
    active = s_journal.active;
    pthread_mutex_unlock(&s_mutex);
    return active;
}
