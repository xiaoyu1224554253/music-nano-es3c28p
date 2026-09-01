#include "pcm_pipeline.h"
#include "impl/pcm_convert.h"
#include "resampler_fxp.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "esp_timer.h"

using namespace esp_audio_libs;

#define TARGET_RATE   44100
#define TARGET_BITS   16
#define TARGET_CH     2

/* 最坏输入: MPEG1 stereo 1152 帧/块 */
#define MAX_IN_FRAMES  1152

struct pcm_pipeline_s {
    resampler_fxp_t *res_fxp;

    bool     active;
    bool     need_resample;
    bool     need_upmix;

    uint32_t src_rate;
    uint8_t  src_bits;
    uint8_t  src_bps;
    uint8_t  src_ch;

    uint8_t *stereo_in;   /* mono -> stereo 上混临时缓冲 */

    int64_t  res_us;          /* 重采样累计耗时 (诊断) */
    int64_t  res_window_t0;   /* 统计窗口起点 */
    uint64_t res_frames;      /* 重采样累计输出帧数 (诊断) */
};

pcm_pipeline_t *pcm_pipeline_create(void)
{
    pcm_pipeline_t *p = (pcm_pipeline_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->stereo_in = (uint8_t *)malloc(MAX_IN_FRAMES * TARGET_CH * 4);
    if (!p->stereo_in) {
        free(p);
        return NULL;
    }
    return p;
}

void pcm_pipeline_close(pcm_pipeline_t *p)
{
    if (!p) return;
    if (p->res_fxp) {
        resampler_fxp_free(p->res_fxp);
        p->res_fxp = NULL;
    }
    p->active = false;
    p->need_resample = false;
    p->need_upmix = false;
}

void pcm_pipeline_free(pcm_pipeline_t *p)
{
    if (!p) return;
    pcm_pipeline_close(p);
    free(p->stereo_in);
    p->stereo_in = NULL;
    free(p);
}

bool pcm_pipeline_open(pcm_pipeline_t *p, uint32_t src_rate, uint8_t src_bits, uint8_t src_ch)
{
    if (!p) return false;

    pcm_pipeline_close(p);

    if (src_rate == 0 || src_bits == 0 || (src_ch != 1 && src_ch != 2)) {
        return false;
    }

    p->src_rate = src_rate;
    p->src_bits = src_bits;
    p->src_bps  = src_bits / 8;
    p->src_ch   = src_ch;

    p->need_upmix    = (src_ch == 1);
    p->need_resample = (src_rate != TARGET_RATE);

    if (p->need_resample) {
        p->res_fxp = resampler_fxp_create();
        if (!p->res_fxp || !resampler_fxp_open(p->res_fxp, src_rate, TARGET_CH)) {
            resampler_fxp_free(p->res_fxp);
            p->res_fxp = NULL;
            return false;
        }
    }

    p->active = true;
    p->res_us = 0;
    p->res_window_t0 = esp_timer_get_time();
    return true;
}

size_t pcm_pipeline_process(pcm_pipeline_t *p, const void *src, size_t src_frames,
                            uint8_t *dst, size_t dst_cap_bytes)
{
    if (!p || !p->active || !src || !dst) return 0;

    const uint8_t *in = (const uint8_t *)src;

    /* 1) mono -> stereo */
    if (p->need_upmix) {
        pcm_convert::copy_frames(in, p->stereo_in,
                                 p->src_bps, p->src_ch,
                                 TARGET_BITS / 8, TARGET_CH,
                                 (uint32_t)src_frames);
        in = p->stereo_in;
    }

    /* 2) 源速率 == 44100: 直接拷贝 */
    if (!p->need_resample) {
        size_t bytes = src_frames * TARGET_CH * (TARGET_BITS / 8);
        if (bytes > dst_cap_bytes) bytes = dst_cap_bytes;
        memcpy(dst, in, bytes);
        return bytes;
    }

    /* 3) 纯整数重采样到 44.1kHz */
    size_t out_frames_cap = dst_cap_bytes / (TARGET_CH * (TARGET_BITS / 8));
    int64_t t0 = esp_timer_get_time();
    size_t out_frames = resampler_fxp_process(p->res_fxp,
                                              (const int16_t *)in, src_frames,
                                              (int16_t *)dst, out_frames_cap);
    p->res_us += esp_timer_get_time() - t0;
    p->res_frames += out_frames;

    /* 每 10s 汇报重采样 CPU 占比 (诊断) */
    int64_t now = esp_timer_get_time();
    if (now - p->res_window_t0 > 10 * 1000000) {
        int64_t win = now - p->res_window_t0;
        printf("[PIPE] 重采样 %d ms / %u 帧 / %.1f s (%.1f%% core, %.0f ns/帧, 入%u帧/次)\n",
               (int)(p->res_us / 1000), (unsigned)p->res_frames, win / 1000000.0,
               (double)p->res_us * 100.0 / win,
               (double)p->res_us * 1000.0 / (p->res_frames ? p->res_frames : 1),
               (unsigned)src_frames);
        p->res_us = 0;
        p->res_frames = 0;
        p->res_window_t0 = now;
    }

    return out_frames * TARGET_CH * (TARGET_BITS / 8);
}
