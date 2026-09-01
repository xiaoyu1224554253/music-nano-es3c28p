#ifndef __UI_PLAYER_H__
#define __UI_PLAYER_H__

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 创建播放器主界面 (ui_loop_task 内调用) */
void ui_player_init(void);

/* 播放控制: 文件浏览器选中 / 自动切歌 / 响应音频结果 */
void player_play_file(const char *group, const char *name);
void player_advance(void);
void player_on_song_finished(void);
void player_on_file_not_found(void);

/* 播放/暂停切换 (播放键 + 蓝牙耳机请求共用) */
void player_toggle_play(void);
void player_next(void);
void player_prev(void);

/* 封面: 解码任务就绪后回调 */
void player_show_cover(void *buf);

/* 播放影子状态 (供主循环分发使用) */
bool player_was_playing(void);
void player_set_was_playing(bool v);

/* 当前播放信息: 供文件浏览器定位正在播放的歌曲 (无歌曲时返回 NULL) */
const char *player_current_group(void);
const char *player_current_name(void);

#ifdef __cplusplus
}
#endif

#endif
