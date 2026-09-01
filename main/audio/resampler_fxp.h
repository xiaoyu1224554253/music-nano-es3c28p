#ifndef __RESAMPLER_FXP_H__
#define __RESAMPLER_FXP_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 纯整数(定点)有理比率 polyphase 重采样器。
 *
 * 把任意源采样率重采样到 44100Hz, 输入/输出均为 int16 交错 PCM。
 * 比率 44100/src_rate 约分为 p/q, 每输出样本用 [p 个相位][taps 个抽头]
 * 的 Q15 整数系数做 int32/int64 点积, 全链路无浮点(ESP32 无 FPU)。
 */

typedef struct resampler_fxp_s resampler_fxp_t;

resampler_fxp_t *resampler_fxp_create(void);   /* 创建重采样器对象 */

/* 配置为 src_rate → 44100 重采样; channels 1 或 2。返回 false 表示分配失败。 */
bool resampler_fxp_open(resampler_fxp_t *r, uint32_t src_rate, uint8_t channels);

/* 重采样一块交错 int16 输入, 写入交错 int16 输出。
 * src=输入, src_frames=输入帧数, dst=输出, dst_cap_frames=输出缓冲可容纳帧数.
 * 返回实际输出帧数. */
size_t resampler_fxp_process(resampler_fxp_t *r, const int16_t *src, size_t src_frames,
                             int16_t *dst, size_t dst_cap_frames);

void resampler_fxp_close(resampler_fxp_t *r);   /* 释放系数等资源 */
void resampler_fxp_free(resampler_fxp_t *r);    /* 释放对象 */

#ifdef __cplusplus
}
#endif

#endif
