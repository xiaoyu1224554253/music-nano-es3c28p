#pragma once

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

void cover_init(QueueHandle_t app_cmd_queue);   /* 启动封面解码任务: app_cmd_queue=完成通知队列 */
void cover_submit_job(const uint8_t *jpg, size_t size);   /* 提交封面解码作业: jpg=JPEG数据, size=数据大小 */

/* 通知 UI: 当前歌曲无内嵌封面, 回退默认图标 */
void cover_notify_no_cover(void);

#ifdef __cplusplus
}
#endif
