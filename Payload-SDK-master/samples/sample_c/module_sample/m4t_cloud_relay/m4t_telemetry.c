#include "m4t_telemetry.h"

#include <dji_error.h>
#include <dji_fc_subscription.h>
#include <dji_logger.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
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
} T_M4tTelemetrySnapshot;

typedef struct {
    bool flight;
    bool displayMode;
    bool position;
    bool gps;
    bool rtkConnection;
    bool rtkPosition;
    bool battery;
} T_M4tSubscriptions;

static pthread_mutex_t s_snapshotMutex = PTHREAD_MUTEX_INITIALIZER;
static T_M4tTelemetrySnapshot s_snapshot;
static pthread_t s_telemetryThread;

static void M4tTelemetry_FormatUtcNow(char *buffer, size_t bufferSize)
{
    time_t now = time(NULL);
    struct tm utcTime;

    if (gmtime_r(&now, &utcTime) == NULL ||
        strftime(buffer, bufferSize, "%Y-%m-%dT%H:%M:%SZ", &utcTime) == 0) {
        snprintf(buffer, bufferSize, "1970-01-01T00:00:00Z");
    }
}

static bool M4tTelemetry_Subscribe(E_DjiFcSubscriptionTopic topic, const char *name)
{
    T_DjiReturnCode result = DjiFcSubscription_SubscribeTopic(topic, DJI_DATA_SUBSCRIPTION_TOPIC_1_HZ, NULL);
    if (result != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("M4T relay failed to subscribe %s, code: 0x%08llX", name, (unsigned long long) result);
        return false;
    }
    return true;
}

static const char *M4tTelemetry_FlightStatusName(uint8_t status)
{
    switch (status) {
        case DJI_FC_SUBSCRIPTION_FLIGHT_STATUS_STOPED:
            return "STOPPED";
        case DJI_FC_SUBSCRIPTION_FLIGHT_STATUS_ON_GROUND:
            return "ON_GROUND";
        case DJI_FC_SUBSCRIPTION_FLIGHT_STATUS_IN_AIR:
            return "IN_AIR";
        default:
            return "UNKNOWN";
    }
}

static void M4tTelemetry_AddError(cJSON *errors, const char *error)
{
    cJSON_AddItemToArray(errors, cJSON_CreateString(error));
}

static void M4tTelemetry_Sample(const T_M4tSubscriptions *subscriptions, uint64_t sequence)
{
    T_M4tTelemetrySnapshot next = {0};
    T_DjiDataTimestamp timestamp = {0};
    T_DjiReturnCode result;
    bool anyTopicValid = false;

    next.sequence = sequence;
    M4tTelemetry_FormatUtcNow(next.recordedAt, sizeof(next.recordedAt));

    if (subscriptions->flight && subscriptions->displayMode) {
        T_DjiFcSubscriptionFlightStatus flightStatus = 0;
        T_DjiFcSubscriptionDisplaymode displayMode = 0;
        result = DjiFcSubscription_GetLatestValueOfTopic(
            DJI_FC_SUBSCRIPTION_TOPIC_STATUS_FLIGHT,
            (uint8_t *) &flightStatus,
            sizeof(flightStatus),
            &timestamp);
        if (result == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            result = DjiFcSubscription_GetLatestValueOfTopic(
                DJI_FC_SUBSCRIPTION_TOPIC_STATUS_DISPLAYMODE,
                (uint8_t *) &displayMode,
                sizeof(displayMode),
                &timestamp);
        }
        if (result == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            next.flightValid = true;
            next.flightStatus = flightStatus;
            next.displayMode = displayMode;
            anyTopicValid = true;
        }
    }

    if (subscriptions->position) {
        T_DjiFcSubscriptionPositionFused position = {0};
        result = DjiFcSubscription_GetLatestValueOfTopic(
            DJI_FC_SUBSCRIPTION_TOPIC_POSITION_FUSED,
            (uint8_t *) &position,
            sizeof(position),
            &timestamp);
        if (result == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            next.positionValid = true;
            next.longitudeDeg = position.longitude * 180.0 / M_PI;
            next.latitudeDeg = position.latitude * 180.0 / M_PI;
            next.altitudeEllipsoidM = position.altitude;
            next.visibleSatellites = position.visibleSatelliteNumber;
            anyTopicValid = true;
        }
    }

    if (subscriptions->gps) {
        T_DjiFcSubscriptionGpsDetails gps = {0};
        result = DjiFcSubscription_GetLatestValueOfTopic(
            DJI_FC_SUBSCRIPTION_TOPIC_GPS_DETAILS,
            (uint8_t *) &gps,
            sizeof(gps),
            &timestamp);
        if (result == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            next.gpsValid = true;
            next.gpsFixState = (int) lroundf(gps.fixState);
            next.horizontalAccuracyM = gps.hacc / 1000.0f;
            next.verticalAccuracyM = gps.vacc / 1000.0f;
            next.satellitesUsed = gps.totalSatelliteNumberUsed;
            anyTopicValid = true;
        }
    }

    if (subscriptions->rtkConnection && subscriptions->rtkPosition) {
        T_DjiFcSubscriptionRTKConnectStatus connection = {0};
        T_DjiFcSubscriptionRtkPositionInfo positionSolution = 0;
        result = DjiFcSubscription_GetLatestValueOfTopic(
            DJI_FC_SUBSCRIPTION_TOPIC_RTK_CONNECT_STATUS,
            (uint8_t *) &connection,
            sizeof(connection),
            &timestamp);
        if (result == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            result = DjiFcSubscription_GetLatestValueOfTopic(
                DJI_FC_SUBSCRIPTION_TOPIC_RTK_POSITION_INFO,
                (uint8_t *) &positionSolution,
                sizeof(positionSolution),
                &timestamp);
        }
        if (result == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            next.rtkValid = true;
            next.rtkConnected = connection.rtkConnected != 0;
            next.rtkPositionSolution = positionSolution;
            anyTopicValid = true;
        }
    }

    if (subscriptions->battery) {
        T_DjiFcSubscriptionWholeBatteryInfo battery = {0};
        result = DjiFcSubscription_GetLatestValueOfTopic(
            DJI_FC_SUBSCRIPTION_TOPIC_BATTERY_INFO,
            (uint8_t *) &battery,
            sizeof(battery),
            &timestamp);
        if (result == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            next.batteryValid = true;
            next.batteryPercentage = battery.percentage;
            next.batteryVoltageV = battery.voltage / 1000.0f;
            next.batteryCurrentA = battery.current / 1000.0f;
            anyTopicValid = true;
        }
    }

    next.psdkConnected = anyTopicValid;
    pthread_mutex_lock(&s_snapshotMutex);
    s_snapshot = next;
    pthread_mutex_unlock(&s_snapshotMutex);
}

static void *M4tTelemetry_Task(void *argument)
{
    T_M4tSubscriptions subscriptions = {0};
    T_DjiReturnCode result;
    uint64_t sequence = 0;

    (void) argument;
    while (true) {
        result = DjiFcSubscription_Init();
        if (result == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            break;
        }
        USER_LOG_ERROR("M4T relay FC subscription init failed, code: 0x%08llX; retrying",
                       (unsigned long long) result);
        sleep(5);
    }

    subscriptions.flight = M4tTelemetry_Subscribe(DJI_FC_SUBSCRIPTION_TOPIC_STATUS_FLIGHT, "flight status");
    subscriptions.displayMode = M4tTelemetry_Subscribe(DJI_FC_SUBSCRIPTION_TOPIC_STATUS_DISPLAYMODE, "display mode");
    subscriptions.position = M4tTelemetry_Subscribe(DJI_FC_SUBSCRIPTION_TOPIC_POSITION_FUSED, "fused position");
    subscriptions.gps = M4tTelemetry_Subscribe(DJI_FC_SUBSCRIPTION_TOPIC_GPS_DETAILS, "GPS details");
    subscriptions.rtkConnection = M4tTelemetry_Subscribe(DJI_FC_SUBSCRIPTION_TOPIC_RTK_CONNECT_STATUS, "RTK connection");
    subscriptions.rtkPosition = M4tTelemetry_Subscribe(DJI_FC_SUBSCRIPTION_TOPIC_RTK_POSITION_INFO, "RTK position info");
    subscriptions.battery = M4tTelemetry_Subscribe(DJI_FC_SUBSCRIPTION_TOPIC_BATTERY_INFO, "battery info");

    while (true) {
        M4tTelemetry_Sample(&subscriptions, ++sequence);
        sleep(1);
    }
    return NULL;
}

T_DjiReturnCode M4tTelemetry_Start(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    M4tTelemetry_FormatUtcNow(s_snapshot.recordedAt, sizeof(s_snapshot.recordedAt));
    if (pthread_create(&s_telemetryThread, NULL, M4tTelemetry_Task, NULL) != 0) {
        USER_LOG_ERROR("M4T relay could not create telemetry thread");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    pthread_detach(s_telemetryThread);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

cJSON *M4tTelemetry_CreateJson(void)
{
    T_M4tTelemetrySnapshot snapshot;
    cJSON *root = cJSON_CreateObject();
    cJSON *flight = cJSON_CreateObject();
    cJSON *position = cJSON_CreateObject();
    cJSON *gps = cJSON_CreateObject();
    cJSON *rtk = cJSON_CreateObject();
    cJSON *battery = cJSON_CreateObject();
    cJSON *errors = cJSON_CreateArray();

    if (root == NULL || flight == NULL || position == NULL || gps == NULL || rtk == NULL ||
        battery == NULL || errors == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(flight);
        cJSON_Delete(position);
        cJSON_Delete(gps);
        cJSON_Delete(rtk);
        cJSON_Delete(battery);
        cJSON_Delete(errors);
        return NULL;
    }

    pthread_mutex_lock(&s_snapshotMutex);
    snapshot = s_snapshot;
    pthread_mutex_unlock(&s_snapshotMutex);

    cJSON_AddStringToObject(root, "recorded_at", snapshot.recordedAt);
    cJSON_AddNumberToObject(root, "sequence", (double) snapshot.sequence);
    cJSON_AddBoolToObject(root, "psdk_connected", snapshot.psdkConnected);

    cJSON_AddBoolToObject(flight, "valid", snapshot.flightValid);
    if (snapshot.flightValid) {
        cJSON_AddNumberToObject(flight, "status_code", snapshot.flightStatus);
        cJSON_AddStringToObject(flight, "status", M4tTelemetry_FlightStatusName(snapshot.flightStatus));
        cJSON_AddNumberToObject(flight, "display_mode_code", snapshot.displayMode);
    } else {
        M4tTelemetry_AddError(errors, "FLIGHT_STATUS_UNAVAILABLE");
    }

    cJSON_AddBoolToObject(position, "valid", snapshot.positionValid);
    if (snapshot.positionValid) {
        cJSON_AddNumberToObject(position, "latitude_deg", snapshot.latitudeDeg);
        cJSON_AddNumberToObject(position, "longitude_deg", snapshot.longitudeDeg);
        cJSON_AddNumberToObject(position, "altitude_ellipsoid_m", snapshot.altitudeEllipsoidM);
        cJSON_AddNumberToObject(position, "visible_satellites", snapshot.visibleSatellites);
    } else {
        M4tTelemetry_AddError(errors, "POSITION_UNAVAILABLE");
    }

    cJSON_AddBoolToObject(gps, "valid", snapshot.gpsValid);
    if (snapshot.gpsValid) {
        cJSON_AddNumberToObject(gps, "fix_state", snapshot.gpsFixState);
        cJSON_AddNumberToObject(gps, "horizontal_accuracy_m", snapshot.horizontalAccuracyM);
        cJSON_AddNumberToObject(gps, "vertical_accuracy_m", snapshot.verticalAccuracyM);
        cJSON_AddNumberToObject(gps, "satellites_used", snapshot.satellitesUsed);
    } else {
        M4tTelemetry_AddError(errors, "GPS_DETAILS_UNAVAILABLE");
    }

    cJSON_AddBoolToObject(rtk, "valid", snapshot.rtkValid);
    if (snapshot.rtkValid) {
        cJSON_AddBoolToObject(rtk, "connected", snapshot.rtkConnected);
        cJSON_AddNumberToObject(rtk, "position_solution", snapshot.rtkPositionSolution);
    } else {
        M4tTelemetry_AddError(errors, "RTK_STATUS_UNAVAILABLE");
    }

    cJSON_AddBoolToObject(battery, "valid", snapshot.batteryValid);
    if (snapshot.batteryValid) {
        cJSON_AddNumberToObject(battery, "percentage", snapshot.batteryPercentage);
        cJSON_AddNumberToObject(battery, "voltage_v", snapshot.batteryVoltageV);
        cJSON_AddNumberToObject(battery, "current_a", snapshot.batteryCurrentA);
    } else {
        M4tTelemetry_AddError(errors, "BATTERY_UNAVAILABLE");
    }
    if (!snapshot.psdkConnected) {
        M4tTelemetry_AddError(errors, "PSDK_NOT_READY");
    }

    cJSON_AddItemToObject(root, "flight", flight);
    cJSON_AddItemToObject(root, "position", position);
    cJSON_AddItemToObject(root, "gps", gps);
    cJSON_AddItemToObject(root, "rtk", rtk);
    cJSON_AddItemToObject(root, "battery", battery);
    cJSON_AddItemToObject(root, "errors", errors);
    return root;
}
