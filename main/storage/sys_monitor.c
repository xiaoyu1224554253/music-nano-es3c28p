#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
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
#include "power_mgr.h"
#include "sys_monitor.h"

/* 安全拷贝: 把 src 复制到 dst (目标大小 dst_sz), 保证结尾 '\0' */
static inline void buf_copy(char *dst, size_t dst_sz, const char *src)
{
    if (dst_sz == 0) {
        return;
    }
    size_t n = strnlen(src, dst_sz - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

#define TAG_SDMMC   "SDMMC"
#define TAG_DETECT  "SD_DETECT"

#define MOUNT_POINT  "/sdcard"       /* SD 卡挂载点 */
#define PIN_SD_DETECT 13             /* SD 卡插入检测引脚 (高=未插入) */

#define PIN_CLK 14                   /* SDMMC 时钟脚 */
#define PIN_CMD 15                   /* SDMMC 命令脚 */
#define PIN_D0   2                   /* SDMMC 数据线 (1bit 模式只需 D0) */

#define SAMPLE_COUNT  10             /* 采样次数 (取平均滤波) */
#define SAMPLE_DELAY_MS 10           /* 相邻两次采样间隔 */
#define SENSOR_INTERVAL_TICKS 10     /* 传感器采样周期计数 (×100ms = 1s) */

#define CPU_TEMP_OFFSET_C  20.0f     /* CPU 内部温度传感器校准偏移 */
#define VBAT_CRITICAL_LOW_V  3.25f   /* 运行中临界电压 (低于此值直接深睡关机) */
#define MUSIC_CACHE        "/sdcard/.music_cache"   /* 音乐缓存目录 */
#define FS_CACHE_MAGIC     0x4D555349              /* 文件缓存魔数 "MUSI" */

/* 跨模块共享状态 */
volatile bool  g_sd_ready = false;      /* SD 卡就绪标志 */
volatile float g_vbat     = 0.0f;       /* 电池电压 (V) */
volatile float g_cpu_temp = 0.0f;       /* CPU 温度 (C) */
volatile bool  g_sd_manual_rescan = false;  /* 手动重扫标志 */
fs_cache_t     *g_fs_cache     = NULL;      /* 文件缓存指针 (PSRAM) */

static sdmmc_card_t *s_card    = NULL;   /* SD 卡信息结构体 */
static bool          s_mounted = false;  /* 是否已挂载 */

static fs_cache_t   *s_fs_cache_owned = NULL;   /* 本模块持有的缓存指针 (用于释放) */

static sd_event_cb_t  s_event_cb  = NULL;   /* 磁盘事件回调 */
static void          *s_event_ctx = NULL;   /* 回调用户数据 */

/* 由 group/name 拼出真实路径:
 * group="sdcard"        → /sdcard/name
 * group="sdcard%a%b"    → /sdcard/a/b/name
 * 只把 group 段的 '%' 还原为 '/', 文件名里的 '%' 保持原样 */
void fs_build_real_path(const char *group, const char *name,
                        char *out, size_t out_size)
{
    const char *rel = group;
    if (strncmp(rel, "sdcard", 6) == 0) {   /* 去掉 "sdcard" 前缀 */
        rel += 6;
        if (*rel == '%') rel++;             /* 去掉分组分隔符 '%' */
    }
    if (*rel == '\0') {
        snprintf(out, out_size, "/sdcard/%s", name);   /* 根目录文件 */
        return;
    }

    snprintf(out, out_size, "/sdcard/%s/%s", rel, name);   /* 子目录文件 */
    char *start    = out + strlen("/sdcard/");              /* rel 段起点 */
    char *name_pos = start + strlen(rel);                   /* rel 段终点 (文件名前) */
    char *end      = out + strlen(out);
    if (name_pos > end) name_pos = end;                     /* snprintf 截断保护 */
    for (char *p = start; p < name_pos; p++) {
        if (*p == '%') *p = '/';   /* 仅 group 段的 '%' 还原为路径分隔符 */
    }
}

/* 把 SD 卡上的音乐缓存文件读入 PSRAM 构建文件索引.
 * 三遍扫描: ①数条目数 + 统计字符串池大小 ②登记子目录 ③登记文件.
 * 分配为 [表头][条目数组][字符串池] 单块; name/group 指向池内,
 * 同目录的 group 串只存一份 (同目录所有文件共用), 避免每条各存一份.
 * 返回 fs_cache_t 或 NULL. */
static fs_cache_t *sd_load_cache_to_psram(void)
{
    DIR *dir = opendir(MUSIC_CACHE);
    if (!dir) {
        ESP_LOGW(TAG_SDMMC, "无法打开缓存目录");
        return NULL;
    }

    /* 第一遍: 统计总条目数 total 与字符串池字节数 pool_size */
    int total = 0;
    size_t pool_size = 7;   /* 共享根分组串 "sdcard" (含结尾 '\0') */
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        const char *name = entry->d_name;
        const char *ext = strrchr(name, '.');
        if (!ext || strcmp(ext, ".txt") != 0) continue;   /* 只要 .txt 缓存文件 */
        if (strcmp(name, "space.dat") == 0) continue;     /* 跳过空间快照文件 */

        if (strcmp(name, "sdcard.txt") == 0) {   /* 根目录缓存: 逐行统计文件数 */
            char path[512];
            snprintf(path, sizeof(path), "%s/sdcard.txt", MUSIC_CACHE);
            FILE *f = fopen(path, "r");
            if (f) {
                char line[FS_NAME_MAX];
                while (fgets(line, sizeof(line), f)) {
                    size_t len = strlen(line);
                    while (len > 0 && (line[len-1]=='\n' || line[len-1]=='\r'))
                        line[--len] = '\0';   /* 去掉行尾换行 */
                    if (len > 0) { total++; pool_size += len + 1; }
                }
                fclose(f);
            }
        }

        if (strncmp(name, "sdcard%", 7) == 0) {   /* 子目录缓存 */
            total++;                                  /* 目录条目 */
            size_t glen = strlen(name) - 4;           /* 组名长度 (去 ".txt") */
            const char *last = strrchr(name, '%');    /* 最后一级分隔符 */
            size_t last_idx = (size_t)(last - name);
            size_t name_len = glen - (last_idx + 1);  /* 本级目录名长度 */
            pool_size += name_len + 1;                /* 目录名 */
            if (last_idx != 6) {                      /* 父目录非根 → 存父 group 串 */
                pool_size += last_idx + 1;
            }
            pool_size += glen + 1;                    /* 该目录文件的 group 串 (只存一份) */

            char path[512];
            snprintf(path, sizeof(path), "%s/%s", MUSIC_CACHE, name);
            FILE *f = fopen(path, "r");
            if (f) {
                char line[FS_NAME_MAX];
                while (fgets(line, sizeof(line), f)) {
                    size_t len = strlen(line);
                    while (len > 0 && (line[len-1]=='\n' || line[len-1]=='\r'))
                        line[--len] = '\0';
                    if (len > 0) { total++; pool_size += len + 1; }
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

    /* 一次性分配: 表头 + 条目数组 + 字符串池 (均在 PSRAM) */
    size_t alloc_size = sizeof(fs_cache_t) + (size_t)total * sizeof(fs_entry_t) + pool_size;
    fs_cache_t *cache = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
    if (!cache) {
        ESP_LOGE(TAG_SDMMC, "PSRAM 分配失败: %zu 字节", alloc_size);
        return NULL;
    }

    cache->magic = FS_CACHE_MAGIC;
    cache->count = 0;
    cache->pool_size = pool_size;

    char *pool = (char *)(cache->entries + total);   /* 字符串池紧随条目数组 */
    size_t po = 0;
    memcpy(pool, "sdcard", 7);                        /* 共享根分组串 */
    po = 7;
    const size_t sdcard_off = 0;                      /* "sdcard" 在池中的偏移 */

    /* 第二遍: 登记所有子目录 (目录条目: group=父目录, name=本级目录名) */
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

        if (strncmp(name, "sdcard%", 7) == 0) {
            size_t glen = strlen(name) - 4;           /* 组名长度 */
            const char *last = strrchr(name, '%');    /* 最后一级分隔符 */
            size_t last_idx = (size_t)(last - name);
            const char *disp = last + 1;              /* 本级目录名 */
            size_t dlen = glen - (last_idx + 1);

            fs_entry_t *e = &cache->entries[cache->count];
            memcpy(pool + po, disp, dlen);    /* 目录名写入池 */
            pool[po + dlen] = '\0';
            e->name = pool + po;
            po += dlen + 1;

            if (last_idx == 6) {              /* 顶层目录: 父=根 */
                e->group = pool + sdcard_off;
            } else {                          /* 父目录 group 串写入池 */
                memcpy(pool + po, name, last_idx);
                pool[po + last_idx] = '\0';
                e->group = pool + po;
                po += last_idx + 1;
            }
            e->is_dir = true;
            cache->count++;
        }
    }
    closedir(dir);

    /* 第三遍: 登记文件条目 */
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

        if (strcmp(name, "sdcard.txt") == 0) {   /* 根目录文件 */
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
                        fs_entry_t *e = &cache->entries[cache->count];
                        memcpy(pool + po, line, len);   /* 文件名写入池 */
                        pool[po + len] = '\0';
                        e->name   = pool + po;
                        e->group  = pool + sdcard_off;  /* 根分组 */
                        e->is_dir = false;
                        po += len + 1;
                        cache->count++;
                    }
                }
                fclose(f);
            }
        }

        if (strncmp(name, "sdcard%", 7) == 0) {   /* 子目录文件 */
            size_t glen = strlen(name) - 4;   /* 完整 group 串长度 */

            /* 该目录的 group 串只写一份, 同目录所有文件共用 */
            memcpy(pool + po, name, glen);
            pool[po + glen] = '\0';
            const char *group_ptr = pool + po;
            po += glen + 1;

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
                        fs_entry_t *e = &cache->entries[cache->count];
                        memcpy(pool + po, line, len);   /* 文件名写入池 */
                        pool[po + len] = '\0';
                        e->name   = pool + po;
                        e->group  = group_ptr;          /* 子目录分组 (共享) */
                        e->is_dir = false;
                        po += len + 1;
                        cache->count++;
                    }
                }
                fclose(f);
            }
        }
    }
    closedir(dir);

    ESP_LOGI(TAG_SDMMC, "PSRAM 缓存已加载: %d 条目, %zu 字节 (池 %zu)",
             cache->count, alloc_size, pool_size);
    return cache;
}

/* 扫描 SD 卡根目录: 打印文件数 + 容量信息 (诊断用) */
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
            count++;   /* 数根目录普通文件数 */
        }
    }
    closedir(dir);

    /* 容量信息 (FatFs 查询) */
    FATFS *fs;
    DWORD free_clst;
    FRESULT fr = f_getfree(MOUNT_POINT, &free_clst, &fs);
    if (fr == FR_OK) {
        uint64_t total_kb = ((uint64_t)fs->n_fatent - 2) * fs->csize / 2;   /* 总量 */
        uint64_t free_kb  = (uint64_t)free_clst * fs->csize / 2;            /* 剩余 */
        ESP_LOGI(TAG_SDMMC, "存储: 总量 %llu KB, 剩余 %llu KB",
                 total_kb, free_kb);
    }
}

/* 在缓存中按完整路径查找文件, 输出其 group/name (供 UI 定位分组) */
bool fs_cache_find_by_path(const char *path,
                           char *group_out, size_t group_size,
                           char *name_out, size_t name_size)
{
    if (!path || !path[0] || !g_fs_cache) return false;

    for (int i = 0; i < g_fs_cache->count; i++) {
        fs_entry_t *e = &g_fs_cache->entries[i];
        if (e->is_dir) continue;   /* 跳过目录条目 */

        char real[512];
        fs_build_real_path(e->group, e->name, real, sizeof(real));   /* 拼真实路径 */
        if (strcmp(real, path) == 0) {
            if (group_out && group_size > 0) {   /* 输出 group */
                buf_copy(group_out, group_size, e->group);
            }
            if (name_out && name_size > 0) {     /* 输出 name */
                buf_copy(name_out, name_size, e->name);
            }
            return true;
        }
    }
    return false;
}

void sdmmc_disk_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,   /* 挂载失败不格式化 (保护数据) */
        .max_files = 5,                    /* 同时打开文件数上限 */
        .allocation_unit_size = 16 * 1024, /* 分配单元 16KB */
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;                                /* 1bit 模式 */
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP; /* 使用内部上拉 */

#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX   /* 某些芯片 SDMMC 引脚可映射, 指定自定义引脚 */
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

    sdmmc_card_print_info(stdout, s_card);   /* 打印卡信息 (容量/类型等) */
    s_mounted = true;
    ESP_LOGI(TAG_SDMMC, "SD 卡已挂载");

    sd_scan_files();           /* 诊断: 根目录文件数/容量 */
    music_scan_init();         /* 扫描/加载音乐缓存 */

    s_fs_cache_owned = sd_load_cache_to_psram();   /* 把缓存读入 PSRAM 建索引 */
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

/* 注册磁盘事件回调: cb=回调函数, user_data=回调时透传的用户数据 */
void sdmmc_disk_set_event_callback(sd_event_cb_t cb, void *user_data)
{
    s_event_cb  = cb;
    s_event_ctx = user_data;
}

extern uint8_t temprature_sens_read(void);

/* 读取 CPU 内部温度 (华氏原始值 → 摄氏, 再减校准偏移) */
static float read_cpu_temp(void)
{
    return ((float)temprature_sens_read() - 32.0f) / 1.8f - CPU_TEMP_OFFSET_C;
}

/* 传感器采样: 多次取平均, 抑制 ADC 噪声.
 * 电池电压 ADC 由 power_mgr 持有, 这里复用其读取接口 (确保周期采样仍更新 g_vbat) */
static void sample_sensors(void)
{
    float temp_sum = 0.0f;
    float vbat_sum = 0.0f;

    for (int i = 0; i < SAMPLE_COUNT; i++) {
        temp_sum += read_cpu_temp();
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_DELAY_MS));

        vbat_sum += power_mgr_vbat_read_once();
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_DELAY_MS));
    }

    float temp_avg = temp_sum / (float)SAMPLE_COUNT;   /* 温度平均 */
    float vbat_avg = vbat_sum / (float)SAMPLE_COUNT;   /* 电压平均 */

    atomic_store_float(&g_cpu_temp, temp_avg);   /* 原子发布, 供 UI/控制台读取 */
    atomic_store_float(&g_vbat, vbat_avg);
}

static void sys_monitor_task(void *arg)
{
    gpio_set_direction(PIN_SD_DETECT, GPIO_MODE_INPUT);   /* SD 检测脚设为输入 */

    bool last = (gpio_get_level(PIN_SD_DETECT) == 1);   /* 初始检测电平 (true=未插入) */
    int  tick = SENSOR_INTERVAL_TICKS;                  /* 采样倒计时, 初始即采样 */

    ESP_LOGI(TAG_DETECT, "启动, GPIO%d 初始=%s", PIN_SD_DETECT, last ? "未插入" : "已插入");

    if (!last) {   /* 开机时卡已在 → 直接挂载 */
        sdmmc_disk_init();
    }

    while (1) {
        bool current = (gpio_get_level(PIN_SD_DETECT) == 1);   /* 当前电平 */

        /* 手动重扫: 模拟拔卡→插卡流程 */
        if (g_sd_manual_rescan) {
            g_sd_manual_rescan = false;
            ESP_LOGI(TAG_DETECT, "手动触发重新扫描");
            g_music_scan_force = true;               /* 强制音乐全量重扫 */
            sdmmc_disk_deinit();
            vTaskDelay(pdMS_TO_TICKS(500));
            sdmmc_disk_init();
            last = (gpio_get_level(PIN_SD_DETECT) == 1);
        }

        if (last && !current) {   /* 高→低: 插入 */
            ESP_LOGI(TAG_DETECT, "检测到 SD 卡插入");
            vTaskDelay(pdMS_TO_TICKS(500));   /* 等卡稳定 */
            sdmmc_disk_init();
        }

        if (!last && current) {   /* 低→高: 拔出 */
            ESP_LOGI(TAG_DETECT, "检测到 SD 卡拔出");
            sdmmc_disk_deinit();
        }

        last = current;

        if (++tick >= SENSOR_INTERVAL_TICKS) {   /* 每 SENSOR_INTERVAL_TICKS 次循环采一次传感器 */
            tick = 0;
            sample_sensors();

            /* 周期低压检测: 低于临界值 → 无动画直接深睡 (深度睡眠即重启系统, 无需善后) */
            if (g_vbat < VBAT_CRITICAL_LOW_V) {
                power_mgr_critical_shutdown();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));   /* 轮询周期 100ms */
    }
}

/* 启动系统监视任务 (固定 core 1) */
void sys_monitor_init(void)
{
    xTaskCreatePinnedToCore(sys_monitor_task, "sys_monitor", 8192, NULL, 1, NULL, 1);
}
