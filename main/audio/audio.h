#ifndef __AUDIO_H__
#define __AUDIO_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────── 歌曲信息 ────────────────────────────
 * 解码任务写入, LVGL 只读。
 * 写入顺序: 先填充各字段, 最后置 g_song_info_valid = true;
 * 读取顺序: 先读 g_song_info_valid, 为 true 后再读 g_song_info。
 */
#define SONG_TITLE_MAX   128
#define SONG_ARTIST_MAX  128
#define SONG_FORMAT_MAX  16

typedef struct {
    char     title[SONG_TITLE_MAX];
    char     artist[SONG_ARTIST_MAX];
    char     format[SONG_FORMAT_MAX];
    uint32_t sample_rate;
    uint8_t  channels;
    uint32_t bitrate_kbps;
    uint32_t duration_sec;
    uint32_t elapsed_sec;
} song_info_t;

extern song_info_t g_song_info;
extern volatile bool g_song_info_valid;

/* ──────────────────────── 解码器抽象接口 ──────────────────────── */
typedef struct audio_decoder_s {
    bool      (*open)(struct audio_decoder_s *self, const char *path);
    bool      (*decode)(struct audio_decoder_s *self, int16_t *pcm, size_t *bytes);
    bool      (*is_eof)(struct audio_decoder_s *self);
    void      (*close)(struct audio_decoder_s *self);
    uint32_t  (*get_sample_rate)(struct audio_decoder_s *self);
    uint8_t   (*get_channels)(struct audio_decoder_s *self);
    uint32_t  (*get_bitrate)(struct audio_decoder_s *self);
    uint32_t  (*get_file_size)(struct audio_decoder_s *self);
    uint32_t  (*get_position)(struct audio_decoder_s *self);
    bool      (*seek)(struct audio_decoder_s *self, uint32_t byte_offset);
    const char *(*get_title)(struct audio_decoder_s *self);
    const char *(*get_artist)(struct audio_decoder_s *self);
    const uint8_t *(*get_cover_data)(struct audio_decoder_s *self);
    size_t         (*get_cover_size)(struct audio_decoder_s *self);
    void           (*take_cover)(struct audio_decoder_s *self);
} audio_decoder_t;

audio_decoder_t *decoder_mp3_create(void);
audio_decoder_t *decoder_flac_create(void);

#ifdef __cplusplus
}
#endif

#endif
