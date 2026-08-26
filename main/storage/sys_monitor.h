#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "atomic_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FS_NAME_MAX   128
#define FS_GROUP_MAX  256

typedef struct {
    char name[FS_NAME_MAX];
    char group[FS_GROUP_MAX];
    bool is_dir;
} fs_entry_t;

typedef struct {
    uint32_t   magic;
    int        count;
    fs_entry_t entries[];
} fs_cache_t;

void sdmmc_disk_init(void);
void sdmmc_disk_deinit(void);
bool sdmmc_disk_is_mounted(void);

typedef void (*sd_event_cb_t)(const char *event, void *user_data);
void sdmmc_disk_set_event_callback(sd_event_cb_t cb, void *user_data);

void sys_monitor_init(void);

/* 音乐文件扫描 (storage/music_scan.c): 生成缓存文件列表 */
void music_scan_init(void);

extern volatile bool   g_sd_ready;
extern volatile float  g_vbat;
extern volatile float  g_cpu_temp;
extern fs_cache_t     *g_fs_cache;

void fs_build_real_path(const char *group, const char *name,
                        char *out, size_t out_size);

/* 在缓存 g_fs_cache 中按真实路径查找文件, 找到则输出其 group/name */
bool fs_cache_find_by_path(const char *path,
                           char *group_out, size_t group_size,
                           char *name_out, size_t name_size);

#ifdef __cplusplus
}
#endif
