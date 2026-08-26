#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "diskio.h"
#include "ff.h"
#include <stdio.h>
#include "esp_heap_caps.h"
#include "atomic_utils.h"
#include "sys_monitor.h"
#include "music_scanner.h"
#include "volume.h"

#define TAG_SDMMC   "SDMMC"
#define TAG_DETECT  "SD_DETECT"
#define TAG_ADC     "SYS_MON"

#define MOUNT_POINT  "/sdcard"
#define PIN_SD_DETECT 13
#define PIN_VOL_UP    36
#define PIN_VOL_DOWN  38
#define VOLUME_STEP   8

#define PIN_CLK 14
#define PIN_CMD 15
#define PIN_D0   2

#define VBAT_ADC_UNIT   ADC_UNIT_1
#define VBAT_ADC_CHAN   ADC_CHANNEL_7

#define SAMPLE_COUNT  10
#define SAMPLE_DELAY_MS 10
#define SENSOR_INTERVAL_TICKS 25

#define CPU_TEMP_OFFSET_C  30.0f
#define MUSIC_CACHE        "/sdcard/.music_cache"
#define FS_CACHE_MAGIC     0x4D555349

volatile bool  g_sd_ready = false;
volatile float g_vbat     = 0.0f;
volatile float g_cpu_temp = 0.0f;
fs_cache_t     *g_fs_cache     = NULL;

static adc_oneshot_unit_handle_t s_adc_handle = NULL;

static sdmmc_card_t *s_card    = NULL;
static bool          s_mounted = false;

static fs_cache_t   *s_fs_cache_owned = NULL;

static sd_event_cb_t  s_event_cb  = NULL;
static void          *s_event_ctx = NULL;

void fs_build_real_path(const char *group, const char *name,
                        char *out, size_t out_size)
{
    const char *rel = group;
    if (strncmp(rel, "sdcard", 6) == 0) {
        rel += 6;
        if (*rel == '_') rel++;
    }
    if (*rel == '\0') {
        snprintf(out, out_size, "/sdcard/%s", name);
    } else {
        snprintf(out, out_size, "/sdcard/%s/%s", rel, name);
        for (char *p = out; *p; p++) {
            if (*p == '_') *p = '/';
        }
    }
}

static fs_cache_t *sd_load_cache_to_psram(void)
{
    DIR *dir = opendir(MUSIC_CACHE);
    if (!dir) {
        ESP_LOGW(TAG_SDMMC, "无法打开缓存目录");
        return NULL;
    }

    int total = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        const char *name = entry->d_name;
        const char *ext = strrchr(name, '.');
        if (!ext || strcmp(ext, ".txt") != 0) continue;
        if (strcmp(name, "space.dat") == 0) continue;

        if (strcmp(name, "sdcard.txt") == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/sdcard.txt", MUSIC_CACHE);
            FILE *f = fopen(path, "r");
            if (f) {
                char line[FS_NAME_MAX];
                while (fgets(line, sizeof(line), f)) {
                    size_t len = strlen(line);
                    while (len > 0 && (line[len-1]=='\n' || line[len-1]=='\r'))
                        line[--len] = '\0';
                    if (len > 0) total++;
                }
                fclose(f);
            }
        }

        if (strncmp(name, "sdcard_", 7) == 0) {
            total++;  // directory entry

            char path[512];
            snprintf(path, sizeof(path), "%s/%s", MUSIC_CACHE, name);
            FILE *f = fopen(path, "r");
            if (f) {
                char line[FS_NAME_MAX];
                while (fgets(line, sizeof(line), f)) {
                    size_t len = strlen(line);
                    while (len > 0 && (line[len-1]=='\n' || line[len-1]=='\r'))
                        line[--len] = '\0';
                    if (len > 0) total++;
                }
                fclose(f);
            }
        }
    }
    closedir(dir);

    if (total == 0) {
        ESP_LOGW(TAG_SDMMC, "缓存无条目");
        return NULL;
    }

    size_t alloc_size = sizeof(fs_cache_t) + (size_t)total * sizeof(fs_entry_t);
    fs_cache_t *cache = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
    if (!cache) {
        ESP_LOGE(TAG_SDMMC, "PSRAM 分配失败: %zu 字节", alloc_size);
        return NULL;
    }

    cache->magic = FS_CACHE_MAGIC;
    cache->count = 0;

    dir = opendir(MUSIC_CACHE);
    if (!dir) {
        heap_caps_free(cache);
        return NULL;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        const char *name = entry->d_name;
        const char *ext = strrchr(name, '.');
        if (!ext || strcmp(ext, ".txt") != 0) continue;
        if (strcmp(name, "space.dat") == 0) continue;

        if (strncmp(name, "sdcard_", 7) == 0) {
            char group[FS_GROUP_MAX];
            strncpy(group, name, FS_GROUP_MAX - 1);
            group[FS_GROUP_MAX - 1] = '\0';
            char *dot = strrchr(group, '.');
            if (dot) *dot = '\0';

            strncpy(cache->entries[cache->count].name, group + 7, FS_NAME_MAX - 1);
            cache->entries[cache->count].name[FS_NAME_MAX - 1] = '\0';
            strncpy(cache->entries[cache->count].group, "sdcard", FS_GROUP_MAX - 1);
            cache->entries[cache->count].is_dir = true;
            cache->count++;
        }
    }
    closedir(dir);

    dir = opendir(MUSIC_CACHE);
    if (!dir) {
        heap_caps_free(cache);
        return NULL;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        const char *name = entry->d_name;
        const char *ext = strrchr(name, '.');
        if (!ext || strcmp(ext, ".txt") != 0) continue;
        if (strcmp(name, "space.dat") == 0) continue;

        if (strcmp(name, "sdcard.txt") == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/sdcard.txt", MUSIC_CACHE);
            FILE *f = fopen(path, "r");
            if (f) {
                char line[FS_NAME_MAX];
                while (fgets(line, sizeof(line), f)) {
                    size_t len = strlen(line);
                    while (len > 0 && (line[len-1]=='\n' || line[len-1]=='\r'))
                        line[--len] = '\0';
                    if (len > 0) {
                        strncpy(cache->entries[cache->count].name, line, FS_NAME_MAX - 1);
                        cache->entries[cache->count].name[FS_NAME_MAX - 1] = '\0';
                        strncpy(cache->entries[cache->count].group, "sdcard", FS_GROUP_MAX - 1);
                        cache->entries[cache->count].is_dir = false;
                        cache->count++;
                    }
                }
                fclose(f);
            }
        }

        if (strncmp(name, "sdcard_", 7) == 0) {
            char group[FS_GROUP_MAX];
            strncpy(group, name, FS_GROUP_MAX - 1);
            group[FS_GROUP_MAX - 1] = '\0';
            char *dot = strrchr(group, '.');
            if (dot) *dot = '\0';

            char path[512];
            snprintf(path, sizeof(path), "%s/%s", MUSIC_CACHE, name);
            FILE *f = fopen(path, "r");
            if (f) {
                char line[FS_NAME_MAX];
                while (fgets(line, sizeof(line), f)) {
                    size_t len = strlen(line);
                    while (len > 0 && (line[len-1]=='\n' || line[len-1]=='\r'))
                        line[--len] = '\0';
                    if (len > 0) {
                        strncpy(cache->entries[cache->count].name, line, FS_NAME_MAX - 1);
                        cache->entries[cache->count].name[FS_NAME_MAX - 1] = '\0';
                        strncpy(cache->entries[cache->count].group, group, FS_GROUP_MAX - 1);
                        cache->entries[cache->count].is_dir = false;
                        cache->count++;
                    }
                }
                fclose(f);
            }
        }
    }
    closedir(dir);

    ESP_LOGI(TAG_SDMMC, "PSRAM 缓存已加载: %d 条目, %zu 字节",
             cache->count, alloc_size);
    return cache;
}

static void sd_scan_files(void)
{
    int count = 0;

    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG_SDMMC, "无法打开目录");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            count++;
        }
    }
    closedir(dir);

    FATFS *fs;
    DWORD free_clst;
    FRESULT fr = f_getfree(MOUNT_POINT, &free_clst, &fs);
    if (fr == FR_OK) {
        uint64_t total_kb = ((uint64_t)fs->n_fatent - 2) * fs->csize / 2;
        uint64_t free_kb  = (uint64_t)free_clst * fs->csize / 2;
        ESP_LOGI(TAG_SDMMC, "存储: 总量 %llu KB, 剩余 %llu KB",
                 total_kb, free_kb);
    }
}

bool fs_cache_find_by_path(const char *path,
                           char *group_out, size_t group_size,
                           char *name_out, size_t name_size)
{
    if (!path || !path[0] || !g_fs_cache) return false;

    for (int i = 0; i < g_fs_cache->count; i++) {
        fs_entry_t *e = &g_fs_cache->entries[i];
        if (e->is_dir) continue;

        char real[512];
        fs_build_real_path(e->group, e->name, real, sizeof(real));
        if (strcmp(real, path) == 0) {
            if (group_out && group_size > 0) {
                strncpy(group_out, e->group, group_size - 1);
                group_out[group_size - 1] = '\0';
            }
            if (name_out && name_size > 0) {
                strncpy(name_out, e->name, name_size - 1);
                name_out[name_size - 1] = '\0';
            }
            return true;
        }
    }
    return false;
}

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
        ESP_LOGE(TAG_SDMMC, "挂载失败 (%s)", esp_err_to_name(ret));
        atomic_store_bool(&g_sd_ready, false);
        if (s_event_cb) s_event_cb("mount_failed", s_event_ctx);
        return;
    }

    sdmmc_card_print_info(stdout, s_card);
    s_mounted = true;
    ESP_LOGI(TAG_SDMMC, "SD 卡已挂载");

    sd_scan_files();
    music_scanner_init();

    s_fs_cache_owned = sd_load_cache_to_psram();
    g_fs_cache = s_fs_cache_owned;
    atomic_store_bool(&g_sd_ready, true);

    if (s_event_cb) s_event_cb("mounted", s_event_ctx);
}

void sdmmc_disk_deinit(void)
{
    if (!s_mounted) return;

    atomic_store_bool(&g_sd_ready, false);

    /* 先置空缓存指针再释放, LVGL 并发访问只会读到 NULL, 不会读已释放内存 */
    g_fs_cache = NULL;
    if (s_fs_cache_owned) {
        heap_caps_free(s_fs_cache_owned);
        s_fs_cache_owned = NULL;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SDMMC, "卸载失败 (%s)", esp_err_to_name(ret));
    }

    s_card    = NULL;
    s_mounted = false;
    ESP_LOGI(TAG_SDMMC, "SD 卡已卸载");

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

extern uint8_t temprature_sens_read(void);

static float read_cpu_temp(void)
{
    return ((float)temprature_sens_read() - 32.0f) / 1.8f - CPU_TEMP_OFFSET_C;
}

static float read_vbat(void)
{
    int raw;
    adc_oneshot_read(s_adc_handle, VBAT_ADC_CHAN, &raw);
    return (float)raw / 4095.0f * 3.3f * 2.0f;
}

static void sample_sensors(void)
{
    float temp_sum = 0.0f;
    float vbat_sum = 0.0f;

    for (int i = 0; i < SAMPLE_COUNT; i++) {
        temp_sum += read_cpu_temp();
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_DELAY_MS));

        vbat_sum += read_vbat();
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_DELAY_MS));
    }

    float temp_avg = temp_sum / (float)SAMPLE_COUNT;
    float vbat_avg = vbat_sum / (float)SAMPLE_COUNT;

    atomic_store_float(&g_cpu_temp, temp_avg);
    atomic_store_float(&g_vbat, vbat_avg);
}

static void sys_monitor_task(void *arg)
{
    gpio_set_direction(PIN_SD_DETECT, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_VOL_UP, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_VOL_DOWN, GPIO_MODE_INPUT);

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = VBAT_ADC_UNIT,
    };
    adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(s_adc_handle, VBAT_ADC_CHAN, &chan_cfg);

    bool last = (gpio_get_level(PIN_SD_DETECT) == 1);
    int  tick = SENSOR_INTERVAL_TICKS;

    ESP_LOGI(TAG_DETECT, "启动, GPIO%d 初始=%s", PIN_SD_DETECT, last ? "未插入" : "已插入");

    if (!last) {
        sdmmc_disk_init();
    }

    while (1) {
        bool current = (gpio_get_level(PIN_SD_DETECT) == 1);

        if (last && !current) {
            ESP_LOGI(TAG_DETECT, "检测到 SD 卡插入");
            vTaskDelay(pdMS_TO_TICKS(500));
            sdmmc_disk_init();
        }

        if (!last && current) {
            ESP_LOGI(TAG_DETECT, "检测到 SD 卡拔出");
            sdmmc_disk_deinit();
        }

        last = current;

        /* 音量按键: 高电平(外部下拉)即增减 */
        if (gpio_get_level(PIN_VOL_UP) == 1) {
            volume_inc(VOLUME_STEP);
        }
        if (gpio_get_level(PIN_VOL_DOWN) == 1) {
            volume_inc(-VOLUME_STEP);
        }

        if (++tick >= SENSOR_INTERVAL_TICKS) {
            tick = 0;
            sample_sensors();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void sys_monitor_init(void)
{
    xTaskCreatePinnedToCore(sys_monitor_task, "sys_monitor", 8192, NULL, 1, NULL, 1);
}
