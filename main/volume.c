#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "atomic_utils.h"
#include "volume.h"

#define VOL_NS     "player"
#define VOL_KEY    "volume"

static const char *TAG = "VOLUME";

static volatile int32_t s_volume = 64;
static int32_t          s_last_saved = -1;

int32_t volume_get(void)
{
    return atomic_load_i32(&s_volume);
}

void volume_set(int32_t v)
{
    if (v < VOLUME_MIN) v = VOLUME_MIN;
    if (v > VOLUME_MAX) v = VOLUME_MAX;
    atomic_store_i32(&s_volume, v);
}

void volume_inc(int32_t delta)
{
    volume_set(volume_get() + delta);
}

void volume_load_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(VOL_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    int32_t v = VOLUME_MIN;
    esp_err_t ret = nvs_get_i32(handle, VOL_KEY, &v);
    nvs_close(handle);

    if (ret == ESP_OK && v >= VOLUME_MIN && v <= VOLUME_MAX) {
        atomic_store_i32(&s_volume, v);
        s_last_saved = v;
        ESP_LOGI(TAG, "已从 NVS 恢复音量: %d", v);
    }
}

void volume_save_to_nvs(void)
{
    int32_t v = volume_get();
    if (v == s_last_saved) return;

    nvs_handle_t handle;
    if (nvs_open(VOL_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败");
        return;
    }

    esp_err_t ret = nvs_set_i32(handle, VOL_KEY, v);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret == ESP_OK) {
        s_last_saved = v;
        ESP_LOGI(TAG, "已保存音量: %d", v);
    } else {
        ESP_LOGW(TAG, "保存失败 (%s)", esp_err_to_name(ret));
    }
}
