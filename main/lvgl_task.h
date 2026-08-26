#ifndef __LVGL_TASK_H__
#define __LVGL_TASK_H__

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "bt_a2dp.h"

typedef struct {
    QueueHandle_t        app_cmd_queue;
    bt_a2dp_iface_t     *bt_iface;
    QueueHandle_t        audio_cmd_queue;
    QueueHandle_t        audio_rsp_queue;
} lvgl_task_params_t;

void lvgl_task_init(const lvgl_task_params_t *params);

/* 当前播放信息: 供文件浏览器定位正在播放的歌曲 (无歌曲时返回 NULL) */
const char *player_current_group(void);
const char *player_current_name(void);

#endif
