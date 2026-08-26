#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_disk.h"

#define TAG "SDMMC"
#define MOUNT_POINT "/sdcard"

#define PIN_CLK 14
#define PIN_CMD 15
#define PIN_D0   2

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

static sd_event_cb_t  s_event_cb  = NULL;
static void          *s_event_ctx = NULL;

void sdmmc_disk_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = PIN_CLK;
    slot_config.cmd = PIN_CMD;
    slot_config.d0  = PIN_D0;
#endif

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "挂载失败 (%s)", esp_err_to_name(ret));
        if (s_event_cb) s_event_cb("mount_failed", s_event_ctx);
        return;
    }

    sdmmc_card_print_info(stdout, s_card);
    s_mounted = true;
    ESP_LOGI(TAG, "SD 卡已挂载");

    if (s_event_cb) s_event_cb("mounted", s_event_ctx);
}

void sdmmc_disk_deinit(void)
{
    if (!s_mounted) return;

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "卸载失败 (%s)", esp_err_to_name(ret));
        return;
    }

    s_card    = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "SD 卡已卸载");

    if (s_event_cb) s_event_cb("unmounted", s_event_ctx);
}

bool sdmmc_disk_is_mounted(void)
{
    return s_mounted;
}

void sdmmc_disk_set_event_callback(sd_event_cb_t cb, void *user_data)
{
    s_event_cb  = cb;
    s_event_ctx = user_data;
}
