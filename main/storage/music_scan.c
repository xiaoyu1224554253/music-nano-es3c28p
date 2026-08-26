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

#define TAG_MUSIC_SCAN   "MUSIC_SCAN"

#define MOUNT_POINT       "/sdcard"
#define MUSIC_CACHE_DIR   "/sdcard/.music_cache"
#define SPACE_FILE        "/sdcard/.music_cache/space.dat"
#define SPACE_THRESHOLD_KB 10
#define PATH_BUF_SIZE     384
#define SUBDIR_INIT       8

static const char *MUSIC_EXTENSIONS[] = {
    ".mp3", ".flac", ".wav", ".aac",
    ".MP3", ".FLAC", ".WAV", ".AAC",
};
static const int NUM_EXTENSIONS = sizeof(MUSIC_EXTENSIONS) / sizeof(MUSIC_EXTENSIONS[0]);

typedef struct {
    char *buf;
    int   count;
    int   capacity;
} path_list_t;

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
    char *dst = list->buf + (size_t)list->count * PATH_BUF_SIZE;
    strncpy(dst, path, PATH_BUF_SIZE - 1);
    dst[PATH_BUF_SIZE - 1] = '\0';
    list->count++;
    return true;
}

static inline const char *path_list_get(path_list_t *list, int idx)
{
    return list->buf + (size_t)idx * PATH_BUF_SIZE;
}

static void path_list_free(path_list_t *list)
{
    free(list->buf);
    list->buf = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool is_music_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;

    for (int i = 0; i < NUM_EXTENSIONS; i++) {
        if (strcmp(dot, MUSIC_EXTENSIONS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void build_cache_path(const char *dir_path, char *out, size_t out_size)
{
    const char *rel = dir_path;
    size_t mount_len = strlen(MOUNT_POINT);

    if (strncmp(dir_path, MOUNT_POINT, mount_len) == 0) {
        rel = dir_path + mount_len;
        if (*rel == '/') rel++;
    }

    if (*rel == '\0') {
        snprintf(out, out_size, "%s/sdcard.txt", MUSIC_CACHE_DIR);
    } else {
        snprintf(out, out_size, "%s/sdcard_%s.txt", MUSIC_CACHE_DIR, rel);
        char *p = out + strlen(MUSIC_CACHE_DIR) + 1;
        while (*p) {
            if (*p == '/') *p = '_';
            p++;
        }
    }
}

static uint64_t get_used_space_kb(void)
{
    FATFS *fs;
    DWORD free_clst;
    FRESULT fr = f_getfree(MOUNT_POINT, &free_clst, &fs);
    if (fr != FR_OK) {
        ESP_LOGE(TAG_MUSIC_SCAN, "获取空间信息失败 (%d)", fr);
        return 0;
    }

    uint64_t total_kb = ((uint64_t)fs->n_fatent - 2) * fs->csize / 2;
    uint64_t free_kb  = (uint64_t)free_clst * fs->csize / 2;
    return total_kb - free_kb;
}

#define SPACE_MAGIC  "MUSICACHE1\n"

static uint64_t read_space_cache(void)
{
    int fd = open(SPACE_FILE, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    char buf[48] = {0};
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (len <= (ssize_t)strlen(SPACE_MAGIC)) return 0;
    if (strncmp(buf, SPACE_MAGIC, strlen(SPACE_MAGIC)) != 0) return 0;
    return strtoull(buf + strlen(SPACE_MAGIC), NULL, 10);
}

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

static void clean_cache_files(void)
{
    DIR *dir = opendir(MUSIC_CACHE_DIR);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        if (strncmp(entry->d_name, "sdcard", 6) == 0) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s",
                     MUSIC_CACHE_DIR, entry->d_name);
            remove(full_path);
        }
    }
    closedir(dir);
}

static void scan_dir(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG_MUSIC_SCAN, "无法打开目录: %s", dir_path);
        return;
    }

    char cache_path[512];
    build_cache_path(dir_path, cache_path, sizeof(cache_path));

    int fd = open(cache_path, O_RDWR | O_CREAT | O_TRUNC, 0);
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
            continue;
        }

        if (strcmp(entry->d_name, "System Volume Information") == 0) {
            continue;
        }

        if (entry->d_type == DT_REG) {
            if (is_music_file(entry->d_name)) {
                /* 跳过 0 字节的损坏/残留文件 */
                char full[512];
                snprintf(full, sizeof(full), "%s/%s", dir_path, entry->d_name);
                struct stat st;
                if (stat(full, &st) == 0 && st.st_size > 0) {
                    write(fd, entry->d_name, strlen(entry->d_name));
                    write(fd, "\n", 1);
                } else {
                    ESP_LOGW(TAG_MUSIC_SCAN, "跳过空文件: %s", full);
                }
            }
        } else if (entry->d_type == DT_DIR) {
            char sub_path[384];
            snprintf(sub_path, sizeof(sub_path), "%s/%s",
                     dir_path, entry->d_name);
            path_list_add(&subdirs, sub_path);
        }
    }

    close(fd);
    closedir(dir);

    for (int i = 0; i < subdirs.count; i++) {
        scan_dir(path_list_get(&subdirs, i));
    }

    path_list_free(&subdirs);
}

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

void music_scan_init(void)
{
    ESP_LOGI(TAG_MUSIC_SCAN, "初始化音乐文件扫描器");

    if (mkdir(MUSIC_CACHE_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG_MUSIC_SCAN, "无法创建缓存目录 %s: %s", MUSIC_CACHE_DIR, strerror(errno));
        return;
    }

    uint64_t current_used = get_used_space_kb();
    uint64_t cached_used = read_space_cache();

    ESP_LOGI(TAG_MUSIC_SCAN, "当前已用空间: %llu KB, 缓存记录: %llu KB",
             current_used, cached_used);

    if (cached_used == 0) {
        ESP_LOGI(TAG_MUSIC_SCAN, "无缓存记录，开始全量扫描");
        music_scan_run();
        return;
    }

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
