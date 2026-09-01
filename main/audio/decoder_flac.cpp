#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "micro_flac/flac_decoder.h"
#include "audio.h"

#define FLAC_TAG "DEC_FLAC"

#define FLAC_INPUT_CHUNK_SIZE       8192
#define FLAC_MAX_PICTURE_SIZE       (512 * 1024)
#define FLAC_MAX_COMMENT_SIZE       4096
#define FLAC_MAX_SEEKTABLE_SIZE     16384
#define FLAC_SERVE_FRAMES           1152   /* 每次最多交给 pipeline 的帧数(与 MAX_IN_FRAMES 一致) */

#define COVER_MAX_SIZE              (512 * 1024)

typedef struct {
    uint64_t sample;
    uint64_t offset;
} flac_seekpt_t;

typedef struct {
    audio_decoder_t iface;

    FILE                     *file;
    micro_flac::FLACDecoder  *flac;
    uint8_t                  *inbuf;
    size_t                    in_off;
    size_t                    in_len;
    bool                      eof;
    bool                      header_done;

    int32_t                  *out32;         /* 解码帧输出 (PSRAM), 交错 int32 左对齐 */
    size_t                    out_cap;       /* out32 容量 = max_block_size * channels */
    size_t                    out_remaining; /* 待输出的交错样本数 */
    size_t                    out_served;    /* out32 已消费偏移 */

    uint32_t                  sample_rate;
    uint8_t                   channels;
    uint8_t                   bits_per_sample;
    uint64_t                  total_samples; /* 每声道总样本数 */
    uint32_t                  file_size;

    char                      title[SONG_TITLE_MAX];
    char                      artist[SONG_ARTIST_MAX];

    uint8_t                  *cover_data;    /* PICTURE 图片字节 (PSRAM), 未移交时由 close 释放 */
    size_t                    cover_size;

    flac_seekpt_t            *seekpts;       /* 解析后的 seektable (PSRAM) */
    uint16_t                  seek_count;

    uint64_t                  skip_remaining; /* 跳过的交错样本数 (seek 用) */
    uint32_t                  audio_start;    /* 音频数据起始字节偏移 (头部结束处) */
    bool                      seek_resync;    /* seek 后需要重同步到下一帧 */
    uint32_t                  resync_budget;  /* 重同步最大扫描字节数 */

    /* 重同步耗时调试 */
    int64_t                   resync_t0_us;
    uint32_t                  resync_iters;
    uint32_t                  resync_skips;
} decoder_flac_t;

/* ── 字节序工具 ── */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static uint32_t le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── Vorbis 注释解析 (原生 FLAC 内为小端) ── */
static void parse_vorbis_comments(const uint8_t *data, size_t len,
                                  char *title, size_t title_size,
                                  char *artist, size_t artist_size)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;

    if (end - p < 4) return;
    uint32_t vlen = le32(p); p += 4;
    if (vlen > (uint32_t)(end - p)) return;
    p += vlen;

    if (end - p < 4) return;
    uint32_t count = le32(p); p += 4;

    for (uint32_t i = 0; i < count; i++) {
        if (end - p < 4) break;
        uint32_t clen = le32(p); p += 4;
        if (clen > (uint32_t)(end - p)) break;

        const uint8_t *eq = (const uint8_t *)memchr(p, '=', clen);
        if (eq) {
            size_t klen = (size_t)(eq - p);
            const uint8_t *val = eq + 1;
            size_t vlen2 = clen - klen - 1;
            char key[16];
            size_t kl = klen < sizeof(key) - 1 ? klen : sizeof(key) - 1;
            memcpy(key, p, kl);
            key[kl] = '\0';
            for (size_t j = 0; key[j]; j++) {
                if (key[j] >= 'A' && key[j] <= 'Z') key[j] += ('a' - 'A');
            }

            if (strcmp(key, "title") == 0 && vlen2 > 0) {
                size_t n = vlen2 < title_size - 1 ? vlen2 : title_size - 1;
                memcpy(title, val, n);
                title[n] = '\0';
                printf("[FLAC] 标题: %s\n", title);
            } else if (strcmp(key, "artist") == 0 && vlen2 > 0) {
                size_t n = vlen2 < artist_size - 1 ? vlen2 : artist_size - 1;
                memcpy(artist, val, n);
                artist[n] = '\0';
                printf("[FLAC] 艺术家: %s\n", artist);
            } else if (strcmp(key, "album") == 0 && vlen2 > 0) {
                char buf[96];
                size_t n = vlen2 < sizeof(buf) - 1 ? vlen2 : sizeof(buf) - 1;
                memcpy(buf, val, n);
                buf[n] = '\0';
                printf("[FLAC] 专辑: %s\n", buf);
            }
        }
        p += clen;
    }
}

/* ── FLAC PICTURE 块解析 (大端), 提取图片字节到 PSRAM ── */
static void parse_picture_block(const uint8_t *data, size_t len,
                                uint8_t **cover_out, size_t *cover_size_out)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;

    if (end - p < 4) return;
    p += 4;                              /* picture type */
    if (end - p < 4) return;
    uint32_t mime_len = be32(p); p += 4;
    if (mime_len > (uint32_t)(end - p)) return;
    const uint8_t *mime = p; p += mime_len;
    if (end - p < 4) return;
    uint32_t desc_len = be32(p); p += 4;
    if (desc_len > (uint32_t)(end - p)) return;
    p += desc_len;
    p += 16;                             /* width/height/depth/colors */
    if (end - p < 4) return;
    uint32_t img_len = be32(p); p += 4;
    if (img_len > (uint32_t)(end - p)) return;

    char mime_buf[32] = {0};
    size_t ml = mime_len < sizeof(mime_buf) - 1 ? mime_len : sizeof(mime_buf) - 1;
    memcpy(mime_buf, mime, ml);
    printf("[FLAC] 封面: MIME=%s, 大小=%" PRIu32 " bytes\n", mime_buf, img_len);

    bool is_jpeg = (img_len >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF);
    if (!is_jpeg) {
        printf("[FLAC] 封面非 JPEG, 跳过 (当前封面模块仅支持 JPEG)\n");
        return;
    }
    if (img_len > COVER_MAX_SIZE) {
        printf("[FLAC] 封面过大 %" PRIu32 " bytes, 跳过\n", img_len);
        return;
    }
    if (!cover_out || *cover_out != NULL) return;

    uint8_t *buf = (uint8_t *)heap_caps_malloc(img_len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        printf("[FLAC] 封面 PSRAM 分配失败\n");
        return;
    }
    memcpy(buf, p, img_len);
    *cover_out = buf;
    if (cover_size_out) *cover_size_out = img_len;
    printf("[FLAC] 封面字节已提取到 PSRAM: %" PRIu32 " bytes\n", img_len);
}

/* ── seektable 解析 (大端) ── */
static void parse_seektable(const uint8_t *data, size_t len, decoder_flac_t *d)
{
    if (len < 18) return;
    size_t count = len / 18;
    if (count > 2048) count = 2048;
    flac_seekpt_t *pts = (flac_seekpt_t *)heap_caps_malloc(count * sizeof(flac_seekpt_t),
                                                            MALLOC_CAP_SPIRAM);
    if (!pts) return;

    uint16_t n = 0;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *s = data + i * 18;
        uint64_t sample = be64(s);
        uint64_t offset = be64(s + 8);
        if (sample == UINT64_MAX) continue;   /* 占位 seekpoint */
        pts[n].sample = sample;
        pts[n].offset = offset;
        n++;
    }
    if (n == 0) {
        heap_caps_free(pts);
        return;
    }
    d->seekpts = pts;
    d->seek_count = n;
    printf("[FLAC] seektable: %u 个 seekpoint\n", n);
}

/* 释放上次残留资源 (防止重复 open 泄漏) */
static void flac_free_state(decoder_flac_t *d)
{
    if (d->file) { fclose(d->file); d->file = NULL; }
    if (d->flac) { delete d->flac; d->flac = NULL; }
    if (d->inbuf) { heap_caps_free(d->inbuf); d->inbuf = NULL; }
    if (d->out32) { heap_caps_free(d->out32); d->out32 = NULL; }
    if (d->cover_data) { heap_caps_free(d->cover_data); d->cover_data = NULL; d->cover_size = 0; }
    if (d->seekpts) { heap_caps_free(d->seekpts); d->seekpts = NULL; d->seek_count = 0; }
    d->in_off = 0;
    d->in_len = 0;
    d->out_cap = 0;
    d->out_remaining = 0;
    d->out_served = 0;
    d->skip_remaining = 0;
    d->audio_start = 0;
    d->seek_resync = false;
    d->resync_budget = 0;
    d->eof = false;
    d->header_done = false;
}

/* 喂数据直到 HEADER_READY; 成功返回 true */
static bool flac_parse_header(decoder_flac_t *d)
{
    int guard = 0;
    while (guard++ < 64) {
        if (d->in_len == 0) {
            d->in_len = fread(d->inbuf, 1, FLAC_INPUT_CHUNK_SIZE, d->file);
            if (d->in_len == 0) return false;
            d->in_off = 0;
        }
        size_t consumed = 0, samples = 0;
        auto res = d->flac->decode(d->inbuf + d->in_off, d->in_len, (int32_t *)NULL, 0,
                                   consumed, samples);
        d->in_off += consumed;
        d->in_len -= consumed;
        if (d->in_len == 0) d->in_off = 0;

        if (res == micro_flac::FLAC_DECODER_HEADER_READY) return true;
        if (res == micro_flac::FLAC_DECODER_NEED_MORE_DATA) continue;
        ESP_LOGW(FLAC_TAG, "头部解析失败: %d", (int)res);
        return false;
    }
    return false;
}

static bool flac_open(audio_decoder_t *iface, const char *path)
{
    decoder_flac_t *d = (decoder_flac_t *)iface;

    flac_free_state(d);

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    d->inbuf = (uint8_t *)heap_caps_malloc(FLAC_INPUT_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!d->inbuf) {
        fclose(f);
        return false;
    }

    micro_flac::FLACDecoder *flac = new micro_flac::FLACDecoder();
    if (!flac) {
        heap_caps_free(d->inbuf);
        d->inbuf = NULL;
        fclose(f);
        return false;
    }
    flac->set_max_metadata_size(micro_flac::FLAC_METADATA_TYPE_VORBIS_COMMENT,
                                FLAC_MAX_COMMENT_SIZE);
    flac->set_max_metadata_size(micro_flac::FLAC_METADATA_TYPE_PICTURE,
                                FLAC_MAX_PICTURE_SIZE);
    flac->set_max_metadata_size(micro_flac::FLAC_METADATA_TYPE_SEEKTABLE,
                                FLAC_MAX_SEEKTABLE_SIZE);
    flac->set_crc_check_enabled(true);

    d->file = f;
    d->flac = flac;
    d->eof = false;

    {
        struct stat st;
        if (stat(path, &st) == 0) d->file_size = (uint32_t)st.st_size;
    }

    if (!flac_parse_header(d)) {
        ESP_LOGW(FLAC_TAG, "打开失败 (无有效 FLAC 头): %s", path);
        flac_free_state(d);
        return false;
    }
    d->header_done = true;
    /* 头部已读完, 当前文件游标 - 缓冲内未消费字节 = 音频数据起点 */
    d->audio_start = (uint32_t)(ftell(d->file) - (long)d->in_len);

    const micro_flac::FLACStreamInfo &info = flac->get_stream_info();
    d->sample_rate = info.sample_rate();
    d->channels    = (uint8_t)info.num_channels();
    d->bits_per_sample = (uint8_t)info.bits_per_sample();
    d->total_samples = info.total_samples_per_channel();

    printf("[FLAC] 流信息: %" PRIu32 " Hz, %u ch, %u bit, total=%" PRIu64
           " (%.1f s)\n",
           d->sample_rate, d->channels, d->bits_per_sample, d->total_samples,
           d->sample_rate ? (double)d->total_samples / d->sample_rate : 0.0);

    d->out_cap = (size_t)info.max_block_size() * d->channels;
    d->out32 = (int32_t *)heap_caps_malloc(d->out_cap * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (!d->out32) {
        ESP_LOGW(FLAC_TAG, "输出缓冲分配失败 (%u samples)", (unsigned)d->out_cap);
        flac_free_state(d);
        return false;
    }

    /* 元数据: 注释/封面/seektable */
    const micro_flac::FLACMetadataBlock *vc =
        flac->get_metadata_block(micro_flac::FLAC_METADATA_TYPE_VORBIS_COMMENT);
    if (vc) {
        parse_vorbis_comments(vc->data, vc->length, d->title, sizeof(d->title),
                              d->artist, sizeof(d->artist));
    }
    const micro_flac::FLACMetadataBlock *pic =
        flac->get_metadata_block(micro_flac::FLAC_METADATA_TYPE_PICTURE);
    if (pic) {
        parse_picture_block(pic->data, pic->length, &d->cover_data, &d->cover_size);
    }
    const micro_flac::FLACMetadataBlock *st =
        flac->get_metadata_block(micro_flac::FLAC_METADATA_TYPE_SEEKTABLE);
    if (st) {
        parse_seektable(st->data, st->length, d);
    }

    printf("[FLAC] 已加载: %s\n", path);
    return true;
}

static bool flac_decode_frame(audio_decoder_t *iface, int16_t *pcm, size_t *bytes)
{
    decoder_flac_t *d = (decoder_flac_t *)iface;

    if (!d->file || !d->flac || !d->out32) return false;

    while (1) {
        /* 有解码好的帧 → 先喂给调用方 */
        if (d->out_remaining > 0) {
            size_t max_samples = FLAC_SERVE_FRAMES * d->channels;
            size_t n = d->out_remaining < max_samples ? d->out_remaining : max_samples;

            if (d->skip_remaining > 0) {
                size_t skip = d->skip_remaining < n ? d->skip_remaining : n;
                d->out_served += skip;
                d->out_remaining -= skip;
                d->skip_remaining -= skip;
                if (d->out_remaining == 0) d->out_served = 0;
                continue;
            }

            uint32_t bits = d->bits_per_sample;
            int shift = (bits == 32) ? 16 : (int)(32 - bits);
            const int32_t *src = d->out32 + d->out_served;
            for (size_t i = 0; i < n; i++) {
                pcm[i] = (int16_t)(src[i] >> shift);
            }
            d->out_served += n;
            d->out_remaining -= n;
            if (d->out_remaining == 0) d->out_served = 0;
            *bytes = n * sizeof(int16_t);
            return true;
        }

        /* 解码新帧 */
        if (d->in_len == 0) {
            d->in_len = fread(d->inbuf, 1, FLAC_INPUT_CHUNK_SIZE, d->file);
            if (d->in_len == 0) { d->eof = true; return false; }
            d->in_off = 0;
        }

        size_t consumed = 0, samples = 0;
        int64_t t0 = d->seek_resync ? esp_timer_get_time() : 0;
        auto res = d->flac->decode(d->inbuf + d->in_off, d->in_len,
                                   d->out32, d->out_cap, consumed, samples);

        if (res == micro_flac::FLAC_DECODER_SUCCESS) {
            /* 成功: consumed 可信, 前进保留帧尾 */
            if (consumed > d->in_len) consumed = d->in_len;
            d->in_off += consumed;
            d->in_len -= consumed;
            if (d->in_len == 0) d->in_off = 0;
            if (d->seek_resync) {
                int64_t total = esp_timer_get_time() - d->resync_t0_us;
                printf("[FLAC] 重同步完成: %" PRIu32 " 次尝试, 整块跳过 %" PRIu32
                       ", 耗时 %lld ms, 到偏移 %lld\n",
                       d->resync_iters, d->resync_skips, total / 1000,
                       (long long)ftell(d->file));
            }
            d->seek_resync = false;   /* 已同步到有效帧 */
            d->out_remaining = samples;
            d->out_served = 0;
            continue;
        }
        if (res == micro_flac::FLAC_DECODER_NEED_MORE_DATA) {
            /* 顺序续帧: consumed == in_len */
            if (consumed > d->in_len) consumed = d->in_len;
            d->in_off += consumed;
            d->in_len -= consumed;
            if (d->in_len == 0) d->in_off = 0;
            continue;
        }
        if (res == micro_flac::FLAC_DECODER_END_OF_STREAM) { d->eof = true; return false; }

        /* 音频阶段错误: 不信任 consumed (错误路径 buffer_index_ 可能残留旧值),
         * 跳到下一个 0xFF 0xF8 帧同步候选重试 (micro-flac 帧状态已复位) */
        if (d->seek_resync && d->resync_budget > 0) {
            d->resync_budget--;
            d->resync_iters++;
            if (t0) {
                int64_t dt = esp_timer_get_time() - t0;
                if (dt > 2000) {
                    printf("[FLAC] 重同步慢迭代 #%" PRIu32 " (偏移 %u, len %u): %lld us\n",
                           d->resync_iters, (unsigned)d->in_off, (unsigned)d->in_len, dt);
                }
            }
            size_t j = 1;
            const uint8_t *b = d->inbuf + d->in_off;
            while (j + 1 < d->in_len &&
                   !(b[j] == 0xFF && (b[j + 1] & 0xFE) == 0xF8)) {
                j++;
            }
            if (j + 1 < d->in_len) {
                d->in_off += j;
                d->in_len -= j;
            } else {
                /* 整块缓冲内无 0xFF 0xF8 候选 → 整块跳过, 读下一块 (避免逐字节空转) */
                d->resync_skips++;
                d->in_off = 0;
                d->in_len = 0;
            }
            continue;
        }
        d->seek_resync = false;
        ESP_LOGW(FLAC_TAG, "解码错误: %d", (int)res);
        d->eof = true;
        return false;
    }
}

static bool flac_is_eof(audio_decoder_t *iface)
{
    return ((decoder_flac_t *)iface)->eof;
}

static void flac_close(audio_decoder_t *iface)
{
    flac_free_state((decoder_flac_t *)iface);
}

static uint32_t flac_get_sample_rate(audio_decoder_t *iface)
{
    return ((decoder_flac_t *)iface)->sample_rate;
}

static uint8_t flac_get_channels(audio_decoder_t *iface)
{
    return ((decoder_flac_t *)iface)->channels;
}

static uint8_t flac_get_bits(audio_decoder_t *iface)
{
    return ((decoder_flac_t *)iface)->bits_per_sample;
}

/* 平均码率: file_size*8 / 时长 / 1000 */
static uint32_t flac_get_bitrate(audio_decoder_t *iface)
{
    decoder_flac_t *d = (decoder_flac_t *)iface;
    if (!d->sample_rate) return 0;
    uint32_t dur = (uint32_t)(d->total_samples / d->sample_rate);
    if (dur == 0) return 0;
    return (uint32_t)(((uint64_t)d->file_size * 8) / dur / 1000);
}

static uint32_t flac_get_file_size(audio_decoder_t *iface)
{
    return ((decoder_flac_t *)iface)->file_size;
}

static uint32_t flac_get_position(audio_decoder_t *iface)
{
    decoder_flac_t *d = (decoder_flac_t *)iface;
    if (!d->file) return 0;
    long pos = ftell(d->file);
    return pos > 0 ? (uint32_t)pos : 0;
}

static bool flac_seek(audio_decoder_t *iface, uint32_t byte_offset)
{
    decoder_flac_t *d = (decoder_flac_t *)iface;
    if (!d->file || !d->flac || d->total_samples == 0) return false;

    uint32_t fsz = d->file_size ? d->file_size : 1;
    uint64_t target = ((uint64_t)byte_offset * d->total_samples) / fsz;
    if (target >= d->total_samples) target = d->total_samples - 1;

    d->out_remaining = 0;
    d->out_served = 0;
    d->in_off = 0;
    d->in_len = 0;
    d->eof = false;

    /* 1) seektable 快速路径: 找 <= target 的最大 seekpoint, 直接跳到帧边界 */
    bool found = false;
    flac_seekpt_t entry = {0, 0};
    if (d->seekpts && d->seek_count > 0) {
        for (int i = d->seek_count - 1; i >= 0; i--) {
            if (d->seekpts[i].sample <= target) {
                entry = d->seekpts[i];
                found = true;
                break;
            }
        }
    }

    if (found) {
        if (fseek(d->file, (long)entry.offset, SEEK_SET) != 0) return false;
        d->skip_remaining = (target - entry.sample) * d->channels;
        d->seek_resync = false;
        printf("[FLAC] SEEK -> seekpoint #%" PRIu64 " (偏移 %" PRIu64 "), 再跳 %" PRIu64
               " samples\n", entry.sample, entry.offset, d->skip_remaining);
        return true;
    }

    /* 2) 无 seektable: 按文件字节比例定位 + 解码器重同步到下一有效帧 */
    uint32_t pos = byte_offset;
    if (pos < d->audio_start) pos = d->audio_start;
    if (pos >= fsz) pos = fsz ? fsz - 1 : 0;
    if (fseek(d->file, (long)pos, SEEK_SET) != 0) return false;

    d->skip_remaining = (uint64_t)(d->out_cap / (d->channels ? d->channels : 1)) * d->channels;
    d->seek_resync = true;
    d->resync_budget = 65536;
    d->resync_t0_us = esp_timer_get_time();
    d->resync_iters = 0;
    d->resync_skips = 0;
    printf("[FLAC] SEEK (重同步) pos=%" PRIu32 ", target=%" PRIu64 " samples\n", pos, target);
    return true;
}

static const char *flac_get_title(audio_decoder_t *iface)
{
    return ((decoder_flac_t *)iface)->title;
}

static const char *flac_get_artist(audio_decoder_t *iface)
{
    return ((decoder_flac_t *)iface)->artist;
}

static const uint8_t *flac_get_cover_data(audio_decoder_t *iface)
{
    return ((decoder_flac_t *)iface)->cover_data;
}

static size_t flac_get_cover_size(audio_decoder_t *iface)
{
    return ((decoder_flac_t *)iface)->cover_size;
}

static void flac_take_cover(audio_decoder_t *iface)
{
    decoder_flac_t *d = (decoder_flac_t *)iface;
    d->cover_data = NULL;
    d->cover_size = 0;
}

audio_decoder_t *decoder_flac_create(void)
{
    decoder_flac_t *d = (decoder_flac_t *)calloc(1, sizeof(decoder_flac_t));
    if (!d) return NULL;

    d->iface.open            = flac_open;
    d->iface.decode          = flac_decode_frame;
    d->iface.is_eof          = flac_is_eof;
    d->iface.close           = flac_close;
    d->iface.get_sample_rate = flac_get_sample_rate;
    d->iface.get_channels    = flac_get_channels;
    d->iface.get_bits        = flac_get_bits;
    d->iface.get_bitrate     = flac_get_bitrate;
    d->iface.get_file_size   = flac_get_file_size;
    d->iface.get_position    = flac_get_position;
    d->iface.seek            = flac_seek;
    d->iface.get_title       = flac_get_title;
    d->iface.get_artist      = flac_get_artist;
    d->iface.get_cover_data  = flac_get_cover_data;
    d->iface.get_cover_size  = flac_get_cover_size;
    d->iface.take_cover      = flac_take_cover;

    return &d->iface;
}
