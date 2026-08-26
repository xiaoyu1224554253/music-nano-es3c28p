#ifndef __SYS_SERIAL_H__
#define __SYS_SERIAL_H__

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    APP_CMD_BT_SCAN      = 0,
    APP_CMD_BT_CONNECT   = 1,
    APP_CMD_BT_DISCONNECT = 2,
    APP_CMD_PLAY         = 3,
    APP_CMD_STOP         = 4,
    APP_CMD_PAUSE        = 5,
    APP_CMD_INFO         = 6,
    APP_CMD_COVER_READY  = 7,
} app_cmd_type_t;

typedef struct {
    app_cmd_type_t type;
    char           param[32];
} app_cmd_t;

void sys_serial_init(QueueHandle_t app_cmd_queue);

#endif
