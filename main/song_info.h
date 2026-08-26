#ifndef __SONG_INFO_H__
#define __SONG_INFO_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SONG_TITLE_MAX   128
#define SONG_ARTIST_MAX  128
#define SONG_FORMAT_MAX  16

/*
 * 歌曲信息: 解码任务写入, LVGL 只读。
 * 写入顺序: 先填充各字段, 最后置 g_song_info_valid = true;
 * 读取顺序: 先读 g_song_info_valid, 为 true 后再读 g_song_info。
 */
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

#ifdef __cplusplus
}
#endif

#endif
