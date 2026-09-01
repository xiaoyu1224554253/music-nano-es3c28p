#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "atomic_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FS_NAME_MAX   128    /* 文件名最大长度 */
#define FS_GROUP_MAX  256    /* 分组(目录)名最大长度 */

/* 文件系统缓存条目: 一个文件或目录 */
typedef struct {
    char name[FS_NAME_MAX];    /* 文件名 */
    char group[FS_GROUP_MAX];  /* 所属分组 (如 "sdcard" 或子目录名) */
    bool is_dir;               /* 是否目录 */
} fs_entry_t;

/* 文件系统缓存 (存 PSRAM, 变长数组): magic=有效性标记, count=条目数 */
typedef struct {
    uint32_t   magic;
    int        count;
    fs_entry_t entries[];   /* 变长数组 */
} fs_cache_t;

void sdmmc_disk_init(void);             /* 初始化 SD 卡 (探测+挂载) */
void sdmmc_disk_deinit(void);           /* 卸载 SD 卡 */
bool sdmmc_disk_is_mounted(void);       /* 查询 SD 卡是否已挂载 */

/* 手动触发重新扫描: 置位后由 sys_monitor 任务模拟"拔卡→插卡"走现有流程 */
extern volatile bool g_sd_manual_rescan;

/* 磁盘事件回调类型: event=事件串 (如 mount/unmount), user_data=用户数据 */
typedef void (*sd_event_cb_t)(const char *event, void *user_data);
void sdmmc_disk_set_event_callback(sd_event_cb_t cb, void *user_data);   /* 注册磁盘事件回调 */

void sys_monitor_init(void);    /* 启动系统监视任务 (电池/温度采样 + SD 管理) */

/* 音乐文件扫描 (storage/music_scan.c): 生成缓存文件列表 */
void music_scan_init(void);

/* 强制全量扫描标志: 置位后 music_scan_init 忽略空间阈值, 用完自动清除 */
extern volatile bool g_music_scan_force;

/* 供其他模块读取的共享状态 (原子/volatile) */
extern volatile bool   g_sd_ready;    /* SD 卡就绪标志 */
extern volatile float  g_vbat;        /* 电池电压 (V) */
extern volatile float  g_cpu_temp;    /* CPU 温度 (C) */
extern fs_cache_t     *g_fs_cache;    /* 文件缓存 (PSRAM) */

/* 由 group/name 拼出真实路径: group=分组, name=文件名, out=输出缓冲, out_size=缓冲大小 */
void fs_build_real_path(const char *group, const char *name,
                        char *out, size_t out_size);

/* 在缓存 g_fs_cache 中按真实路径查找文件, 找到则输出其 group/name.
 * path=完整路径; group_out/name_out=输出缓冲, *_size 为对应大小. 返回 true=找到. */
bool fs_cache_find_by_path(const char *path,
                           char *group_out, size_t group_size,
                           char *name_out, size_t name_size);

#ifdef __cplusplus
}
#endif
