#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"

/* 安全拷贝: 把 src 复制到 dst (目标大小 dst_sz), 保证结尾 '\0'.
 * 相比 strncpy 语义清晰: 始终以 dst_sz 为上限, 不填充多余 '\0'. */
static inline void buf_copy(char *dst, size_t dst_sz, const char *src)
{
    if (dst_sz == 0) {
        return;
    }
    size_t n = strnlen(src, dst_sz - 1);   /* 计算不超过缓冲大小的源串长度 */
    memcpy(dst, src, n);
    dst[n] = '\0';
}

#define TAG_MUSIC_SCAN   "MUSIC_SCAN"

#define MOUNT_POINT       "/sdcard"              /* SD 卡挂载点 */
#define MUSIC_CACHE_DIR   "/sdcard/.music_cache" /* 缓存目录 (隐藏在 SD 卡上) */
#define SPACE_FILE        "/sdcard/.music_cache/space.dat"  /* 空间快照文件 */
#define SPACE_THRESHOLD_KB 10                   /* 空间变化超过此值才重扫 */
#define PATH_BUF_SIZE     384                    /* 每条路径缓冲 */
#define SUBDIR_INIT       8                      /* 子目录列表初始容量 */

volatile bool g_music_scan_force = false;   /* 强制全量扫描标志 (外部置位, 用完自动清除) */

/* 识别为音乐文件的扩展名 (含大小写) */
static const char *MUSIC_EXTENSIONS[] = {
    ".mp3", ".flac", ".wav", ".aac",
    ".MP3", ".FLAC", ".WAV", ".AAC",
};
static const int NUM_EXTENSIONS = sizeof(MUSIC_EXTENSIONS) / sizeof(MUSIC_EXTENSIONS[0]);

/* 定长路径列表: 所有路径等宽存储 (PATH_BUF_SIZE), 便于按索引寻址 */
typedef struct {
    char *buf;       /* 连续缓冲 */
    int   count;     /* 已用条目数 */
    int   capacity;  /* 可容纳条目数 */
} path_list_t;

/* 初始化路径列表, 分配 SUBDIR_INIT 条空间 */
static bool path_list_init(path_list_t *list)
{
    list->capacity = SUBDIR_INIT;
    list->count = 0;
    list->buf = malloc(PATH_BUF_SIZE * list->capacity);
    if (!list->buf) {
        ESP_LOGE(TAG_MUSIC_SCAN, "内存分配失败");
        return false;
    }
    return true;
}

/* 追加一条路径; 容量不足时倍增扩容 */
static bool path_list_add(path_list_t *list, const char *path)
{
    if (list->count >= list->capacity) {
        int new_cap = list->capacity * 2;
        char *new_buf = realloc(list->buf, PATH_BUF_SIZE * new_cap);
        if (!new_buf) {
            ESP_LOGE(TAG_MUSIC_SCAN, "内存扩展失败");
            return false;
        }
        list->buf = new_buf;
        list->capacity = new_cap;
    }
    char *dst = list->buf + (size_t)list->count * PATH_BUF_SIZE;   /* 定位第 count 条 */
    buf_copy(dst, PATH_BUF_SIZE, path);
    list->count++;
    return true;
}

/* 按索引取路径 */
static inline const char *path_list_get(path_list_t *list, int idx)
{
    return list->buf + (size_t)idx * PATH_BUF_SIZE;
}

/* 释放列表内存 */
static void path_list_free(path_list_t *list)
{
    free(list->buf);
    list->buf = NULL;
    list->count = 0;
    list->capacity = 0;
}

/* 判断文件名是否为音乐文件 (按扩展名匹配) */
static bool is_music_file(const char *name)
{
    const char *dot = strrchr(name, '.');   /* 最后一个点作为扩展名起点 */
    if (!dot) return false;

    for (int i = 0; i < NUM_EXTENSIONS; i++) {
        if (strcmp(dot, MUSIC_EXTENSIONS[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* 把目录路径映射到缓存文件路径:
 * /sdcard          → .music_cache/sdcard.txt
 * /sdcard/sub/dir  → .music_cache/sdcard_sub%d_dir.txt   ('/' 换成 '%', 保持单一文件名) */
static void build_cache_path(const char *dir_path, char *out, size_t out_size)
{
    const char *rel = dir_path;
    size_t mount_len = strlen(MOUNT_POINT);

    if (strncmp(dir_path, MOUNT_POINT, mount_len) == 0) {
        rel = dir_path + mount_len;   /* 跳过挂载点前缀 */
        if (*rel == '/') rel++;
    }

    if (*rel == '\0') {
        snprintf(out, out_size, "%s/sdcard.txt", MUSIC_CACHE_DIR);
    } else {
        snprintf(out, out_size, "%s/sdcard_%s.txt", MUSIC_CACHE_DIR, rel);
        char *p = out + strlen(MUSIC_CACHE_DIR) + 1;   /* 定位到 "sdcard_" 之后 */
        while (*p) {
            if (*p == '/') *p = '%';   /* 目录分隔符替换为 %, 避免路径层级 */
            p++;
        }
    }
}

/* 获取 SD 卡已用空间 (KB), 用 FatFs 直接查 */
static uint64_t get_used_space_kb(void)
{
    FATFS *fs;
    DWORD free_clst;
    FRESULT fr = f_getfree(MOUNT_POINT, &free_clst, &fs);
    if (fr != FR_OK) {
        ESP_LOGE(TAG_MUSIC_SCAN, "获取空间信息失败 (%d)", fr);
        return 0;
    }

    /* n_fatent-2 = 有效簇数; csize/2 = 每簇扇区数→KB (扇区 512B) */
    uint64_t total_kb = ((uint64_t)fs->n_fatent - 2) * fs->csize / 2;
    uint64_t free_kb  = (uint64_t)free_clst * fs->csize / 2;
    return total_kb - free_kb;
}

#define SPACE_MAGIC  "MUSICACHE1\n"   /* 空间缓存文件魔数 */

/* 读取上次扫描时记录的已用空间 (KB); 无/损坏返回 0 */
static uint64_t read_space_cache(void)
{
    int fd = open(SPACE_FILE, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    char buf[48] = {0};
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (len <= (ssize_t)strlen(SPACE_MAGIC)) return 0;          /* 内容过短 */
    if (strncmp(buf, SPACE_MAGIC, strlen(SPACE_MAGIC)) != 0) return 0;   /* 魔数不符 */
    return strtoull(buf + strlen(SPACE_MAGIC), NULL, 10);       /* 解析数字 */
}

/* 把本次扫描的已用空间写入缓存文件 */
static void write_space_cache(uint64_t used_kb)
{
    int fd = open(SPACE_FILE, O_RDWR | O_CREAT | O_TRUNC, 0);
    if (fd < 0) {
        ESP_LOGE(TAG_MUSIC_SCAN, "无法创建空间缓存文件: %s", strerror(errno));
        return;
    }

    char buf[48];
    int len = snprintf(buf, sizeof(buf), SPACE_MAGIC "%llu", used_kb);
    write(fd, buf, len);
    close(fd);
}

/* 删除 .music_cache 下所有 sdcard*.txt 旧缓存文件 */
static void clean_cache_files(void)
{
    DIR *dir = opendir(MUSIC_CACHE_DIR);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;                /* 只处理普通文件 */
        if (strncmp(entry->d_name, "sdcard", 6) == 0) {      /* 前缀 sdcard 的都是本模块缓存 */
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s",
                     MUSIC_CACHE_DIR, entry->d_name);
            remove(full_path);
        }
    }
    closedir(dir);
}

/* 递归扫描目录 dir_path: 音乐文件名写入对应缓存文件, 子目录收集后递归 */
static void scan_dir(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG_MUSIC_SCAN, "无法打开目录: %s", dir_path);
        return;
    }

    char cache_path[512];
    build_cache_path(dir_path, cache_path, sizeof(cache_path));   /* 本目录的缓存文件路径 */

    int fd = open(cache_path, O_RDWR | O_CREAT | O_TRUNC, 0);     /* 覆盖写 */
    if (fd < 0) {
        ESP_LOGE(TAG_MUSIC_SCAN, "无法创建缓存文件: %s", cache_path);
        closedir(dir);
        return;
    }

    path_list_t subdirs;
    if (!path_list_init(&subdirs)) {
        close(fd);
        closedir(dir);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (strcmp(entry->d_name, ".music_cache") == 0) {
            continue;   /* 跳过缓存目录自身 */
        }

        if (strcmp(entry->d_name, "System Volume Information") == 0) {
            continue;   /* 跳过 Windows 系统目录 */
        }

        if (entry->d_type == DT_REG) {
            if (is_music_file(entry->d_name)) {
                /* 跳过 0 字节的损坏/残留文件 */
                char full[512];
                snprintf(full, sizeof(full), "%s/%s", dir_path, entry->d_name);
                struct stat st;
                if (stat(full, &st) == 0 && st.st_size > 0) {
                    write(fd, entry->d_name, strlen(entry->d_name));   /* 一行一个文件名 */
                    write(fd, "\n", 1);
                } else {
                    ESP_LOGW(TAG_MUSIC_SCAN, "跳过空文件: %s", full);
                }
            }
        } else if (entry->d_type == DT_DIR) {
            char sub_path[384];
            snprintf(sub_path, sizeof(sub_path), "%s/%s",
                     dir_path, entry->d_name);
            path_list_add(&subdirs, sub_path);   /* 记录子目录待递归 */
        }
    }

    close(fd);
    closedir(dir);

    for (int i = 0; i < subdirs.count; i++) {   /* 递归处理所有子目录 */
        scan_dir(path_list_get(&subdirs, i));
    }

    path_list_free(&subdirs);
}

/* 执行一次全量扫描: 清缓存 → 扫目录 → 记录空间快照 */
static void music_scan_run(void)
{
    ESP_LOGI(TAG_MUSIC_SCAN, "开始扫描音乐文件...");

    int64_t t0 = esp_timer_get_time();

    clean_cache_files();
    scan_dir(MOUNT_POINT);

    uint64_t used_kb = get_used_space_kb();
    write_space_cache(used_kb);

    int64_t elapsed = esp_timer_get_time() - t0;
    ESP_LOGI(TAG_MUSIC_SCAN, "扫描完成, 耗时 %.2f ms", elapsed / 1000.0f);
}

/* 音乐扫描初始化: 根据空间变化决定是否重扫 (除非被强制).
 * 策略: SD 卡已用空间与上次记录偏差 >10KB → 说明曲目有增删, 重扫; 否则用缓存, 秒开. */
void music_scan_init(void)
{
    ESP_LOGI(TAG_MUSIC_SCAN, "初始化音乐文件扫描器");

    if (mkdir(MUSIC_CACHE_DIR, 0777) != 0 && errno != EEXIST) {   /* 确保缓存目录存在 */
        ESP_LOGE(TAG_MUSIC_SCAN, "无法创建缓存目录 %s: %s", MUSIC_CACHE_DIR, strerror(errno));
        return;
    }

    uint64_t current_used = get_used_space_kb();   /* 当前已用空间 */
    uint64_t cached_used = read_space_cache();     /* 上次记录的已用空间 */

    ESP_LOGI(TAG_MUSIC_SCAN, "当前已用空间: %llu KB, 缓存记录: %llu KB",
             current_used, cached_used);

    bool force = g_music_scan_force;   /* 外部强制标志 (一次性) */
    g_music_scan_force = false;
    if (force) {
        ESP_LOGI(TAG_MUSIC_SCAN, "手动触发重新扫描");
        music_scan_run();
        return;
    }

    if (cached_used == 0) {   /* 首次运行, 无缓存记录 */
        ESP_LOGI(TAG_MUSIC_SCAN, "无缓存记录，开始全量扫描");
        music_scan_run();
        return;
    }

    /* 比较当前与记录的空间, 超过阈值才重扫 */
    int64_t diff = (int64_t)current_used - (int64_t)cached_used;
    if (diff < 0) diff = -diff;
    if (diff > SPACE_THRESHOLD_KB) {
        ESP_LOGI(TAG_MUSIC_SCAN, "空间变化 %lld KB 超过阈值 %d KB，重新扫描",
                 diff, SPACE_THRESHOLD_KB);
        music_scan_run();
    } else {
        ESP_LOGI(TAG_MUSIC_SCAN, "空间变化在阈值内，使用缓存");
    }
}
