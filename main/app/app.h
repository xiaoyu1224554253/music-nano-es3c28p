#ifndef __APP_H__
#define __APP_H__

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 应用级命令 (串口控制台产生, UI/封面模块消费) */
typedef enum {
    APP_CMD_BT_SCAN       = 0,
    APP_CMD_BT_CONNECT    = 1,
    APP_CMD_BT_DISCONNECT = 2,
    APP_CMD_PLAY          = 3,
    APP_CMD_STOP          = 4,
    APP_CMD_PAUSE         = 5,
    APP_CMD_INFO          = 6,
    APP_CMD_COVER_READY   = 7,
} app_cmd_type_t;

typedef struct {
    app_cmd_type_t type;
    char           param[32];
} app_cmd_t;

/* 串口控制台初始化 (任务创建, 读 STDIN 解析命令) */
void console_init(QueueHandle_t app_cmd_queue);

#ifdef __cplusplus
}
#endif

#endif
