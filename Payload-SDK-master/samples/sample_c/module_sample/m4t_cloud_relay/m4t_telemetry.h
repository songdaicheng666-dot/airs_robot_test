#ifndef M4T_TELEMETRY_H
#define M4T_TELEMETRY_H

#include <dji_typedef.h>
#include <utils/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

T_DjiReturnCode M4tTelemetry_Start(void);
cJSON *M4tTelemetry_CreateJson(void);

#ifdef __cplusplus
}
#endif

#endif
