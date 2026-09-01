#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "bt_a2dp.h"
#include "audio_task.h"
#include "sys_monitor.h"
#include "app.h"
#include "drv_display.h"
#include "ui_core.h"
#include "ui_player.h"
#include "menu.h"
#include "power_mgr.h"

#define LVGL_TAG "LVGL"

static QueueSetHandle_t s_queue_set = NULL;

void ui_loop_task(void *arg)
{
    ui_core_display_init();

    ui_player_init();
    bt_list_init(g_ui_bt_iface);
    fs_list_set_play_cb(player_play_file);

    s_queue_set = xQueueCreateSet(3);
    xQueueAddToSet(g_ui_app_cmd_queue,  s_queue_set);
    xQueueAddToSet(g_ui_bt_iface->evt_queue, s_queue_set);
    xQueueAddToSet(g_ui_audio_rsp_queue, s_queue_set);

    /* LCD 后半段: 距 SLPOUT ≥120ms 后发寄存器命令 + DISPON (不足则阻塞补齐) */
    lcd_init_finish();
    lv_timer_handler();
    power_mgr_init();   


    app_cmd_t   app_cmd;
    bt_evt_t    bt_evt;
    audio_rsp_t audio_rsp;
    audio_cmd_t audio_cmd;
    bt_cmd_t    bt_cmd;

    while (1) {
        lv_timer_handler();

        QueueHandle_t active = xQueueSelectFromSet(s_queue_set, 0);

        if (active == g_ui_app_cmd_queue) {
            while (xQueueReceive(g_ui_app_cmd_queue, &app_cmd, 0) == pdTRUE) {
                switch (app_cmd.type) {
                case APP_CMD_BT_SCAN:
                    memset(&bt_cmd, 0, sizeof(bt_cmd));
                    bt_cmd.type = BT_CMD_SCAN;
                    xQueueSend(g_ui_bt_iface->cmd_queue, &bt_cmd, 0);
                    printf("[LVGL] scan\n");
                    break;

                case APP_CMD_BT_CONNECT:
                    if (bt_a2dp_is_connected()) {
                        printf("[LVGL] already connected\n");
                    } else {
                        memset(&bt_cmd, 0, sizeof(bt_cmd));
                        bt_cmd.type = BT_CMD_CONNECT;
                        size_t plen = strnlen(app_cmd.param, sizeof(bt_cmd.device_name) - 1);
                        memcpy(bt_cmd.device_name, app_cmd.param, plen);
                        bt_cmd.device_name[plen] = '\0';
                        xQueueSend(g_ui_bt_iface->cmd_queue, &bt_cmd, 0);
                        printf("[LVGL] connecting: %s\n", app_cmd.param);
                    }
                    break;

                case APP_CMD_BT_DISCONNECT:
                    memset(&bt_cmd, 0, sizeof(bt_cmd));
                    bt_cmd.type = BT_CMD_DISCONNECT;
                    xQueueSend(g_ui_bt_iface->cmd_queue, &bt_cmd, 0);
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
                            xQueueSend(g_ui_audio_cmd_queue, &audio_cmd, 0);
                            player_set_was_playing(true);
                            printf("[LVGL] PLAY sent | ts=%lld us\n", esp_timer_get_time());
                        }
                    } else {
                        printf("[LVGL] not connected, can't play\n");
                    }
                    break;

                case APP_CMD_STOP:
                    audio_cmd.type = AUDIO_CMD_STOP;
                    xQueueSend(g_ui_audio_cmd_queue, &audio_cmd, 0);
                    player_set_was_playing(false);
                    break;

                case APP_CMD_PAUSE:
                    audio_cmd.type = AUDIO_CMD_PAUSE;
                    xQueueSend(g_ui_audio_cmd_queue, &audio_cmd, 0);
                    break;

                case APP_CMD_INFO:
                    printf("[info] connected: %s | playing: %s\n",
                           bt_a2dp_is_connected() ? "yes" : "no",
                           player_was_playing() ? "yes" : "no");
                    break;

                case APP_CMD_COVER_READY: {
                    void *buf;
                    memcpy(&buf, app_cmd.param, sizeof(buf));
                    player_show_cover(buf);
                    break;
                }
                }
            }
        }

        if (active == g_ui_bt_iface->evt_queue) {
            while (xQueueReceive(g_ui_bt_iface->evt_queue, &bt_evt, 0) == pdTRUE) {
                switch (bt_evt.type) {
                case BT_EVT_DEVICE_FOUND:
                    bt_list_on_device_found(bt_evt.device_name);
                    break;
                case BT_EVT_SCAN_DONE:
                    bt_list_on_scan_done();
                    break;
                case BT_EVT_CONNECTED:
                    audio_cmd.type = AUDIO_CMD_BT_CONNECTED;
                    xQueueSend(g_ui_audio_cmd_queue, &audio_cmd, 0);
                    bt_list_on_connected(bt_evt.device_name);
                    if (player_was_playing()) {
                        printf("[LVGL] BT resume, notify audio\n");
                    }
                    break;
                case BT_EVT_CONNECT_FAILED:
                    bt_list_on_connect_failed(bt_evt.device_name);
                    break;
                case BT_EVT_DISCONNECTED:
                    audio_cmd.type = AUDIO_CMD_BT_DISCONNECTED;
                    xQueueSend(g_ui_audio_cmd_queue, &audio_cmd, 0);
                    bt_list_on_disconnected();
                    break;
                case BT_EVT_STREAM_READY:
                    printf("[stream ready]\n");
                    break;
                case BT_EVT_STREAM_STOPPED:
                    printf("[stream stopped]\n");
                    break;
                case BT_EVT_PLAY_PAUSE:
                    printf("[LVGL] BT 切换播放/暂停\n");
                    player_toggle_play();
                    break;
                case BT_EVT_TRANSPORT_NEXT:
                    printf("[LVGL] BT 下一曲\n");
                    player_next();
                    break;
                case BT_EVT_TRANSPORT_PREV:
                    printf("[LVGL] BT 上一曲\n");
                    player_prev();
                    break;
                case BT_EVT_STATE_RSP:
                    bt_list_on_state_rsp(bt_evt.state, bt_evt.device_name);
                    break;
                }
            }
        }

        if (active == g_ui_audio_rsp_queue) {
            while (xQueueReceive(g_ui_audio_rsp_queue, &audio_rsp, 0) == pdTRUE) {
                switch (audio_rsp.type) {
                case AUDIO_RSP_BT_CHECK:
                    if (bt_a2dp_is_connected()) {
                        audio_cmd.type = AUDIO_CMD_BT_CONNECTED;
                    } else {
                        audio_cmd.type = AUDIO_CMD_BT_DISCONNECTED;
                    }
                    xQueueSend(g_ui_audio_cmd_queue, &audio_cmd, 0);
                    break;
                case AUDIO_RSP_FILE_NOT_FOUND:
                    player_on_file_not_found();
                    break;
                case AUDIO_RSP_SONG_FINISHED:
                    player_on_song_finished();
                    break;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
