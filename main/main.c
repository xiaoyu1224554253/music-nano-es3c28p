#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "app.h"
#include "drv_display.h"
#include "power_mgr.h"
#include "ui_core.h"
#include "audio_task.h"
#include "bt_a2dp.h"
#include "sys_monitor.h"
#include "cover.h"

static QueueHandle_t s_app_cmd_queue  = NULL;
static QueueHandle_t s_audio_cmd_queue = NULL;
static QueueHandle_t s_audio_rsp_queue = NULL;

void app_main(void)
{
    /* 最先: 开外设供电 + LCD 前半段 (SPI/面板/SLPOUT, 非阻塞), 让 120ms 在启动期间流逝 */
    power_mgr_early_init();
    lcd_init_early(SPI2_HOST);

    nvs_flash_init();

    s_app_cmd_queue  = xQueueCreate(10, sizeof(app_cmd_t));
    s_audio_cmd_queue = xQueueCreate(10, sizeof(audio_cmd_t));
    s_audio_rsp_queue = xQueueCreate(5,  sizeof(audio_rsp_t));

    bt_a2dp_iface_t *bt_iface = bt_a2dp_init();
    if (!bt_iface) {
        printf("FATAL: 蓝牙初始化失败\n");
        return;
    }

    console_init(s_app_cmd_queue);

    ui_params_t ui_params = {
        .app_cmd_queue   = s_app_cmd_queue,
        .bt_iface        = bt_iface,
        .audio_cmd_queue = s_audio_cmd_queue,
        .audio_rsp_queue = s_audio_rsp_queue,
    };
    ui_core_init(&ui_params);

    audio_task_params_t audio_params = {
        .cmd_queue  = s_audio_cmd_queue,
        .rsp_queue  = s_audio_rsp_queue,
        .pcm_stream = bt_iface->pcm_stream,
    };
    audio_task_init(&audio_params);

    sys_monitor_init();

    cover_init(s_app_cmd_queue);

    printf("\n系统就绪 | 输入命令: stats | ram | psram | vbat | temp | scan | conn <名称> | disconn | play | stop | pause | info\n");

    vTaskSuspend(NULL);
}
