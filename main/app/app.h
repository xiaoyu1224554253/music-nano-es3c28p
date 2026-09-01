#ifndef __APP_H__
#define __APP_H__

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 应用级命令 (串口控制台产生, UI/封面模块消费).
 * 命令统一走 app_cmd_queue 队列, 实现生产者(控制台)/消费者(UI)解耦. */
typedef enum {
    APP_CMD_BT_SCAN       = 0,  /* 开始蓝牙扫描 */
    APP_CMD_BT_CONNECT    = 1,  /* 连接指定设备, param=设备名 */
    APP_CMD_BT_DISCONNECT = 2,  /* 断开当前蓝牙连接 */
    APP_CMD_PLAY          = 3,  /* 播放 (param=路径) */
    APP_CMD_STOP          = 4,  /* 停止播放 */
    APP_CMD_PAUSE         = 5,  /* 暂停播放 */
    APP_CMD_INFO          = 6,  /* 查询当前状态/信息 */
    APP_CMD_COVER_READY   = 7,  /* 封面解码完成, 通知 UI 刷新 (由封面任务发出) */
} app_cmd_type_t;

/* 应用命令结构体: type=命令类型, param=可选参数 (设备名/文件路径等) */
typedef struct {
    app_cmd_type_t type;
    char           param[32];
} app_cmd_t;

/* 串口控制台初始化 (任务创建, 读 STDIN 解析命令).
 * app_cmd_queue: 解析出的命令要投递到的应用命令队列 */
void console_init(QueueHandle_t app_cmd_queue);

#ifdef __cplusplus
}
#endif

#endif
