#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 本地音频输出 (I2S → ES8311 → 板载喇叭).
 * ESP32-S3 不支持经典蓝牙, 原 A2DP 发射改为本地播放.
 *
 * 用法: 先 audio_out_init() 取得 PCM 流缓冲, 解码任务把 44.1kHz/16bit/立体声
 * PCM 写入该流, 输出任务负责送到 ES8311. */

StreamBufferHandle_t audio_out_init(void);   /* 初始化 I2S + codec, 返回 PCM 流句柄 */

bool     audio_out_is_ready(void);           /* 输出设备是否就绪 */
int      audio_out_volume_get(void);         /* 当前输出音量 0~100 */
void     audio_out_volume_set(int volume);   /* 设置输出音量 0~100 */
void     audio_out_write(const int16_t *data, size_t samples);  /* 直接写 PCM (可选) */

#ifdef __cplusplus
}
#endif
