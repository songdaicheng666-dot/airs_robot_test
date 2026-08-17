#include "test_m4t_cloud_relay.h"

#include "m4t_telemetry.h"
#include <curl/curl.h>
#include <dji_error.h>
#include <dji_logger.h>
#include <utils/cJSON.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define M4T_RELAY_DEFAULT_CONFIG_PATH "m4t_relay_config.json"
#define M4T_RELAY_MAX_CONFIG_SIZE (8U * 1024U)
#define M4T_RELAY_MAX_RESPONSE_SIZE (64U * 1024U)
#define M4T_RELAY_MAX_BASE_URL 256U
#define M4T_RELAY_MAX_DEVICE_ID 64U
#define M4T_RELAY_MAX_TOKEN 192U
#define M4T_RELAY_MAX_URL 768U
#define M4T_RELAY_MAX_PATH 512U

typedef struct {
    char baseUrl[M4T_RELAY_MAX_BASE_URL];
    char deviceId[M4T_RELAY_MAX_DEVICE_ID];
    char deviceToken[M4T_RELAY_MAX_TOKEN];
    long pollTimeoutSeconds;
    unsigned int heartbeatIntervalSeconds;
} T_M4tRelayConfig;

typedef struct {
    char *data;
    size_t size;
} T_M4tHttpResponse;

static T_M4tRelayConfig s_config;
static pthread_t s_pollThread;
static pthread_t s_heartbeatThread;

static bool M4tRelay_IsSafeIdentifier(const char *value)
{
    size_t i;
    size_t length;

    if (value == NULL) {
        return false;
    }
    length = strlen(value);
    if (length == 0 || length >= M4T_RELAY_MAX_DEVICE_ID) {
        return false;
    }
    for (i = 0; i < length; ++i) {
        if (!isalnum((unsigned char) value[i]) && value[i] != '-' && value[i] != '_' && value[i] != '.') {
            return false;
        }
    }
    return true;
}

static bool M4tRelay_CopyJsonString(cJSON *root, const char *key, char *target, size_t targetSize)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    size_t length;

    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    length = strlen(item->valuestring);
    if (length == 0 || length >= targetSize) {
        return false;
    }
    memcpy(target, item->valuestring, length + 1);
    return true;
}

static char *M4tRelay_ReadFile(const char *path)
{
    FILE *file = fopen(path, "rb");
    long length;
    char *buffer;

    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
        (unsigned long) length > M4T_RELAY_MAX_CONFIG_SIZE || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    buffer = calloc((size_t) length + 1, 1);
    if (buffer == NULL || fread(buffer, 1, (size_t) length, file) != (size_t) length) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    fclose(file);
    return buffer;
}

static bool M4tRelay_LoadConfig(void)
{
    const char *configPath = getenv("M4T_RELAY_CONFIG");
    char *configText;
    cJSON *root;
    cJSON *number;
    size_t baseUrlLength;

    if (configPath == NULL || configPath[0] == '\0') {
        configPath = M4T_RELAY_DEFAULT_CONFIG_PATH;
    }
    configText = M4tRelay_ReadFile(configPath);
    if (configText == NULL) {
        USER_LOG_ERROR("M4T relay could not read config file: %s", configPath);
        return false;
    }
    root = cJSON_Parse(configText);
    free(configText);
    if (root == NULL || !cJSON_IsObject(root) ||
        !M4tRelay_CopyJsonString(root, "base_url", s_config.baseUrl, sizeof(s_config.baseUrl)) ||
        !M4tRelay_CopyJsonString(root, "device_id", s_config.deviceId, sizeof(s_config.deviceId)) ||
        !M4tRelay_CopyJsonString(root, "device_token", s_config.deviceToken, sizeof(s_config.deviceToken))) {
        USER_LOG_ERROR("M4T relay config is missing a required string");
        cJSON_Delete(root);
        return false;
    }

    s_config.pollTimeoutSeconds = 25;
    s_config.heartbeatIntervalSeconds = 5;
    number = cJSON_GetObjectItemCaseSensitive(root, "poll_timeout_seconds");
    if (cJSON_IsNumber(number)) {
        s_config.pollTimeoutSeconds = number->valueint;
    }
    number = cJSON_GetObjectItemCaseSensitive(root, "heartbeat_interval_seconds");
    if (cJSON_IsNumber(number)) {
        s_config.heartbeatIntervalSeconds = (unsigned int) number->valueint;
    }
    cJSON_Delete(root);

    baseUrlLength = strlen(s_config.baseUrl);
    while (baseUrlLength > 0 && s_config.baseUrl[baseUrlLength - 1] == '/') {
        s_config.baseUrl[--baseUrlLength] = '\0';
    }
    if ((strncmp(s_config.baseUrl, "http://", 7) != 0 && strncmp(s_config.baseUrl, "https://", 8) != 0) ||
        !M4tRelay_IsSafeIdentifier(s_config.deviceId) || strlen(s_config.deviceToken) < 32 ||
        s_config.pollTimeoutSeconds < 1 || s_config.pollTimeoutSeconds > 30 ||
        s_config.heartbeatIntervalSeconds < 1 || s_config.heartbeatIntervalSeconds > 60) {
        USER_LOG_ERROR("M4T relay config contains an invalid value");
        return false;
    }
    USER_LOG_INFO("M4T relay configured for %s at %s", s_config.deviceId, s_config.baseUrl);
    return true;
}

static size_t M4tRelay_WriteResponse(void *contents, size_t size, size_t count, void *userData)
{
    T_M4tHttpResponse *response = userData;
    size_t bytes;
    char *expanded;

    if (size != 0 && count > SIZE_MAX / size) {
        return 0;
    }
    bytes = size * count;
    if (response->size + bytes > M4T_RELAY_MAX_RESPONSE_SIZE) {
        return 0;
    }
    expanded = realloc(response->data, response->size + bytes + 1);
    if (expanded == NULL) {
        return 0;
    }
    response->data = expanded;
    memcpy(response->data + response->size, contents, bytes);
    response->size += bytes;
    response->data[response->size] = '\0';
    return bytes;
}

static CURLcode M4tRelay_Request(
    const char *method,
    const char *path,
    const char *requestBody,
    long timeoutSeconds,
    T_M4tHttpResponse *response,
    long *httpStatus)
{
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    char url[M4T_RELAY_MAX_URL];
    char authorization[M4T_RELAY_MAX_TOKEN + 32];
    CURLcode result;

    response->data = calloc(1, 1);
    response->size = 0;
    *httpStatus = 0;
    if (curl == NULL || response->data == NULL ||
        snprintf(url, sizeof(url), "%s%s", s_config.baseUrl, path) >= (int) sizeof(url) ||
        snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", s_config.deviceToken) >=
            (int) sizeof(authorization)) {
        if (curl != NULL) {
            curl_easy_cleanup(curl);
        }
        free(response->data);
        response->data = NULL;
        return CURLE_FAILED_INIT;
    }

    headers = curl_slist_append(headers, authorization);
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "m4t-manifold3-relay/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, M4tRelay_WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) strlen(requestBody));
    }

    result = curl_easy_perform(curl);
    if (result == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, httpStatus);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

static bool M4tRelay_PostJson(const char *path, cJSON *body)
{
    char *requestBody = cJSON_PrintUnformatted(body);
    T_M4tHttpResponse response = {0};
    long httpStatus;
    CURLcode result;

    if (requestBody == NULL) {
        return false;
    }
    result = M4tRelay_Request("POST", path, requestBody, 15L, &response, &httpStatus);
    cJSON_free(requestBody);
    free(response.data);
    if (result != CURLE_OK) {
        USER_LOG_ERROR("M4T relay POST failed: %s", curl_easy_strerror(result));
        return false;
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        USER_LOG_ERROR("M4T relay POST returned HTTP %ld", httpStatus);
        return false;
    }
    return true;
}

static void M4tRelay_Backoff(unsigned int *delaySeconds, unsigned int *seed)
{
    struct timespec delay = {
        .tv_sec = *delaySeconds,
        .tv_nsec = (long) (rand_r(seed) % 250U) * 1000000L,
    };
    nanosleep(&delay, NULL);
    if (*delaySeconds < 30U) {
        *delaySeconds *= 2U;
        if (*delaySeconds > 30U) {
            *delaySeconds = 30U;
        }
    }
}

static bool M4tRelay_PostCommandState(
    const char *commandId,
    const char *state,
    cJSON *resultValue,
    const char *error)
{
    char path[M4T_RELAY_MAX_PATH];
    cJSON *body;
    bool success;

    if (!M4tRelay_IsSafeIdentifier(commandId) ||
        snprintf(path, sizeof(path), "/v1/devices/%s/commands/%s/state", s_config.deviceId, commandId) >=
            (int) sizeof(path)) {
        return false;
    }
    body = cJSON_CreateObject();
    if (body == NULL) {
        return false;
    }
    cJSON_AddStringToObject(body, "state", state);
    if (resultValue != NULL) {
        cJSON_AddItemToObject(body, "result", cJSON_Duplicate(resultValue, true));
    }
    if (error != NULL) {
        cJSON_AddStringToObject(body, "error", error);
    }
    success = M4tRelay_PostJson(path, body);
    cJSON_Delete(body);
    return success;
}

static void M4tRelay_PostCommandStateUntilSuccess(
    const char *commandId,
    const char *state,
    cJSON *resultValue,
    const char *error,
    unsigned int *seed)
{
    unsigned int retryDelay = 1;
    while (!M4tRelay_PostCommandState(commandId, state, resultValue, error)) {
        M4tRelay_Backoff(&retryDelay, seed);
    }
}

static cJSON *M4tRelay_ExecuteCommand(cJSON *command, const char *commandType)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *payload;

    if (result == NULL) {
        return NULL;
    }
    if (strcmp(commandType, "PING") == 0) {
        cJSON_AddStringToObject(result, "message", "pong");
        payload = cJSON_GetObjectItemCaseSensitive(command, "payload");
        if (cJSON_IsObject(payload)) {
            cJSON_AddItemToObject(result, "echo", cJSON_Duplicate(payload, true));
        } else {
            cJSON_AddItemToObject(result, "echo", cJSON_CreateObject());
        }
        return result;
    }
    if (strcmp(commandType, "STATUS_QUERY") == 0) {
        cJSON *telemetry = M4tTelemetry_CreateJson();
        if (telemetry == NULL) {
            cJSON_Delete(result);
            return NULL;
        }
        cJSON_AddItemToObject(result, "telemetry", telemetry);
        return result;
    }
    cJSON_Delete(result);
    return NULL;
}

static void *M4tRelay_PollTask(void *argument)
{
    char path[M4T_RELAY_MAX_PATH];
    unsigned int retryDelay = 1;
    unsigned int seed = (unsigned int) time(NULL) ^ (unsigned int) (uintptr_t) pthread_self();

    (void) argument;
    snprintf(path, sizeof(path), "/v1/devices/%s/commands/next?timeout_s=%ld",
             s_config.deviceId, s_config.pollTimeoutSeconds);
    while (true) {
        T_M4tHttpResponse response = {0};
        long httpStatus = 0;
        CURLcode requestResult = M4tRelay_Request(
            "GET", path, NULL, s_config.pollTimeoutSeconds + 10L, &response, &httpStatus);
        if (requestResult != CURLE_OK) {
            USER_LOG_ERROR("M4T relay long poll failed: %s", curl_easy_strerror(requestResult));
            free(response.data);
            M4tRelay_Backoff(&retryDelay, &seed);
            continue;
        }
        if (httpStatus == 204) {
            free(response.data);
            retryDelay = 1;
            continue;
        }
        if (httpStatus != 200) {
            USER_LOG_ERROR("M4T relay long poll returned HTTP %ld", httpStatus);
            free(response.data);
            M4tRelay_Backoff(&retryDelay, &seed);
            continue;
        }

        cJSON *command = cJSON_Parse(response.data);
        cJSON *commandIdValue;
        cJSON *commandTypeValue;
        const char *commandId;
        const char *commandType;
        cJSON *result;
        free(response.data);
        retryDelay = 1;
        if (command == NULL) {
            USER_LOG_ERROR("M4T relay received invalid command JSON");
            continue;
        }
        commandIdValue = cJSON_GetObjectItemCaseSensitive(command, "command_id");
        commandTypeValue = cJSON_GetObjectItemCaseSensitive(command, "type");
        if (!cJSON_IsString(commandIdValue) || !cJSON_IsString(commandTypeValue) ||
            !M4tRelay_IsSafeIdentifier(commandIdValue->valuestring)) {
            USER_LOG_ERROR("M4T relay command is missing a valid ID or type");
            cJSON_Delete(command);
            continue;
        }
        commandId = commandIdValue->valuestring;
        commandType = commandTypeValue->valuestring;
        M4tRelay_PostCommandStateUntilSuccess(commandId, "RECEIVED", NULL, NULL, &seed);
        result = M4tRelay_ExecuteCommand(command, commandType);
        if (result == NULL) {
            M4tRelay_PostCommandStateUntilSuccess(
                commandId, "FAILED", NULL, "unsupported command or telemetry allocation failure", &seed);
        } else {
            M4tRelay_PostCommandStateUntilSuccess(commandId, "COMPLETED", result, NULL, &seed);
            USER_LOG_INFO("M4T relay completed command %s (%s)", commandId, commandType);
            cJSON_Delete(result);
        }
        cJSON_Delete(command);
    }
    return NULL;
}

static void *M4tRelay_HeartbeatTask(void *argument)
{
    char path[M4T_RELAY_MAX_PATH];

    (void) argument;
    snprintf(path, sizeof(path), "/v1/devices/%s/telemetry", s_config.deviceId);
    while (true) {
        cJSON *telemetry = M4tTelemetry_CreateJson();
        if (telemetry == NULL) {
            USER_LOG_ERROR("M4T relay could not allocate telemetry JSON");
        } else {
            M4tRelay_PostJson(path, telemetry);
            cJSON_Delete(telemetry);
        }
        sleep(s_config.heartbeatIntervalSeconds);
    }
    return NULL;
}

T_DjiReturnCode DjiTest_M4tCloudRelayStartService(void)
{
    T_DjiReturnCode result;
    int threadResult;

    memset(&s_config, 0, sizeof(s_config));
    if (!M4tRelay_LoadConfig()) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        USER_LOG_ERROR("M4T relay failed to initialize libcurl");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    result = M4tTelemetry_Start();
    if (result != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        return result;
    }
    threadResult = pthread_create(&s_pollThread, NULL, M4tRelay_PollTask, NULL);
    if (threadResult == 0) {
        threadResult = pthread_create(&s_heartbeatThread, NULL, M4tRelay_HeartbeatTask, NULL);
    }
    if (threadResult != 0) {
        USER_LOG_ERROR("M4T relay failed to create communication threads: %s", strerror(threadResult));
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    pthread_detach(s_pollThread);
    pthread_detach(s_heartbeatThread);
    USER_LOG_INFO("M4T cloud relay service started; flight-control commands are disabled");
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}
