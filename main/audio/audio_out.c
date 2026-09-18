#include "audio_out.h"

#include "board_config.h"
#include "board_i2c.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "audio_out"

/* 与解码输出一致: 44.1kHz / 16bit / 立体声 (见 pcm_pipeline.cpp) */
#define AOUT_SAMPLE_RATE  44100
#define AOUT_BITS         16
#define AOUT_CHANNELS     2

#define AOUT_STREAM_BYTES (24 * 1024)   /* PCM 流缓冲: 与原先送给 A2DP 的容量一致 */
#define AOUT_CHUNK_BYTES  4096          /* 单次搬运块大小 */

static StreamBufferHandle_t  s_pcm_stream = NULL;
static esp_codec_dev_handle_t s_dev       = NULL;
static bool                   s_ready     = false;
static int                    s_volume    = 70;

/* 输出任务: 从 PCM 流缓冲取数据写入 ES8311 */
static void audio_out_task(void *arg)
{
    (void)arg;
    uint8_t *buf = heap_caps_malloc(AOUT_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "输出缓冲分配失败");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        size_t n = xStreamBufferReceive(s_pcm_stream, buf, AOUT_CHUNK_BYTES, pdMS_TO_TICKS(20));
        if (n > 0 && s_dev != NULL) {
            esp_codec_dev_write(s_dev, buf, n);
        }
    }
}

StreamBufferHandle_t audio_out_init(void)
{
    if (s_pcm_stream != NULL) return s_pcm_stream;

    /* ── I2S: 主模式, MCLK = 256 × fs ── */
    i2s_chan_config_t chan_cfg = {
        .id                   = BOARD_I2S_PORT,
        .role                 = I2S_ROLE_MASTER,
        .dma_desc_num         = 6,
        .dma_frame_num        = 240,
        .auto_clear_after_cb  = true,
        .auto_clear_before_cb = false,
        .intr_priority        = 0,
    };
    i2s_chan_handle_t tx_handle = NULL;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AOUT_SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width  = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width  = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode       = I2S_SLOT_MODE_STEREO,
            .slot_mask       = I2S_STD_SLOT_BOTH,
            .ws_width        = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol          = false,
            .bit_shift       = true,
        },
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws   = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din  = GPIO_NUM_NC,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    /* ── esp_codec_dev: 数据口(I2S) + 控制口(I2C) + ES8311 ── */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = BOARD_I2S_PORT,
        .tx_handle = tx_handle,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = BOARD_CODEC_I2C_PORT,
        .addr       = BOARD_CODEC_ADDR,
        .bus_handle = board_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if    = ctrl_if,
        .gpio_if    = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,   /* 仅播放 */
        .pa_pin     = BOARD_PA_EN_GPIO,
        .use_mclk   = true,
        .hw_gain = {
            .pa_voltage        = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .pa_reverted = true,   /* 板载功放为低电平使能 */
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (s_dev == NULL) {
        ESP_LOGE(TAG, "ES8311 设备创建失败");
        return NULL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AOUT_BITS,
        .channel         = AOUT_CHANNELS,
        .channel_mask    = 0,
        .sample_rate     = AOUT_SAMPLE_RATE,
        .mclk_multiple   = 0,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(s_dev, &fs));
    esp_codec_dev_set_out_vol(s_dev, s_volume);

    s_pcm_stream = xStreamBufferCreate(AOUT_STREAM_BYTES, AOUT_CHUNK_BYTES);
    if (s_pcm_stream == NULL) {
        ESP_LOGE(TAG, "PCM 流缓冲创建失败");
        return NULL;
    }
    s_ready = true;

    xTaskCreatePinnedToCore(audio_out_task, "audio_out", 4096, NULL, 6, NULL, 1);
    ESP_LOGI(TAG, "本地输出就绪: I2S %dHz/%dbit/%dch → ES8311 → 喇叭",
             AOUT_SAMPLE_RATE, AOUT_BITS, AOUT_CHANNELS);
    return s_pcm_stream;
}

bool audio_out_is_ready(void)
{
    return s_ready;
}

int audio_out_volume_get(void)
{
    return s_volume;
}

void audio_out_volume_set(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    if (s_dev != NULL) {
        esp_codec_dev_set_out_vol(s_dev, s_volume);
    }
}

void audio_out_write(const int16_t *data, size_t samples)
{
    if (s_dev == NULL) return;
    esp_codec_dev_write(s_dev, (const uint8_t *)data, samples * sizeof(int16_t));
}
