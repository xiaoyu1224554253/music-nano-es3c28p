#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "atomic_utils.h"
#include "settings.h"

#define SETTINGS_NS  "player"

static const char *TAG = "SETTINGS";

/* ──────────────────────────── 音量 ──────────────────────────── */
#define VOL_KEY    "volume"

static volatile int32_t s_volume = 64;
static int32_t          s_vol_last_saved = -1;

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
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    int32_t v = VOLUME_MIN;
    esp_err_t ret = nvs_get_i32(handle, VOL_KEY, &v);
    nvs_close(handle);

    if (ret == ESP_OK && v >= VOLUME_MIN && v <= VOLUME_MAX) {
        atomic_store_i32(&s_volume, v);
        s_vol_last_saved = v;
        ESP_LOGI(TAG, "已从 NVS 恢复音量: %d", v);
    }
}

void volume_save_to_nvs(void)
{
    int32_t v = volume_get();
    if (v == s_vol_last_saved) return;

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败");
        return;
    }

    esp_err_t ret = nvs_set_i32(handle, VOL_KEY, v);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret == ESP_OK) {
        s_vol_last_saved = v;
        ESP_LOGI(TAG, "已保存音量: %d", v);
    } else {
        ESP_LOGW(TAG, "保存失败 (%s)", esp_err_to_name(ret));
    }
}

/* ──────────────────────── 上次播放歌曲 ──────────────────────── */
#define LAST_KEY   "song"
#define LAST_MAX   512

static char s_last[LAST_MAX] = {0};

void last_song_save(const char *path)
{
    if (!path || !path[0]) return;

    /* 路径未变化时跳过写 flash, 降低 NVS 磨损 */
    if (strcmp(s_last, path) == 0) return;

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败");
        return;
    }

    esp_err_t ret = nvs_set_str(handle, LAST_KEY, path);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret == ESP_OK) {
        strncpy(s_last, path, sizeof(s_last) - 1);
        s_last[sizeof(s_last) - 1] = '\0';
        ESP_LOGI(TAG, "已保存播放路径: %s", path);
    } else {
        ESP_LOGW(TAG, "保存失败 (%s)", esp_err_to_name(ret));
    }
}

bool last_song_load(char *buf, size_t size)
{
    if (!buf || size == 0) return false;

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t len = size;
    esp_err_t ret = nvs_get_str(handle, LAST_KEY, buf, &len);
    nvs_close(handle);

    if (ret != ESP_OK || len == 0 || buf[0] == '\0') {
        return false;
    }

    strncpy(s_last, buf, sizeof(s_last) - 1);
    s_last[sizeof(s_last) - 1] = '\0';
    return true;
}

/* ──────────────────────────── 播放模式 ──────────────────────────── */
#define MODE_KEY   "play_mode"

bool settings_mode_load(play_mode_t *mode)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    int32_t m = PLAY_MODE_SEQUENTIAL;
    esp_err_t ret = nvs_get_i32(h, MODE_KEY, &m);
    nvs_close(h);

    if (ret == ESP_OK && m >= PLAY_MODE_SEQUENTIAL && m <= PLAY_MODE_RANDOM) {
        *mode = (play_mode_t)m;
        return true;
    }
    return false;
}

void settings_mode_save(play_mode_t mode)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, MODE_KEY, (int32_t)mode);
        nvs_commit(h);
        nvs_close(h);
    }
}
