#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "bt_a2dp.h"
#include "lvgl.h"

#define MAIN_TAG "MAIN"
#define LINE_BUF_SIZE 64

#define TFT_HOR_RES   172
#define TFT_VER_RES   320
#define DRAW_BUF_SIZE (TFT_HOR_RES * TFT_VER_RES / 2 )
static lv_color_t draw_buf[DRAW_BUF_SIZE];

static lv_disp_draw_buf_t draw_buf_dsc;
static lv_disp_drv_t disp_drv;
static lv_disp_t * disp;

static lv_indev_drv_t indev_drv;
static lv_indev_t * indev;

static bt_a2dp_iface_t *s_bt = NULL;

static void evt_handler(void)
{
    bt_evt_t evt;

    while (xQueueReceive(s_bt->evt_queue, &evt, 0) == pdTRUE) {
        switch (evt.type) {
        case BT_EVT_DEVICE_FOUND:
            printf("[发现设备] %s\n", evt.device_name);
            break;
        case BT_EVT_SCAN_DONE:
            printf("[扫描完成]\n");
            break;
        case BT_EVT_CONNECTED:
            printf("[已连接]\n");
            break;
        case BT_EVT_CONNECT_FAILED:
            printf("[连接失败] %s\n", evt.device_name);
            break;
        case BT_EVT_DISCONNECTED:
            printf("[已断开]\n");
            break;
        default:
            break;
        }
    }
}

static int16_t sine_lut[128];
static bool sine_lut_ready = false;

static void build_sine_lut(void)
{
    for (int i = 0; i < 128; i++) {
        sine_lut[i] = (int16_t)(16000.0 * sinf(2.0 * 3.14159265 * i / 128));
    }
    sine_lut_ready = true;
}

static bool send_test_pcm(void)
{
    if (!sine_lut_ready) build_sine_lut();

    static uint32_t phase = 0;
    const int samples = 441;
    int16_t buf[samples * 2];
    int freq = 440;

    for (int i = 0; i < samples; i++) {
        int idx = (phase * freq * 128 / 44100) & 127;
        int16_t val = sine_lut[idx];
        buf[i * 2]     = val;
        buf[i * 2 + 1] = val;
        phase++;
    }

    xStreamBufferSend(s_bt->pcm_stream, buf, sizeof(buf), pdMS_TO_TICKS(50));

    bt_evt_t evt;
    bool disconnected = false;
    while (xQueueReceive(s_bt->evt_queue, &evt, 0) == pdTRUE) {
        if (evt.type == BT_EVT_DISCONNECTED) {
            printf("[已断开]\n");
            disconnected = true;
        }
    }

    if (disconnected || !bt_a2dp_is_connected()) {
        xStreamBufferReset(s_bt->pcm_stream);
        return false;
    }

    return true;
}

static void print_system_stats(void)
{
    static int64_t last_time = 0;
    static uint32_t last_total = 0;
    static uint32_t last_idle0 = 0;
    static uint32_t last_idle1 = 0;

    int64_t now = esp_timer_get_time();
    if (last_time == 0) {
        last_time = now;
        TaskStatus_t *tasks = NULL;
        UBaseType_t task_count = uxTaskGetNumberOfTasks();
        tasks = malloc(task_count * sizeof(TaskStatus_t));
        if (tasks) {
            task_count = uxTaskGetSystemState(tasks, task_count, NULL);
            for (UBaseType_t i = 0; i < task_count; i++) {
                if (strcmp(tasks[i].pcTaskName, "IDLE0") == 0) last_idle0 = tasks[i].ulRunTimeCounter;
                if (strcmp(tasks[i].pcTaskName, "IDLE1") == 0) last_idle1 = tasks[i].ulRunTimeCounter;
                last_total += tasks[i].ulRunTimeCounter;
            }
            free(tasks);
        }
        return;
    }

    if (now - last_time < 5000000) return;

    last_time = now;

    TaskStatus_t *tasks = NULL;
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    tasks = malloc(task_count * sizeof(TaskStatus_t));
    if (!tasks) return;

    uint32_t cur_total = 0;
    uint32_t cur_idle0 = 0;
    uint32_t cur_idle1 = 0;

    task_count = uxTaskGetSystemState(tasks, task_count, NULL);
    for (UBaseType_t i = 0; i < task_count; i++) {
        if (strcmp(tasks[i].pcTaskName, "IDLE0") == 0) cur_idle0 = tasks[i].ulRunTimeCounter;
        if (strcmp(tasks[i].pcTaskName, "IDLE1") == 0) cur_idle1 = tasks[i].ulRunTimeCounter;
        cur_total += tasks[i].ulRunTimeCounter;
    }
    free(tasks);

    uint32_t delta_total = cur_total - last_total;
    if (delta_total == 0) return;

    int cpu0_idle = (int)((uint64_t)(cur_idle0 - last_idle0) * 200 / delta_total);
    int cpu1_idle = (int)((uint64_t)(cur_idle1 - last_idle1) * 200 / delta_total);

    last_total  = cur_total;
    last_idle0  = cur_idle0;
    last_idle1  = cur_idle1;

    uint32_t free_heap = esp_get_free_heap_size();
    printf("[系统] 空闲内存: %"PRIu32"KB | CPU0 空闲: %d%% | CPU1 空闲: %d%%\n",
           free_heap / 1024, cpu0_idle, cpu1_idle);
}

void my_disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p) {

    lv_disp_flush_ready(disp_drv);
}

void app_main(void)
{
    nvs_flash_init();

    lv_init();
    lv_disp_draw_buf_init(&draw_buf_dsc, draw_buf, NULL, DRAW_BUF_SIZE);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = TFT_HOR_RES;
    disp_drv.ver_res = TFT_VER_RES;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf_dsc;
    disp_drv.sw_rotate = 1;
    disp_drv.rotated = LV_DISP_ROT_NONE;
    disp = lv_disp_drv_register(&disp_drv);

    s_bt = bt_a2dp_init();
    if (!s_bt) {
        printf("蓝牙初始化失败\n");
        return;
    }

    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    printf("\n=== 音乐播放器 ===\n");
    printf("命令: scan | conn <名称> | disconn | info\n");
    printf("例如: scan\n");
    printf("      conn TWS\n");
    printf("      disconn\n");
    printf("      info\n\n");
    printf("命令> ");

    char line[LINE_BUF_SIZE];
    int  line_pos = 0;
    bool connected = false;

    while (1) {
        evt_handler();

        char ch;
        while (read(STDIN_FILENO, &ch, 1) > 0) {
            if (ch == '\n' || ch == '\r') {
                if (line_pos > 0) {
                    line[line_pos] = '\0';
                    printf("\n");

                    bt_cmd_t cmd;
                    memset(&cmd, 0, sizeof(cmd));

                    if (strcmp(line, "scan") == 0) {
                        if (connected) {
                            printf("请先断开当前连接\n");
                        } else {
                            printf("[命令] 开始扫描...\n");
                            cmd.type = BT_CMD_SCAN;
                            xQueueSend(s_bt->cmd_queue, &cmd, 0);
                        }
                    } else if (strncmp(line, "conn ", 5) == 0) {
                        if (connected) {
                            printf("请先断开当前连接\n");
                        } else {
                            strncpy(cmd.device_name, line + 5, sizeof(cmd.device_name) - 1);
                            printf("[命令] 正在连接: %s\n", cmd.device_name);
                            cmd.type = BT_CMD_CONNECT;
                            xQueueSend(s_bt->cmd_queue, &cmd, 0);
                        }
                    } else if (strcmp(line, "disconn") == 0) {
                        printf("[命令] 正在断开...\n");
                        cmd.type = BT_CMD_DISCONNECT;
                        xQueueSend(s_bt->cmd_queue, &cmd, 0);
                    } else if (strcmp(line, "info") == 0) {
                        printf("连接状态: %s\n", bt_a2dp_is_connected() ? "已连接" : "未连接");
                    } else {
                        printf("未知命令: %s\n", line);
                    }

                    line_pos = 0;
                    printf("命令> ");
                }
            } else if (ch == '\b' || ch == 127) {
                if (line_pos > 0) line_pos--;
            } else if (line_pos < LINE_BUF_SIZE - 1) {
                line[line_pos++] = ch;
            }
        }

        print_system_stats();

        if (bt_a2dp_is_connected()) {
            if (!connected) {
                connected = true;
                ESP_LOGI(MAIN_TAG, "开始发送测试 PCM 音频");
            }
            if (!send_test_pcm()) {
                connected = false;
                ESP_LOGI(MAIN_TAG, "停止发送 PCM");
            }
        } else {
            if (connected) {
                connected = false;
                ESP_LOGI(MAIN_TAG, "停止发送 PCM");
            }
        }

        vTaskDelay(1);
    }
}
