#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 设置持久化 (NVS, 统一 "player" 命名空间):
 * 音量 / 上次播放歌曲 / 播放模式
 */

/* ── 音量 ── */
#define VOLUME_MIN      0
#define VOLUME_MAX      127

int32_t volume_get(void);
void    volume_set(int32_t v);
void    volume_inc(int32_t delta);

void    volume_load_from_nvs(void);
void    volume_save_to_nvs(void);

/* ── 上次播放歌曲 ── */
void last_song_save(const char *path);
bool last_song_load(char *buf, size_t size);

/* ── 播放模式 ── */
typedef enum {
    PLAY_MODE_SEQUENTIAL = 0,   /* 顺序(到头回绕) */
    PLAY_MODE_SINGLE,           /* 单曲循环 */
    PLAY_MODE_RANDOM,           /* 随机 */
} play_mode_t;

bool settings_mode_load(play_mode_t *mode);
void settings_mode_save(play_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif
