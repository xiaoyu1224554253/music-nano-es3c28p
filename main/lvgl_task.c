#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lcd_jd9853.h"
#include "touch_cst816.h"
#include "lvgl.h"
#include "bt_a2dp.h"
#include "sys_serial.h"
#include "audio_task.h"
#include "lvgl_task.h"
#include "sys_monitor.h"
#include "ui_list.h"
#include "song_info.h"
#include "last_song.h"
#include "volume.h"
#include <dirent.h>
#include "esp_heap_caps.h"
#include "File.h"

extern const lv_font_t chinese_16;

#define LVGL_TAG "LVGL"

#define TFT_HOR_RES   172
#define TFT_VER_RES   320
#define DRAW_BUF_SIZE  (TFT_HOR_RES * TFT_VER_RES / 3)
#define LVGL_BUF_SIZE  (DRAW_BUF_SIZE / 2)

#define TOUCH_Y_MIN  5
#define TOUCH_Y_MAX  310

#define COLOR_BG      lv_color_hex(0x050505)
#define COLOR_CARD    lv_color_hex(0x111111)
#define COLOR_FG      lv_color_hex(0xF0F0F0)
#define COLOR_MUTED   lv_color_hex(0x888888)
#define COLOR_ACCENT  lv_color_hex(0x00D992)
#define COLOR_BORDER  lv_color_hex(0x2A2A2A)
#define COLOR_DIM     lv_color_hex(0x555555)

static QueueHandle_t        s_app_cmd_queue  = NULL;
static bt_a2dp_iface_t     *s_bt_iface       = NULL;
static QueueHandle_t        s_audio_cmd_queue = NULL;
static QueueHandle_t        s_audio_rsp_queue = NULL;
static QueueSetHandle_t     s_queue_set       = NULL;
static bool                 s_was_playing     = false;

static lv_color_t           s_draw_buf1[LVGL_BUF_SIZE];
static lv_color_t           s_draw_buf2[LVGL_BUF_SIZE];
static lv_disp_draw_buf_t   s_draw_buf_dsc;
static lv_disp_drv_t        s_disp_drv;
static esp_lcd_panel_handle_t s_panel = NULL;

static lv_obj_t *s_status_label;
static lv_obj_t *s_title_label;
static lv_obj_t *s_artist_label;
static lv_obj_t *s_progress_bar;
static lv_obj_t *s_time_current;
static lv_obj_t *s_time_total;
static lv_obj_t *s_fmt_val;
static lv_obj_t *s_sr_val;
static lv_obj_t *s_bd_val;
static lv_obj_t *s_album_art;
static lv_obj_t *s_album_img;
static lv_obj_t *s_album_icon;
static lv_img_dsc_t s_cover_dsc;
static lv_obj_t *s_play_icon;
static lv_obj_t *s_mode_icon;

/* 音量弹窗: (140,45) 30x110, 变化时显示, 2秒无变化隐藏 */
#define VOL_POP_X       140
#define VOL_POP_Y       45
#define VOL_POP_W       30
#define VOL_POP_H       110
#define VOL_POP_BAR_H   90
#define VOL_POP_HIDE_MS 2000

static lv_obj_t *s_vol_cont = NULL;
static lv_obj_t *s_vol_bar  = NULL;
static lv_obj_t *s_vol_val  = NULL;
static int32_t   s_vol_last_ui = -1;
static int       s_vol_idle = 0;

static void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                          lv_color_t *color_p)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, color_p);
    lv_disp_flush_ready(disp_drv);
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t tx, ty;
    if (touch_read(&tx, &ty)) {
        //printf("%d, %d\n", tx, ty);
        ty = (uint16_t)(((int)ty - TOUCH_Y_MIN) * (TFT_VER_RES - 1)
                        / (TOUCH_Y_MAX - TOUCH_Y_MIN));
        tx = TFT_HOR_RES - 1 - tx;
        ty = TFT_VER_RES - 1 - ty;
        if (tx >= TFT_HOR_RES) tx = TFT_HOR_RES - 1;
        if (ty >= TFT_VER_RES) ty = TFT_VER_RES - 1;
        data->point.x = tx;
        data->point.y = ty;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static lv_obj_t *make_icon_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t w, lv_coord_t h,
                                const char *symbol)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, COLOR_MUTED, 0);
    lv_obj_center(icon);

    return btn;
}

/* ── 播放列表: 当前文件夹联动 (仅读内存缓存 g_fs_cache, 不碰 SD) ── */
static char s_pl_group[FS_GROUP_MAX];
static int  s_pl_index = 0;
static int  s_pl_count = 0;

/* ── 播放模式与命令节流 ── */
typedef enum {
    PLAY_MODE_SEQUENTIAL = 0,   /* 顺序(到头回绕) */
    PLAY_MODE_SINGLE,           /* 单曲循环 */
    PLAY_MODE_RANDOM,           /* 随机 */
} play_mode_t;

static play_mode_t s_play_mode = PLAY_MODE_SEQUENTIAL;
static bool        s_auto_advancing = false;
static int64_t     s_last_user_cmd_us = 0;

#define CMD_MIN_INTERVAL_US  500000

static int player_count_files(const char *group)
{
    if (!g_fs_cache || !group) return 0;
    int count = 0;
    for (int i = 0; i < g_fs_cache->count; i++) {
        fs_entry_t *e = &g_fs_cache->entries[i];
        if (!e->is_dir && strcmp(e->group, group) == 0) count++;
    }
    return count;
}

static const char *player_file_name_at(const char *group, int idx)
{
    if (!g_fs_cache || !group) return NULL;
    int seen = 0;
    for (int i = 0; i < g_fs_cache->count; i++) {
        fs_entry_t *e = &g_fs_cache->entries[i];
        if (e->is_dir || strcmp(e->group, group) != 0) continue;
        if (seen == idx) return e->name;
        seen++;
    }
    return NULL;
}

static void player_update_label(void)
{
    char buf[32];
    if (s_pl_count > 0) {
        snprintf(buf, sizeof(buf), "%d/%d", s_pl_index + 1, s_pl_count);
    } else {
        snprintf(buf, sizeof(buf), "PLAYER");
    }
    lv_label_set_text(s_status_label, buf);
}

/* 当前播放信息: 供文件浏览器打开时定位正在播放的歌曲 */
const char *player_current_group(void)
{
    if (s_pl_count <= 0) return NULL;
    return s_pl_group;
}

const char *player_current_name(void)
{
    if (s_pl_count <= 0) return NULL;
    return player_file_name_at(s_pl_group, s_pl_index);
}

/* 用户命令发送: 与上一条间隔 < 500ms 则丢弃 */
static bool audio_user_send(const audio_cmd_t *cmd)
{
    int64_t now = esp_timer_get_time();
    if (s_last_user_cmd_us != 0 && (now - s_last_user_cmd_us) < CMD_MIN_INTERVAL_US) {
        printf("[LVGL] 命令间隔<500ms, 忽略\n");
        return false;
    }
    s_last_user_cmd_us = now;
    xQueueSend(s_audio_cmd_queue, cmd, 0);
    return true;
}

/* 封面: 立即清空引用, 隐藏图片控件, 显示默认图标 */
static void cover_clear(void)
{
    if (!s_album_img) return;
    lv_img_set_src(s_album_img, NULL);
    lv_obj_add_flag(s_album_img, LV_OBJ_FLAG_HIDDEN);
    if (s_album_icon) lv_obj_clear_flag(s_album_icon, LV_OBJ_FLAG_HIDDEN);
}

/* 封面: 让 img 控件指向解码任务给的 PSRAM 缓冲 (LVGL 不拷贝内存) */
static void cover_show(void *buf)
{
    if (!s_album_img || !buf) return;
    s_cover_dsc.data = buf;
    lv_img_set_src(s_album_img, &s_cover_dsc);
    lv_obj_clear_flag(s_album_img, LV_OBJ_FLAG_HIDDEN);
    if (s_album_icon) lv_obj_add_flag(s_album_icon, LV_OBJ_FLAG_HIDDEN);
}

static void player_play_index(int idx)
{
    if (s_pl_count <= 0) return;

    cover_clear();

    idx = (idx % s_pl_count + s_pl_count) % s_pl_count;

    const char *name = player_file_name_at(s_pl_group, idx);
    if (!name) return;

    char path[512];
    fs_build_real_path(s_pl_group, name, path, sizeof(path));

    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = AUDIO_CMD_PLAY;
    strncpy(cmd.path, path, sizeof(cmd.path) - 1);

    if (!audio_user_send(&cmd)) return;

    s_pl_index = idx;
    s_auto_advancing = false;
    s_was_playing = true;
    printf("[LVGL] PLAY: %s\n", path);

    player_update_label();

    /* 记录播放路径到 flash, 供下次插入 SD 卡时恢复 */
    last_song_save(path);
    /* 两向同步: 浏览器打开时刷新绿色高亮 */
    fs_browser_refresh();
}

/* 自动切歌(歌曲播完 / 跳过未找到): 直接发送, 不节流 */
static void player_advance(void)
{
    if (s_pl_count <= 0) return;

    cover_clear();

    int next;
    switch (s_play_mode) {
    case PLAY_MODE_SINGLE:
        next = s_pl_index;
        break;
    case PLAY_MODE_RANDOM:
        if (s_pl_count <= 1) {
            next = 0;
        } else {
            next = (int)(esp_random() % (uint32_t)s_pl_count);
            if (next == s_pl_index) next = (next + 1) % s_pl_count;
        }
        break;
    case PLAY_MODE_SEQUENTIAL:
    default:
        next = (s_pl_index + 1) % s_pl_count;
        break;
    }

    const char *name = player_file_name_at(s_pl_group, next);
    if (!name) return;

    char path[512];
    fs_build_real_path(s_pl_group, name, path, sizeof(path));

    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = AUDIO_CMD_PLAY;
    strncpy(cmd.path, path, sizeof(cmd.path) - 1);
    xQueueSend(s_audio_cmd_queue, &cmd, 0);

    s_pl_index = next;
    s_auto_advancing = true;
    s_was_playing = true;
    printf("[LVGL] AUTO PLAY: %s (mode %d)\n", path, s_play_mode);

    player_update_label();

    /* 自动切歌同样记录路径并刷新浏览器高亮 */
    last_song_save(path);
    fs_browser_refresh();
}

static void mode_update_icon(void)
{
    const char *sym = LV_SYMBOL_LEFT;
    if (s_play_mode == PLAY_MODE_SINGLE) {
        sym = LV_SYMBOL_LOOP;
    } else if (s_play_mode == PLAY_MODE_RANDOM) {
        sym = LV_SYMBOL_SHUFFLE;
    }
    if (s_mode_icon) {
        lv_label_set_text(s_mode_icon, sym);
    }
}

static void mode_btn_click_cb(lv_event_t *e)
{
    s_play_mode = (play_mode_t)((s_play_mode + 1) % 3);
    mode_update_icon();
    printf("[LVGL] 播放模式: %d\n", s_play_mode);

    /* 写入 NVS, 下次启动恢复 */
    nvs_handle_t h;
    if (nvs_open("player", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "play_mode", s_play_mode);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 启动时从 NVS 恢复播放模式, 返回是否成功 (供 UI 同步图标) */
static bool mode_load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("player", NVS_READONLY, &h) != ESP_OK) return false;

    int32_t m = PLAY_MODE_SEQUENTIAL;
    esp_err_t ret = nvs_get_i32(h, "play_mode", &m);
    nvs_close(h);

    if (ret == ESP_OK && m >= PLAY_MODE_SEQUENTIAL && m <= PLAY_MODE_RANDOM) {
        s_play_mode = (play_mode_t)m;
        printf("[LVGL] 播放模式恢复: %d\n", s_play_mode);
        return true;
    }
    return false;
}

static void player_play_file(const char *group, const char *name)
{
    if (!group || !name) return;

    strncpy(s_pl_group, group, sizeof(s_pl_group) - 1);
    s_pl_group[sizeof(s_pl_group) - 1] = '\0';
    s_pl_count = player_count_files(s_pl_group);

    int found = 0;
    for (int i = 0; i < s_pl_count; i++) {
        const char *n = player_file_name_at(s_pl_group, i);
        if (n && strcmp(n, name) == 0) {
            found = i;
            break;
        }
    }
    player_play_index(found);
}

static void player_next(void)
{
    if (s_pl_count <= 0) return;
    player_play_index(s_pl_index + 1);
}

static void player_prev(void)
{
    if (s_pl_count <= 0) return;
    player_play_index(s_pl_index - 1);
}

static void player_prev_click_cb(lv_event_t *e) { player_prev(); }
static void player_next_click_cb(lv_event_t *e) { player_next(); }

/* 播放/暂停切换 */
static void play_btn_click_cb(lv_event_t *e)
{
    if (atomic_load_bool(&g_pcm_active)) {
        audio_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = AUDIO_CMD_PAUSE;
        if (audio_user_send(&cmd)) {
            printf("[LVGL] PAUSE\n");
        }
    } else if (atomic_load_bool(&g_song_info_valid) && s_pl_count > 0) {
        /* 暂停中且有已加载歌曲 → 纯续播: 不清封面, 绕过 500ms 节流 */
        const char *name = player_file_name_at(s_pl_group, s_pl_index);
        if (name) {
            char path[512];
            fs_build_real_path(s_pl_group, name, path, sizeof(path));
            audio_cmd_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.type = AUDIO_CMD_PLAY;
            strncpy(cmd.path, path, sizeof(cmd.path) - 1);
            xQueueSend(s_audio_cmd_queue, &cmd, 0);
            s_was_playing = true;
            printf("[LVGL] RESUME: %s\n", path);
        }
    } else {
        player_play_index(s_pl_index);
    }
}

/* ── 文件不存在提示弹窗 ── */
static lv_obj_t *s_dialog = NULL;

static void dialog_close_cb(lv_event_t *e)
{
    if (s_dialog) {
        lv_obj_del(s_dialog);
        s_dialog = NULL;
    }
}

static void show_file_not_found(void)
{
    if (s_dialog) lv_obj_del(s_dialog);

    s_dialog = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_dialog, 150, 90);
    lv_obj_align(s_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_dialog, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dialog, 8, 0);
    lv_obj_set_style_border_width(s_dialog, 0, 0);
    lv_obj_set_style_shadow_width(s_dialog, 24, 0);
    lv_obj_set_style_shadow_color(s_dialog, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(s_dialog, LV_OPA_60, 0);
    lv_obj_clear_flag(s_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(s_dialog);
    lv_label_set_text(lbl, "该文件不存在");
    lv_obj_set_style_text_font(lbl, &chinese_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *btn = lv_btn_create(s_dialog);
    lv_obj_set_size(btn, 64, 30);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, dialog_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "确认");
    lv_obj_set_style_text_font(btn_lbl, &chinese_16, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_black(), 0);
    lv_obj_center(btn_lbl);
}

/* ── 歌曲信息轮询: 读 g_song_info 更新标签/进度 ── */
static void song_info_monitor_cb(lv_timer_t *timer)
{
    /* 播放/暂停图标跟随实际播放状态 */
    if (s_play_icon) {
        lv_label_set_text(s_play_icon,
                          atomic_load_bool(&g_pcm_active) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    if (atomic_load_bool(&g_song_info_valid)) {
        lv_label_set_text(s_title_label, g_song_info.title[0] ? g_song_info.title : " ");
        lv_label_set_text(s_artist_label, g_song_info.artist[0] ? g_song_info.artist : " ");
        lv_label_set_text(s_fmt_val, g_song_info.format);

        char buf[16];
        snprintf(buf, sizeof(buf), "%.1fk", (double)g_song_info.sample_rate / 1000.0);
        lv_label_set_text(s_sr_val, buf);
        snprintf(buf, sizeof(buf), "%" PRIu32 "k", g_song_info.bitrate_kbps);
        lv_label_set_text(s_bd_val, buf);

        uint32_t cur = g_song_info.elapsed_sec;
        uint32_t tot = g_song_info.duration_sec;
        snprintf(buf, sizeof(buf), "%" PRIu32 ":%02" PRIu32, cur / 60, cur % 60);
        lv_label_set_text(s_time_current, buf);
        snprintf(buf, sizeof(buf), "%" PRIu32 ":%02" PRIu32, tot / 60, tot % 60);
        lv_label_set_text(s_time_total, buf);

        if (tot > 0) {
            int val = (int)((uint64_t)cur * 1000 / tot);
            if (val > 1000) val = 1000;
            lv_bar_set_value(s_progress_bar, val, LV_ANIM_OFF);
        }
    } else {
        lv_label_set_text(s_title_label, "标题");
        lv_label_set_text(s_artist_label, "作者");
        lv_label_set_text(s_fmt_val, "");
        lv_label_set_text(s_sr_val, "");
        lv_label_set_text(s_bd_val, "");
        lv_label_set_text(s_time_current, "0:00");
        lv_label_set_text(s_time_total, "0:00");
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    }
}

static void fs_sd_monitor_cb(lv_timer_t *timer)
{
    static bool last_ready = false;
    bool sd_ready = atomic_load_bool(&g_sd_ready);

    if (!sd_ready && last_ready) {
        /* 音频立即停止, 干净 close_decoder */
        audio_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = AUDIO_CMD_STOP;
        xQueueSend(s_audio_cmd_queue, &cmd, 0);
        s_was_playing = false;

        /* 播放器状态复位: 残留的 SONG_FINISHED→advance 全部 no-op */
        s_pl_count = 0;
        s_pl_index = 0;
        player_update_label();

        cover_clear();

        fs_browser_on_sd_remove();
    }

    if (sd_ready && !last_ready) {
        fs_browser_on_sd_ready();

        /* 恢复上次歌曲: 读 flash 路径 → 在缓存中查找 → 找到则加载到解码器但不自动播放,
         * 紧随其后发一个暂停; 主UI跳到该曲, 浏览器同步高亮 */
        char saved[512];
        if (last_song_load(saved, sizeof(saved))) {
            char group[FS_GROUP_MAX];
            char name[FS_NAME_MAX];
            if (fs_cache_find_by_path(saved, group, sizeof(group), name, sizeof(name))) {
                player_play_file(group, name);

                /* 只加载不播放: 直接发暂停 (不用 audio_user_send, 避免被节流丢弃) */
                audio_cmd_t cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.type = AUDIO_CMD_PAUSE;
                xQueueSend(s_audio_cmd_queue, &cmd, 0);

                fs_browser_jump();
            }
        }
    }

    last_ready = sd_ready;
}

/* 创建音量弹窗(初始隐藏), 值变化时由定时器显示更新 */
static void vol_popup_create(void)
{
    s_vol_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(s_vol_cont, VOL_POP_X, VOL_POP_Y);
    lv_obj_set_size(s_vol_cont, VOL_POP_W, VOL_POP_H);
    lv_obj_set_style_bg_color(s_vol_cont, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_vol_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vol_cont, 6, 0);
    lv_obj_set_style_border_width(s_vol_cont, 0, 0);
    lv_obj_set_style_shadow_width(s_vol_cont, 0, 0);
    lv_obj_set_style_pad_all(s_vol_cont, 0, 0);
    lv_obj_clear_flag(s_vol_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_vol_bar = lv_bar_create(s_vol_cont);
    lv_obj_set_pos(s_vol_bar, (VOL_POP_W - 22) / 2, 4);
    lv_obj_set_size(s_vol_bar, 22, VOL_POP_BAR_H);
    lv_obj_set_style_bg_color(s_vol_bar, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_opa(s_vol_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vol_bar, 3, 0);
    lv_obj_set_style_radius(s_vol_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_vol_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(s_vol_bar, 0, VOLUME_MAX);
    lv_bar_set_value(s_vol_bar, volume_get(), LV_ANIM_OFF);

    s_vol_val = lv_label_create(s_vol_cont);
    lv_obj_set_pos(s_vol_val, 0, 4 + VOL_POP_BAR_H + 2);
    lv_obj_set_size(s_vol_val, VOL_POP_W, 18);
    lv_obj_set_style_text_align(s_vol_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_vol_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_vol_val, lv_color_black(), 0);
    lv_label_set_text_fmt(s_vol_val, "%" PRId32, volume_get());

    lv_obj_add_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);
    s_vol_last_ui = volume_get();
}

/* 音量轮询 (5Hz): 值变化则显示弹窗并写入 NVS, 2秒无变化自动隐藏 */
static void volume_monitor_cb(lv_timer_t *timer)
{
    int32_t v = volume_get();

    if (v != s_vol_last_ui) {
        s_vol_last_ui = v;
        s_vol_idle = 0;
        lv_obj_move_foreground(s_vol_cont);
        lv_obj_clear_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_vol_bar, v, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_vol_val, "%" PRId32, v);
    } else {
        s_vol_idle++;
        if (s_vol_idle >= VOL_POP_HIDE_MS / 200) {
            lv_obj_add_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);
        }
    }

    volume_save_to_nvs();
}

static void create_ui(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* ── 屏幕底色 ── */
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── TOP BAR ── */
    /* 菜单按钮 */
    lv_obj_t *btn_menu = make_icon_btn(scr, 5, 5, 40, 40, LV_SYMBOL_LIST);
    lv_obj_t *icon_menu = lv_obj_get_child(btn_menu, 0);
    lv_obj_set_style_text_font(icon_menu, &lv_font_montserrat_18, 0);
    lv_obj_add_event_cb(btn_menu, fs_menu_click_cb, LV_EVENT_CLICKED, NULL);

    /* 蓝牙按钮 */
    lv_obj_t *btn_ble = make_icon_btn(scr, 126, 5, 40, 40, LV_SYMBOL_BLUETOOTH);
    lv_obj_t *icon_ble = lv_obj_get_child(btn_ble, 0);
    lv_obj_set_style_text_font(icon_ble, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(btn_ble, bt_menu_click_cb, LV_EVENT_CLICKED, NULL);

    /* 文字 */
    s_status_label = lv_label_create(scr);
    lv_obj_set_pos(s_status_label, 54, 15);
    lv_obj_set_size(s_status_label, 65, 16);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_label, COLOR_MUTED, 0);
    lv_label_set_text(s_status_label, "PLAYER");


    /* 专辑封面区域 */
    s_album_art = lv_obj_create(scr);
    lv_obj_set_pos(s_album_art, 35, 40);
    lv_obj_set_size(s_album_art, 100, 100);
    lv_obj_set_style_radius(s_album_art, 20, 0);
    lv_obj_set_style_bg_color(s_album_art, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(s_album_art, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_album_art, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(s_album_art, LV_OPA_10, 0);
    lv_obj_set_style_border_width(s_album_art, 1, 0);
    lv_obj_set_style_shadow_color(s_album_art, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(s_album_art, 24, 0);
    lv_obj_set_style_shadow_opa(s_album_art, LV_OPA_60, 0);
    /* 子元素(封面图)按容器圆角裁剪 */
    lv_obj_set_style_clip_corner(s_album_art, true, 0);
    /* 封面铺满容器: 去内边距 + 禁止滚动 */
    lv_obj_set_style_pad_all(s_album_art, 0, 0);
    lv_obj_clear_flag(s_album_art, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *album_icon = lv_label_create(s_album_art);
    lv_label_set_text(album_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(album_icon, COLOR_ACCENT, 0);
    lv_obj_set_style_text_opa(album_icon, LV_OPA_40, 0);
    lv_obj_set_style_text_font(album_icon, &lv_font_montserrat_20, 0);
    lv_obj_center(album_icon);
    s_album_icon = album_icon;

    /* 封面 img: 只指向解码任务写好的 PSRAM RGB565, LVGL 不拷贝 */
    s_cover_dsc.header.always_zero = 0;
    s_cover_dsc.header.w = 100;
    s_cover_dsc.header.h = 100;
    s_cover_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_cover_dsc.data_size = 100 * 100 * 2;
    s_cover_dsc.data = NULL;

    s_album_img = lv_img_create(s_album_art);
    lv_obj_set_pos(s_album_img, 0, 0);
    lv_obj_set_size(s_album_img, 100, 100);
    lv_img_set_src(s_album_img, NULL);
    lv_obj_add_flag(s_album_img, LV_OBJ_FLAG_HIDDEN);

    /* ── 歌曲信息区域 ── */
    s_title_label = lv_label_create(scr);
    lv_obj_set_pos(s_title_label, 8, 148);
    lv_obj_set_size(s_title_label, 156, 20);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_title_label, &chinese_16, 0);
    lv_obj_set_style_text_color(s_title_label, COLOR_FG, 0);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_title_label, "标题");

    s_artist_label = lv_label_create(scr);
    lv_obj_set_pos(s_artist_label, 8, 170);
    lv_obj_set_size(s_artist_label, 156, 16);
    lv_obj_set_style_text_align(s_artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_artist_label, &chinese_16, 0);
    lv_obj_set_style_text_color(s_artist_label, COLOR_MUTED, 0);
    lv_label_set_long_mode(s_artist_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_artist_label, "作者");

    /* ── 播放进度�?── */
    s_progress_bar = lv_bar_create(scr);
    lv_obj_set_pos(s_progress_bar, 14, 194);
    lv_obj_set_size(s_progress_bar, 144, 4);
    lv_obj_set_style_radius(s_progress_bar, 2, 0);
    lv_obj_set_style_radius(s_progress_bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_progress_bar, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_progress_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(s_progress_bar, 0, 0);
    lv_obj_set_style_border_width(s_progress_bar, 0, LV_PART_INDICATOR);
    lv_bar_set_range(s_progress_bar, 0, 1000);
    lv_bar_set_value(s_progress_bar, 350, LV_ANIM_OFF);

    s_time_current = lv_label_create(scr);
    lv_obj_set_pos(s_time_current, 14, 204);
    lv_obj_set_style_text_font(s_time_current, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_time_current, COLOR_ACCENT, 0);
    lv_label_set_text(s_time_current, "1:24");

    s_time_total = lv_label_create(scr);
    lv_obj_set_pos(s_time_total, 110, 204);
    lv_obj_set_size(s_time_total, 48, 12);
    lv_obj_set_style_text_align(s_time_total, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_time_total, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_time_total, COLOR_MUTED, 0);
    lv_label_set_text(s_time_total, "4:02");

    /* ── 播放控制按钮�?── */
    /* 播放 / 暂停 */
    lv_obj_t *play_btn = lv_btn_create(scr);
    lv_obj_set_pos(play_btn, 63, 212);
    lv_obj_set_size(play_btn, 46, 46);
    lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(play_btn, 0, 0);
    lv_obj_set_style_shadow_color(play_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_shadow_width(play_btn, 12, 0);
    lv_obj_set_style_shadow_opa(play_btn, LV_OPA_30, 0);

    s_play_icon = lv_label_create(play_btn);
    lv_label_set_text(s_play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_play_icon, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_play_icon, &lv_font_montserrat_20, 0);
    lv_obj_center(s_play_icon);
    lv_obj_add_event_cb(play_btn, play_btn_click_cb, LV_EVENT_CLICKED, NULL);

    /* 上一�?*/
    lv_obj_t *prev_btn = lv_btn_create(scr);
    lv_obj_set_pos(prev_btn, 25, 220);
    lv_obj_set_size(prev_btn, 36, 36);
    lv_obj_set_style_radius(prev_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(prev_btn, LV_OPA_0, 0);
    lv_obj_set_style_border_width(prev_btn, 0, 0);
    lv_obj_set_style_shadow_width(prev_btn, 0, 0);

    lv_obj_t *prev_icon = lv_label_create(prev_btn);
    lv_label_set_text(prev_icon, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(prev_icon, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(prev_icon, &lv_font_montserrat_20, 0);
    lv_obj_center(prev_icon);
    lv_obj_add_event_cb(prev_btn, player_prev_click_cb, LV_EVENT_CLICKED, NULL);


    /* 下一�?*/
    lv_obj_t *next_btn = lv_btn_create(scr);
    lv_obj_set_pos(next_btn, 111, 220);
    lv_obj_set_size(next_btn, 36, 36);
    lv_obj_set_style_radius(next_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(next_btn, LV_OPA_0, 0);
    lv_obj_set_style_border_width(next_btn, 0, 0);
    lv_obj_set_style_shadow_width(next_btn, 0, 0);

    lv_obj_t *next_icon = lv_label_create(next_btn);
    lv_label_set_text(next_icon, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(next_icon, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(next_icon, &lv_font_montserrat_20, 0);
    lv_obj_center(next_icon);
    lv_obj_add_event_cb(next_btn, player_next_click_cb, LV_EVENT_CLICKED, NULL);

    /* ── 底部面板 ── */
    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_set_pos(panel, 4, 272);
    lv_obj_set_size(panel, 164, 46);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_bg_color(panel, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    /* 模式按钮 */
    lv_obj_t *mode_btn = lv_btn_create(panel);
    lv_obj_set_pos(mode_btn, 5, 5);
    lv_obj_set_size(mode_btn, 35, 35);
    lv_obj_set_style_radius(mode_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mode_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(mode_btn, LV_OPA_10, 0);
    lv_obj_set_style_border_width(mode_btn, 0, 0);
    lv_obj_set_style_shadow_width(mode_btn, 0, 0);

    s_mode_icon = lv_label_create(mode_btn);
    lv_label_set_text(s_mode_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(s_mode_icon, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_mode_icon, &lv_font_montserrat_14, 0);
    lv_obj_center(s_mode_icon);
    mode_update_icon();
    lv_obj_add_event_cb(mode_btn, mode_btn_click_cb, LV_EVENT_CLICKED, NULL);

    /* ── 技术参数信�?(右侧三列) ── */
    /* 格式 */
    s_fmt_val = lv_label_create(panel);
    lv_obj_set_pos(s_fmt_val, 52, 14);
    lv_obj_set_style_text_font(s_fmt_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_fmt_val, COLOR_ACCENT, 0);
    lv_label_set_text(s_fmt_val, "");

    /* 采样�?*/

    s_sr_val = lv_label_create(panel);
    lv_obj_set_pos(s_sr_val, 92, 14);
    lv_obj_set_style_text_font(s_sr_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sr_val, COLOR_FG, 0);
    lv_label_set_text(s_sr_val, "");

    /* 位深�?*/

    s_bd_val = lv_label_create(panel);
    lv_obj_set_pos(s_bd_val, 128, 14);
    lv_obj_set_style_text_font(s_bd_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_bd_val, COLOR_FG, 0);
    lv_label_set_text(s_bd_val, "");

    lv_timer_create(fs_sd_monitor_cb, 50, NULL);
    lv_timer_create(song_info_monitor_cb, 500, NULL);
    lv_timer_create(volume_monitor_cb, 200, NULL);

    vol_popup_create();
}

static void lvgl_task(void *arg)
{
    lv_init();

    s_panel = lcd_init(SPI2_HOST);
    touch_init();

    lv_disp_draw_buf_init(&s_draw_buf_dsc, s_draw_buf1, s_draw_buf2, LVGL_BUF_SIZE);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = TFT_HOR_RES;
    s_disp_drv.ver_res  = TFT_VER_RES;
    s_disp_drv.flush_cb = my_disp_flush;
    s_disp_drv.draw_buf = &s_draw_buf_dsc;
    lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    create_ui();

    bt_list_init(s_bt_iface);

    fs_list_set_play_cb(player_play_file);

    s_queue_set = xQueueCreateSet(3);
    xQueueAddToSet(s_app_cmd_queue,  s_queue_set);
    xQueueAddToSet(s_bt_iface->evt_queue, s_queue_set);
    xQueueAddToSet(s_audio_rsp_queue, s_queue_set);

    ESP_LOGI(LVGL_TAG, "UI ready");

    app_cmd_t   app_cmd;
    bt_evt_t    bt_evt;
    audio_rsp_t audio_rsp;
    audio_cmd_t audio_cmd;
    bt_cmd_t    bt_cmd;

    while (1) {
        uint32_t lv_delay = lv_timer_handler();

        QueueHandle_t active = xQueueSelectFromSet(s_queue_set, 0);

        if (active == s_app_cmd_queue) {
            while (xQueueReceive(s_app_cmd_queue, &app_cmd, 0) == pdTRUE) {
                switch (app_cmd.type) {
                case APP_CMD_BT_SCAN:
                    memset(&bt_cmd, 0, sizeof(bt_cmd));
                    bt_cmd.type = BT_CMD_SCAN;
                    xQueueSend(s_bt_iface->cmd_queue, &bt_cmd, 0);
                    printf("[LVGL] scan\n");
                    break;

                case APP_CMD_BT_CONNECT:
                    if (bt_a2dp_is_connected()) {
                        printf("[LVGL] already connected\n");
                    } else {
                        memset(&bt_cmd, 0, sizeof(bt_cmd));
                        bt_cmd.type = BT_CMD_CONNECT;
                        strncpy(bt_cmd.device_name, app_cmd.param,
                                sizeof(bt_cmd.device_name) - 1);
                        xQueueSend(s_bt_iface->cmd_queue, &bt_cmd, 0);
                        printf("[LVGL] connecting: %s\n", app_cmd.param);
                    }
                    break;

                case APP_CMD_BT_DISCONNECT:
                    memset(&bt_cmd, 0, sizeof(bt_cmd));
                    bt_cmd.type = BT_CMD_DISCONNECT;
                    xQueueSend(s_bt_iface->cmd_queue, &bt_cmd, 0);
                    printf("[LVGL] disconnecting\n");
                    break;

                case APP_CMD_PLAY:
                    if (bt_a2dp_is_connected()) {
                        struct stat st;
                        if (!sdmmc_disk_is_mounted()) {
                            printf("[LVGL] SD not mounted\n");
                        } else if (stat("/sdcard/a.mp3", &st) != 0) {
                            printf("[LVGL] /sdcard/a.mp3 not found\n");
                        } else {
                            audio_cmd.type = AUDIO_CMD_PLAY;
                            strcpy(audio_cmd.path, "/sdcard/a.mp3");
                            xQueueSend(s_audio_cmd_queue, &audio_cmd, 0);
                            s_was_playing = true;
                            printf("[LVGL] PLAY sent | ts=%lld us\n", esp_timer_get_time());
                        }
                    } else {
                        printf("[LVGL] not connected, can't play\n");
                    }
                    break;

                case APP_CMD_STOP:
                    audio_cmd.type = AUDIO_CMD_STOP;
                    xQueueSend(s_audio_cmd_queue, &audio_cmd, 0);
                    s_was_playing = false;
                    break;

                case APP_CMD_PAUSE:
                    audio_cmd.type = AUDIO_CMD_PAUSE;
                    xQueueSend(s_audio_cmd_queue, &audio_cmd, 0);
                    break;

                case APP_CMD_INFO:
                    printf("[info] connected: %s | playing: %s\n",
                           bt_a2dp_is_connected() ? "yes" : "no",
                           s_was_playing ? "yes" : "no");
                    break;

                case APP_CMD_COVER_READY: {
                    void *buf;
                    memcpy(&buf, app_cmd.param, sizeof(buf));
                    cover_show(buf);
                    break;
                }
                }
            }
        }

        if (active == s_bt_iface->evt_queue) {
            while (xQueueReceive(s_bt_iface->evt_queue, &bt_evt, 0) == pdTRUE) {
                switch (bt_evt.type) {
                case BT_EVT_DEVICE_FOUND:
                    bt_list_on_device_found(bt_evt.device_name);
                    break;
                case BT_EVT_SCAN_DONE:
                    bt_list_on_scan_done();
                    break;
                case BT_EVT_CONNECTED:
                    audio_cmd.type = AUDIO_CMD_BT_CONNECTED;
                    xQueueSend(s_audio_cmd_queue, &audio_cmd, 0);
                    bt_list_on_connected(bt_evt.device_name);
                    if (s_was_playing) {
                        printf("[LVGL] BT resume, notify audio\n");
                    }
                    break;
                case BT_EVT_CONNECT_FAILED:
                    bt_list_on_connect_failed(bt_evt.device_name);
                    break;
                case BT_EVT_DISCONNECTED:
                    audio_cmd.type = AUDIO_CMD_BT_DISCONNECTED;
                    xQueueSend(s_audio_cmd_queue, &audio_cmd, 0);
                    bt_list_on_disconnected();
                    break;
                case BT_EVT_STREAM_READY:
                    printf("[stream ready]\n");
                    break;
                case BT_EVT_STREAM_STOPPED:
                    printf("[stream stopped]\n");
                    break;
                case BT_EVT_STATE_RSP:
                    bt_list_on_state_rsp(bt_evt.state, bt_evt.device_name);
                    break;
                }
            }
        }

        if (active == s_audio_rsp_queue) {
            while (xQueueReceive(s_audio_rsp_queue, &audio_rsp, 0) == pdTRUE) {
                switch (audio_rsp.type) {
                case AUDIO_RSP_BT_CHECK:
                    if (bt_a2dp_is_connected()) {
                        audio_cmd.type = AUDIO_CMD_BT_CONNECTED;
                    } else {
                        audio_cmd.type = AUDIO_CMD_BT_DISCONNECTED;
                    }
                    xQueueSend(s_audio_cmd_queue, &audio_cmd, 0);
                    break;
                case AUDIO_RSP_FILE_NOT_FOUND:
                    if (s_auto_advancing) {
                        player_advance();
                    } else {
                        show_file_not_found();
                    }
                    break;
                case AUDIO_RSP_SONG_FINISHED:
                    s_auto_advancing = true;
                    player_advance();
                    break;
                }
            }
        }

        if (lv_delay > 20) lv_delay = 20;
        if (lv_delay < 10) lv_delay = 10;
        vTaskDelay(pdMS_TO_TICKS(lv_delay));
    }
}

void lvgl_task_init(const lvgl_task_params_t *params)
{
    s_app_cmd_queue  = params->app_cmd_queue;
    s_bt_iface       = params->bt_iface;
    s_audio_cmd_queue = params->audio_cmd_queue;
    s_audio_rsp_queue = params->audio_rsp_queue;

    volume_load_from_nvs();
    mode_load_from_nvs();

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 1, NULL, 1);
}
