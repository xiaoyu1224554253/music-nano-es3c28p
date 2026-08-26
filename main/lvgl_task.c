#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "lcd_jd9853.h"
#include "touch_cst816.h"
#include "lvgl.h"
#include "bt_a2dp.h"
#include "sys_serial.h"
#include "audio_task.h"
#include "lvgl_task.h"
#include "sys_monitor.h"
#include "ui_list.h"
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
static lv_obj_t *s_play_icon;
static lv_obj_t *s_mode_icon;

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

static void fs_sd_monitor_cb(lv_timer_t *timer)
{
    static bool last_ready = false;
    bool sd_ready = atomic_load_bool(&g_sd_ready);

    if (!sd_ready && last_ready) {
        fs_browser_on_sd_remove();
        atomic_store_bool(&g_sd_remove_ack, true);
    }

    if (sd_ready && !last_ready) {
        fs_browser_on_sd_ready();
    }

    last_ready = sd_ready;
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

    lv_obj_t *album_icon = lv_label_create(s_album_art);
    lv_label_set_text(album_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(album_icon, COLOR_ACCENT, 0);
    lv_obj_set_style_text_opa(album_icon, LV_OPA_40, 0);
    lv_obj_set_style_text_font(album_icon, &lv_font_montserrat_20, 0);
    lv_obj_center(album_icon);

    /* ── 歌曲信息区域 ── */
    s_title_label = lv_label_create(scr);
    lv_obj_set_pos(s_title_label, 8, 148);
    lv_obj_set_size(s_title_label, 156, 20);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_title_label, &chinese_16, 0);
    lv_obj_set_style_text_color(s_title_label, COLOR_FG, 0);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_title_label, "Midnight 中文");

    s_artist_label = lv_label_create(scr);
    lv_obj_set_pos(s_artist_label, 8, 170);
    lv_obj_set_size(s_artist_label, 156, 16);
    lv_obj_set_style_text_align(s_artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_artist_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_artist_label, COLOR_MUTED, 0);
    lv_label_set_long_mode(s_artist_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_artist_label, "Aurora Waves");

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
    lv_label_set_text(s_mode_icon, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_color(s_mode_icon, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_mode_icon, &lv_font_montserrat_14, 0);
    lv_obj_center(s_mode_icon);

    /* ── 技术参数信�?(右侧三列) ── */
    /* 格式 */
    s_fmt_val = lv_label_create(panel);
    lv_obj_set_pos(s_fmt_val, 52, 14);
    lv_obj_set_style_text_font(s_fmt_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_fmt_val, COLOR_ACCENT, 0);
    lv_label_set_text(s_fmt_val, "MP3");

    /* 采样�?*/

    s_sr_val = lv_label_create(panel);
    lv_obj_set_pos(s_sr_val, 92, 14);
    lv_obj_set_style_text_font(s_sr_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sr_val, COLOR_FG, 0);
    lv_label_set_text(s_sr_val, "44.1k");

    /* 位深�?*/

    s_bd_val = lv_label_create(panel);
    lv_obj_set_pos(s_bd_val, 128, 14);
    lv_obj_set_style_text_font(s_bd_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_bd_val, COLOR_FG, 0);
    lv_label_set_text(s_bd_val, "320k");

    lv_timer_create(fs_sd_monitor_cb, 50, NULL);
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
                }
            }
        }

        if (active == s_bt_iface->evt_queue) {
            while (xQueueReceive(s_bt_iface->evt_queue, &bt_evt, 0) == pdTRUE) {
                switch (bt_evt.type) {
                case BT_EVT_DEVICE_FOUND:
                    printf("[found] %s\n", bt_evt.device_name);
                    break;
                case BT_EVT_SCAN_DONE:
                    printf("[scan done]\n");
                    break;
                case BT_EVT_CONNECTED:
                    printf("[connected]\n");
                    audio_cmd.type = AUDIO_CMD_BT_CONNECTED;
                    xQueueSend(s_audio_cmd_queue, &audio_cmd, 0);
                    if (s_was_playing) {
                        printf("[LVGL] BT resume, notify audio\n");
                    }
                    break;
                case BT_EVT_CONNECT_FAILED:
                    printf("[connect failed] %s\n", bt_evt.device_name);
                    break;
                case BT_EVT_DISCONNECTED:
                    printf("[disconnected]\n");
                    audio_cmd.type = AUDIO_CMD_BT_DISCONNECTED;
                    xQueueSend(s_audio_cmd_queue, &audio_cmd, 0);
                    break;
                case BT_EVT_STREAM_READY:
                    printf("[stream ready]\n");
                    break;
                case BT_EVT_STREAM_STOPPED:
                    printf("[stream stopped]\n");
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

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 1, NULL, 1);
}
