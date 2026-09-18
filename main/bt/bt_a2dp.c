/* ESP32-S3 不支持经典蓝牙 (BR/EDR), 原 A2DP Source 发射在本芯片上无法实现
 * (CONFIG_BT_CLASSIC_ENABLED / CONFIG_BT_A2DP_ENABLE 在 esp32s3 下不存在).
 *
 * 本文件保留原 bt_a2dp.h 的对外接口形状, 内部改为本地音频输出
 * (I2S → ES8311 → 板载喇叭), 这样音频任务与 UI 无需改动:
 * 解码后的 PCM 仍写入 iface->pcm_stream, 由 audio_out 消费.
 * 蓝牙扫描/连接命令不再有实际作用 (命令被丢弃). */

#include "bt_a2dp.h"

#include "audio_out.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define TAG "audio_route"

static bt_a2dp_iface_t s_iface = {0};

bt_a2dp_iface_t *bt_a2dp_init(void)
{
    StreamBufferHandle_t stream = audio_out_init();

    s_iface.pcm_stream = stream;
    s_iface.cmd_queue  = xQueueCreate(4, sizeof(bt_cmd_t));
    s_iface.evt_queue  = xQueueCreate(4, sizeof(bt_evt_t));

    ESP_LOGI(TAG, "音频路由: 本地喇叭 (ES8311), 蓝牙不可用");
    return &s_iface;
}

/* 本地输出始终就绪 → 恒为"已连接" (UI/电源管理据此判断是否可以只息屏) */
bt_state_t bt_a2dp_get_state(void)
{
    return audio_out_is_ready() ? BT_STATE_CONNECTED : BT_STATE_DISCONNECTED;
}

bool bt_a2dp_is_connected(void)
{
    return audio_out_is_ready();
}
