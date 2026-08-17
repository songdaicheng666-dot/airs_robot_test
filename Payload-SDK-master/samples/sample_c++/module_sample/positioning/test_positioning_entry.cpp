/**
 ********************************************************************
 * @file    test_positioning_entry.cpp
 * @brief
 *
 * @copyright (c) 2018 DJI. All rights reserved.
 *
 * All information contained herein is, and remains, the property of DJI.
 * The intellectual and technical concepts contained herein are proprietary
 * to DJI and may be covered by U.S. and foreign patents, patents in process,
 * and protected by trade secret or copyright law.  Dissemination of this
 * information, including but not limited to data and other proprietary
 * material(s) incorporated within the information, in any form, is strictly
 * prohibited without the express written consent of DJI.
 *
 * If you receive this source code without DJI’s authorization, you may not
 * further disseminate the information, and you must immediately remove the
 * source code and notify DJI of its removal. DJI reserves the right to pursue
 * legal actions against you for any loss(es) or damage(s) caused by your
 * failure to do so.
 *
 *********************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include <iostream>
#include <fc_subscription/test_fc_subscription.h>
#include "positioning/test_positioning.h"
#include "dji_positioning.h"
#include "dji_network_rtk.h"
#include "dji_logger.h"
#include "utils/util_misc.h"
#include "dji_platform.h"
#include "time_sync/test_time_sync.h"

#define FC_SUBSCRIPTION_TASK_FREQ         (1)

static void DjiUser_PrintPositioningInfo(void)
{
    T_DjiReturnCode djiStat;
    T_DjiFcSubscriptionRtkPositionInfo positionInfo = {0};
    T_DjiFcSubscriptionGpsPosition gpsPosition = {0};
    T_DjiDataTimestamp timestamp = {0};
    T_DjiOsalHandler *osalHandler = NULL;

    osalHandler = DjiPlatform_GetOsalHandler();
    for (int i = 0; i < 10; ++i) {
        osalHandler->TaskSleepMs(1000 / FC_SUBSCRIPTION_TASK_FREQ);
        djiStat = DjiFcSubscription_GetLatestValueOfTopic(DJI_FC_SUBSCRIPTION_TOPIC_RTK_POSITION_INFO,
                                                          (uint8_t *) &positionInfo,
                                                          sizeof(T_DjiFcSubscriptionRtkPositionInfo),
                                                          &timestamp);
        if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            USER_LOG_ERROR("get value of topic rtk position info error.");
        } else {
            USER_LOG_INFO("RTK position info: %d", positionInfo);
        }

        djiStat = DjiFcSubscription_GetLatestValueOfTopic(DJI_FC_SUBSCRIPTION_TOPIC_GPS_POSITION,
                                                          (uint8_t *) &gpsPosition,
                                                          sizeof(T_DjiFcSubscriptionGpsPosition),
                                                          &timestamp);
        if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            USER_LOG_ERROR("get value of topic gps position error.");
        } else {
            USER_LOG_INFO("gps position: x = %d y = %d z = %d.", gpsPosition.x, gpsPosition.y, gpsPosition.z);
        }
    }
}

static void DjiTest_ReceiveNetworkRtkStateCallback(E_DjiNetworkRtkOnboardState state)
{
    const char* str;
    switch (state)
    {
     case DJI_LOG_IN:
         str = "LOG_IN";
         break;
     case DJI_CONNECTING:
         str = "CONNECTING";
         break;
     case DJI_TRANSFER:
         str = "TRANSFER";
         break;
     case DJI_RECONNECT: 
         str = "RECONNECT";
         break;
     case DJI_BROKEN:
         str = "BROKEN";
         break;
     case DJI_STOPPING:
         str = "STOPPING";
         break;
     case DJI_REBOOTING:
         str = "NEEDED REBOOT";
         break;
     default:
         str = "UNKNOW";
         USER_LOG_WARN("Receive Network RTK State: %d", state);
         break;
    }
    USER_LOG_INFO("Receive Network RTK State: %s", str);
}

/* Exported functions definition ---------------------------------------------*/
T_DjiReturnCode DjiTest_RunOnboardRTKSample(void)
{
    T_DjiReturnCode djiStat = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    T_DjiOsalHandler *osalHandler = DjiPlatform_GetOsalHandler();
    char inputSelectSample;

   USER_LOG_INFO("--> Step 1: Init Sample");

    djiStat = DjiFcSubscription_Init();
    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("init data subscription module error.");
        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
    }

    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_RTK_POSITION_INFO, DJI_DATA_SUBSCRIPTION_TOPIC_50_HZ,
                                               NULL);
    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("Subscribe topic quaternion error.");
        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
    }


    djiStat = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_GPS_POSITION, DJI_DATA_SUBSCRIPTION_TOPIC_1_HZ,
                                               NULL);
    if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("Subscribe topic gps position error.");
        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
    }

start:
    osalHandler->TaskSleepMs(100);

    std::cout
        << "\n"
        << "| Available commands:                                                                                            |\n"
        << "| [0] Start onboard Network RTK                                                                                  |\n"
        << "| [1] Stop onboard Network RTK                                                                                   |\n"
        << "| [2] Print position info                                                                                        |\n"
        << "| [q] Quit                                                                                                       |\n"
        << std::endl;

    std::cin >> inputSelectSample;
    switch (inputSelectSample)
    {
    case '0':
        T_DjiNetworkRtkServiceConfig config;
        {
            std::cout << " Input Network RTK account: ";
            std::cin >> config.account;
            std::cout << " Input Network RTK password: ";
            std::cin >> config.password;
            std::cout << " Input Network RTK host: ";
            std::cin >> config.host;
            std::cout << " Input Network RTK port: ";
            std::cin >> config.port;
            std::cout << " Input Network RTK mountpoint: ";
            std::cin >> config.mountPoint;
            USER_LOG_INFO("Input account: %s, password: %s, host: %s, port: %s, mountpoint: %s",
                config.account, config.password, config.host, config.port, config.mountPoint);
        }
        djiStat = DjiNetworkRtk_RegReceiveNetworkRtkStateCallback(DjiTest_ReceiveNetworkRtkStateCallback);
        if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS)
        {
            USER_LOG_ERROR("reg network rtk state error.");
            return djiStat;
        }

        USER_LOG_INFO("Start onboard Network RTK sample begin");

        djiStat = DjiNetworkRtk_StartService(&config);
        if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS)
        {
            USER_LOG_ERROR("Start network rtk on aircraft error.");
            return djiStat;
        }
        DjiUser_PrintPositioningInfo();
        USER_LOG_INFO("Start onboard Network RTK sample end");
        goto start;
    case '1':
        USER_LOG_INFO("Stop onboard Network RTK sample begin");

        djiStat = DjiNetworkRtk_StopService();
        if (djiStat != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS)
        {
            USER_LOG_ERROR("stop network rtk on aircraft error.");
            return djiStat;
        }
        USER_LOG_INFO("Stop onboard Network RTK sample end");
        goto start;
    case '2':
        DjiUser_PrintPositioningInfo();
        goto start;
    case 'q':
        return djiStat;
    default:
        USER_LOG_ERROR("Invalid command");
        break;
    }
    return djiStat;
}

/****************** (C) COPYRIGHT DJI Innovations *****END OF FILE****/

/****************** (C) COPYRIGHT DJI Innovations *****END OF FILE****/

