#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "lcd_jd9853.h"
#include "touch_cst816.h"
#include "lvgl.h"
#include "bt_a2dp.h"
#include "sys_serial.h"
#include "audio_task.h"
#include "lvgl_task.h"

#define LVGL_TAG "LVGL"

#define TFT_HOR_RES   172
#define TFT_VER_RES   320
#define DRAW_BUF_SIZE  (TFT_HOR_RES * TFT_VER_RES / 3)
#define LVGL_BUF_SIZE  (DRAW_BUF_SIZE / 2)

#define MAX_TOUCH_DOTS  100

#define TOUCH_Y_MIN  10
#define TOUCH_Y_MAX  300

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

static esp_lcd_panel_handle_t    s_panel    = NULL;
static lv_obj_t *s_touch_dots[MAX_TOUCH_DOTS];
static int      s_dot_idx = 0;

static void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                          lv_color_t *color_p)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, color_p);
    lv_disp_flush_ready(disp_drv);
}

static void lvgl_task(void *arg)
{
    lv_init();

    /* --- LCD hardware init --- */
    s_panel = lcd_init(SPI2_HOST);
    touch_init();

    /* --- LVGL display driver --- */
    lv_disp_draw_buf_init(&s_draw_buf_dsc, s_draw_buf1, s_draw_buf2, LVGL_BUF_SIZE);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = TFT_HOR_RES;
    s_disp_drv.ver_res  = TFT_VER_RES;
    s_disp_drv.flush_cb = my_disp_flush;
    s_disp_drv.draw_buf = &s_draw_buf_dsc;
    lv_disp_drv_register(&s_disp_drv);

    /* 白底 + 绿85×159(左上) + 蓝85×159(右下) */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_STATE_DEFAULT);

    lv_obj_t *green_block = lv_obj_create(lv_scr_act());
    lv_obj_set_size(green_block, 85, 159);
    lv_obj_set_pos(green_block, 1, 1);
    lv_obj_set_style_bg_color(green_block, lv_color_make(0, 255, 0), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(green_block, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(green_block, 0, LV_STATE_DEFAULT);

    lv_obj_t *blue_block = lv_obj_create(lv_scr_act());
    lv_obj_set_size(blue_block, 85, 159);
    lv_obj_set_pos(blue_block, 86, 160);
    lv_obj_set_style_bg_color(blue_block, lv_color_make(0, 0, 255), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(blue_block, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(blue_block, 0, LV_STATE_DEFAULT);

    /* 触摸轨迹点 (5×5 红色圆) */
    for (int i = 0; i < MAX_TOUCH_DOTS; i++) {
        s_touch_dots[i] = lv_obj_create(lv_scr_act());
        lv_obj_set_size(s_touch_dots[i], 5, 5);
        lv_obj_set_style_bg_color(s_touch_dots[i], lv_color_make(255, 0, 0), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_touch_dots[i], LV_OPA_COVER, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_touch_dots[i], 0, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(s_touch_dots[i], LV_RADIUS_CIRCLE, LV_STATE_DEFAULT);
        lv_obj_add_flag(s_touch_dots[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_queue_set = xQueueCreateSet(3);
    xQueueAddToSet(s_app_cmd_queue,  s_queue_set);
    xQueueAddToSet(s_bt_iface->evt_queue, s_queue_set);
    xQueueAddToSet(s_audio_rsp_queue, s_queue_set);

    ESP_LOGI(LVGL_TAG, "LVGL 任务就绪");

    app_cmd_t  app_cmd;
    bt_evt_t   bt_evt;
    audio_rsp_t audio_rsp;
    audio_cmd_t audio_cmd;
    bt_cmd_t   bt_cmd;

    while (1) {
        uint32_t lv_delay = lv_timer_handler();

        /* --- 队列事件处理 --- */
        QueueHandle_t active = xQueueSelectFromSet(s_queue_set, 0);

        if (active == s_app_cmd_queue) {
            while (xQueueReceive(s_app_cmd_queue, &app_cmd, 0) == pdTRUE) {
                switch (app_cmd.type) {
                case APP_CMD_BT_SCAN:
                    memset(&bt_cmd, 0, sizeof(bt_cmd));
                    bt_cmd.type = BT_CMD_SCAN;
                    xQueueSend(s_bt_iface->cmd_queue, &bt_cmd, 0);
                    printf("[LVGL] 开始扫描\n");
                    break;

                case APP_CMD_BT_CONNECT:
                    if (bt_a2dp_is_connected()) {
                        printf("[LVGL] 已连接, 请先断开\n");
                    } else {
                        memset(&bt_cmd, 0, sizeof(bt_cmd));
                        bt_cmd.type = BT_CMD_CONNECT;
                        strncpy(bt_cmd.device_name, app_cmd.param,
                                sizeof(bt_cmd.device_name) - 1);
                        xQueueSend(s_bt_iface->cmd_queue, &bt_cmd, 0);
                        printf("[LVGL] 正在连接: %s\n", app_cmd.param);
                    }
                    break;

                case APP_CMD_BT_DISCONNECT:
                    memset(&bt_cmd, 0, sizeof(bt_cmd));
                    bt_cmd.type = BT_CMD_DISCONNECT;
                    xQueueSend(s_bt_iface->cmd_queue, &bt_cmd, 0);
                    printf("[LVGL] 正在断开\n");
                    break;

                case APP_CMD_PLAY:
                    if (bt_a2dp_is_connected()) {
                        audio_cmd.type = AUDIO_CMD_PLAY;
                        xQueueSend(s_audio_cmd_queue, &audio_cmd, 0);
                        s_was_playing = true;
                    } else {
                        printf("[LVGL] 未连接, 无法播放\n");
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
                    printf("[信息] 连接状态: %s | 播放状态: %s\n",
                           bt_a2dp_is_connected() ? "已连接" : "未连接",
                           s_was_playing ? "播放中" : "已停止");
                    break;
                }
            }
        }

        if (active == s_bt_iface->evt_queue) {
            while (xQueueReceive(s_bt_iface->evt_queue, &bt_evt, 0) == pdTRUE) {
                switch (bt_evt.type) {
                case BT_EVT_DEVICE_FOUND:
                    printf("[发现设备] %s\n", bt_evt.device_name);
                    break;
                case BT_EVT_SCAN_DONE:
                    printf("[扫描完成]\n");
                    break;
                case BT_EVT_CONNECTED:
                    printf("[已连接]\n");
                    audio_cmd.type = AUDIO_CMD_BT_CONNECTED;
                    xQueueSend(s_audio_cmd_queue, &audio_cmd, 0);
                    if (s_was_playing) {
                        printf("[LVGL] BT 恢复, 通知音频继续播放\n");
                    }
                    break;
                case BT_EVT_CONNECT_FAILED:
                    printf("[连接失败] %s\n", bt_evt.device_name);
                    break;
                case BT_EVT_DISCONNECTED:
                    printf("[已断开]\n");
                    audio_cmd.type = AUDIO_CMD_BT_DISCONNECTED;
                    xQueueSend(s_audio_cmd_queue, &audio_cmd, 0);
                    break;
                case BT_EVT_STREAM_READY:
                    printf("[流已就绪]\n");
                    break;
                case BT_EVT_STREAM_STOPPED:
                    printf("[流已停止]\n");
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

        /* 50Hz 触摸轮询 */
        static TickType_t s_last_touch = 0;
        TickType_t now = xTaskGetTickCount();
        if ((now - s_last_touch) >= pdMS_TO_TICKS(20)) {
            s_last_touch = now;
            uint16_t tx, ty;
            if (touch_read(&tx, &ty)) {
                /* Y轴线性重映射: CST816T 固件锁死在 ~[17,289], 拉伸到全屏 */
                #define TOUCH_Y_MIN  17
                #define TOUCH_Y_MAX  289
                ty = (uint16_t)(((int)ty - TOUCH_Y_MIN) * (TFT_VER_RES - 1)
                                / (TOUCH_Y_MAX - TOUCH_Y_MIN));

                tx = TFT_HOR_RES - 1 - tx;
                ty = TFT_VER_RES - 1 - ty;

                if (tx > TFT_HOR_RES - 1) tx = TFT_HOR_RES - 1;
                if (ty > TFT_VER_RES - 1) ty = TFT_VER_RES - 1;
                lv_obj_set_pos(s_touch_dots[s_dot_idx], tx - 2, ty - 2);
                lv_obj_clear_flag(s_touch_dots[s_dot_idx], LV_OBJ_FLAG_HIDDEN);
                s_dot_idx++;
                if (s_dot_idx >= MAX_TOUCH_DOTS) s_dot_idx = 0;
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

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 4096, NULL, 8, NULL, 1);
}
