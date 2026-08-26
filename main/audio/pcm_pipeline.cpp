#include "pcm_pipeline.h"
#include "resampler.h"
#include "pcm_convert.h"

#include <cstring>
#include <cstdlib>

using namespace esp_audio_libs;

#define TARGET_RATE   44100.0f
#define TARGET_BITS   16
#define TARGET_CH     2

/* 最坏输入: MPEG1 stereo 1152 帧/块 */
#define MAX_IN_FRAMES  1152
/* 最坏输出: 8kHz MPEG2.5 mono (576 帧) -> 44100/8000≈5.5x ≈ 3175 帧, 取 4096 */
#define MAX_OUT_FRAMES 4096

struct pcm_pipeline_s {
    resampler::Resampler *resampler;

    bool     active;
    bool     need_resample;
    bool     need_upmix;

    uint32_t src_rate;
    uint8_t  src_bits;
    uint8_t  src_bps;
    uint8_t  src_ch;

    uint8_t *stereo_in;   /* mono -> stereo 上混临时缓冲 */
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
    if (p->resampler) {
        delete p->resampler;
        p->resampler = NULL;
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
    p->need_resample = (src_rate != (uint32_t)TARGET_RATE);

    if (p->need_resample) {
        resampler::ResamplerConfiguration cfg;
        cfg.source_sample_rate      = (float)src_rate;
        cfg.target_sample_rate      = TARGET_RATE;
        cfg.source_bits_per_sample  = src_bits;
        cfg.target_bits_per_sample  = TARGET_BITS;
        cfg.channels                = TARGET_CH;   /* 上混后恒为 stereo */
        cfg.use_pre_or_post_filter  = true;        /* 降采样抗混叠 */
        cfg.subsample_interpolate   = false;
        cfg.number_of_taps          = 32;
        cfg.number_of_filters       = 8;

        p->resampler = new resampler::Resampler(MAX_IN_FRAMES * TARGET_CH,
                                                MAX_OUT_FRAMES * TARGET_CH);
        if (!p->resampler || !p->resampler->initialize(cfg)) {
            delete p->resampler;
            p->resampler = NULL;
            return false;
        }
    }

    p->active = true;
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

    /* 2) 源速率 == 44100: 直接 (必要时做位深转换) */
    if (!p->need_resample) {
        if (p->src_bits == TARGET_BITS) {
            size_t bytes = src_frames * TARGET_CH * (TARGET_BITS / 8);
            if (bytes > dst_cap_bytes) bytes = dst_cap_bytes;
            memcpy(dst, in, bytes);
            return bytes;
        }
        pcm_convert::copy_frames(in, dst,
                                 p->src_bps, TARGET_CH,
                                 TARGET_BITS / 8, TARGET_CH,
                                 (uint32_t)src_frames);
        return src_frames * TARGET_CH * (TARGET_BITS / 8);
    }

    /* 3) 重采样到 44.1kHz */
    size_t out_frames_cap = dst_cap_bytes / (TARGET_CH * (TARGET_BITS / 8));
    resampler::ResamplerResults res =
        p->resampler->resample(in, dst, src_frames, out_frames_cap, 0.0f);

    return res.frames_generated * TARGET_CH * (TARGET_BITS / 8);
}
