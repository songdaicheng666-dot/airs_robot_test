/**
 ********************************************************************
 * @file    pps.c
 * @version V2.0.0
 * @date    2025/10/20
 * @brief
 *
 * @copyright (c) 2021 DJI. All rights reserved.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <pps.h>
#include <pthread.h>
#include "dji_logger.h"
#include "osal/osal.h"

/* Private constants ---------------------------------------------------------*/
// GPIO pin 26 connected to PPS signal
#define PPS_GPIO 26

/* Private types -------------------------------------------------------------*/

/* Private values -------------------------------------------------------------*/
static uint32_t pps_newest_trigger_time_ms = 0;
static uint32_t pps_trigger_time_diff_ms = 0;
static int gpio_fd = 0;
static pthread_t pps_thread;

/* Private functions declaration ---------------------------------------------*/
static void *pps_signal_watcher(void *arg);
static void handle_pps_event();
static uint32_t get_pps_trigger_time_diff_ms();

/* Exported functions definition ---------------------------------------------*/
T_DjiReturnCode DjiTestRsp_PpsSignalResponseInit(void) {
    char buf[128];
    int fd, len;
    int gpio = PPS_GPIO;

    fd = open("/sys/class/gpio/unexport", O_WRONLY);
    if (fd >= 0) {
        len = snprintf(buf, sizeof(buf), "%d", gpio);
        write(fd, buf, len);
        close(fd);
    }

    if ((fd = open("/sys/class/gpio/export", O_WRONLY)) < 0) {
        USER_LOG_ERROR("Failed to open export");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    len = snprintf(buf, sizeof(buf), "%d", gpio);
    if (write(fd, buf, len) != len) {
        USER_LOG_ERROR("Failed to export GPIO");
        close(fd);
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    close(fd);

    snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/direction", gpio);
    if ((fd = open(buf, O_WRONLY)) < 0) {
        USER_LOG_ERROR("Failed to open direction");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    if (write(fd, "in", 2) != 2) {
        USER_LOG_ERROR("Failed to set direction");
        close(fd);
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    close(fd);

    snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/edge", gpio);
    if ((fd = open(buf, O_WRONLY)) < 0) {
        USER_LOG_ERROR("Failed to open edge");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    if (write(fd, "rising", 6) != 6) {
        USER_LOG_ERROR("Failed to set edge");
        close(fd);
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    close(fd);

    snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/value", gpio);
    gpio_fd = open(buf, O_RDONLY | O_NONBLOCK);
    if(gpio_fd < 0) {
        USER_LOG_ERROR("Failed to open value");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }


    if (pthread_create(&pps_thread, NULL, pps_signal_watcher, NULL) != 0) {
        USER_LOG_ERROR("Failed to create PPS watcher thread");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode DjiTestRsp_GetNewestPpsTriggerLocalTimeUs(uint64_t *localTimeUs) {
    if (localTimeUs == NULL) {
        USER_LOG_ERROR("input pointer is null.");
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }

    if (pps_newest_trigger_time_ms == 0) {
        USER_LOG_WARN("pps have not been triggered.");
        return DJI_ERROR_SYSTEM_MODULE_CODE_BUSY;
    }
    *localTimeUs = (uint64_t)pps_newest_trigger_time_ms * 1000ULL;

    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

/* Private functions definition-----------------------------------------------*/
static void handle_pps_event() {
    uint32_t timeNowMs;
    Osal_GetTimeMs(&timeNowMs);

    if (pps_newest_trigger_time_ms != 0) {
        pps_trigger_time_diff_ms = timeNowMs - pps_newest_trigger_time_ms;
    }

    pps_newest_trigger_time_ms = timeNowMs;
}

static uint32_t get_pps_trigger_time_diff_ms() {
    return pps_trigger_time_diff_ms;
}

static void *pps_signal_watcher(void * arg) {
    struct pollfd pfd;
    char buf;

    pfd.fd = gpio_fd;
    pfd.events = POLLPRI | POLLERR;

    lseek(gpio_fd, 0, SEEK_SET);
    read(gpio_fd, &buf, 1);

    USER_LOG_INFO("Monitoring PPS signal on GPIO %d...\n", PPS_GPIO);

    while (true) {
        int ret = poll(&pfd, 1, 1500);

        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll failed");
            break;
        }

        if (ret == 0) {
            USER_LOG_ERROR("PPS signal polling timeout");
            continue;
        }

        if (pfd.revents & POLLPRI) {
            lseek(gpio_fd, 0, SEEK_SET);
            if (read(gpio_fd, &buf, 1) != 1) {
                perror("read failed");
                continue;
            }

            uint32_t timeNowMs;
            Osal_GetTimeMs(&timeNowMs);

            if (pps_newest_trigger_time_ms > 0 && (timeNowMs - pps_newest_trigger_time_ms) < 800) {
                continue;
            }

            handle_pps_event();

            uint32_t diff_ms = get_pps_trigger_time_diff_ms();
            uint64_t time_us;
            DjiTestRsp_GetNewestPpsTriggerLocalTimeUs (&time_us);

            if (time_us > 0) {
                USER_LOG_INFO("PPS triggered. Interval: %u ms | Time: %llu μs\n",
                       diff_ms, time_us);
            } else {
                USER_LOG_INFO("PPS triggered. (First event)\n");
            }
        }
    }

    close(gpio_fd);

    int fd = open("/sys/class/gpio/unexport", O_WRONLY);
    if (fd >= 0) {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d", PPS_GPIO);
        write(fd, buf, len);
        close(fd);
    }

    printf("\nExiting PPS monitor\n");
    return 0;
}