#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "last_song.h"

#define LAST_NS    "player"
#define LAST_KEY   "song"
#define LAST_MAX   512

static const char *TAG = "LAST_SONG";

static char s_last[LAST_MAX] = {0};

void last_song_save(const char *path)
{
    if (!path || !path[0]) return;

    /* 路径未变化时跳过写 flash, 降低 NVS 磨损 */
    if (strcmp(s_last, path) == 0) return;

    nvs_handle_t handle;
    if (nvs_open(LAST_NS, NVS_READWRITE, &handle) != ESP_OK) {
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
    if (nvs_open(LAST_NS, NVS_READONLY, &handle) != ESP_OK) {
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
