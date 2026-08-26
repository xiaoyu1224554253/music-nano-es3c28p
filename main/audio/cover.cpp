#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "JPEGDEC.h"
#include "cover.h"
#include "app.h"

/* 该组件只编译了 C API (JPEG_openRAM 等), 在此声明供 C++ 使用 */
extern "C" {
int JPEG_openRAM(JPEGIMAGE *pJPEG, uint8_t *pData, int iDataSize, JPEG_DRAW_CALLBACK *pfnDraw);
int JPEG_decode(JPEGIMAGE *pJPEG, int x, int y, int iOptions);
void JPEG_close(JPEGIMAGE *pJPEG);
void JPEG_setPixelType(JPEGIMAGE *pJPEG, int iType);
int JPEG_getWidth(JPEGIMAGE *pJPEG);
int JPEG_getHeight(JPEGIMAGE *pJPEG);
}

#define COVER_TAG         "COVER"
#define COVER_SIZE        100
#define COVER_PIXELS      (COVER_SIZE * COVER_SIZE)
#define COVER_STACK_WORDS 4096

/* LVGL 开了 LV_COLOR_16_SWAP, 最终输出缓冲需字节交换 */

static QueueHandle_t     s_app_cmd_queue = NULL;
static TaskHandle_t      s_task = NULL;
static StaticTask_t     *s_tcb = NULL;
static StackType_t      *s_stack = NULL;
static SemaphoreHandle_t s_job_sem = NULL;
static SemaphoreHandle_t s_slot_mutex = NULL;

/* 待解码的封面源数据 (PSRAM, 所有权归封面模块, 解码后释放) */
static uint8_t *s_job_data = NULL;
static size_t   s_job_size = 0;

static uint16_t *s_out_buf = NULL;  /* 常驻 100x100 RGB565 输出 (PSRAM) */
static JPEGIMAGE *s_jpeg = NULL;    /* 常驻解码器 (PSRAM) */

static uint16_t *s_raw = NULL;      /* 解码中间缓冲 (PSRAM, 临时) */
static int       s_raw_w = 0;
static int       s_raw_h = 0;

/* JPEG 解码回调: 把 MCU 块拷进中间缓冲 */
static int cover_jpeg_draw(JPEGDRAW *pDraw)
{
    if (!s_raw) return 0;

    uint16_t *dest = s_raw + (size_t)pDraw->y * s_raw_w + pDraw->x;
    const uint16_t *src = pDraw->pPixels;
    int h = pDraw->iHeight;
    while (h-- > 0) {
        memcpy(dest, src, (size_t)pDraw->iWidth * sizeof(uint16_t));
        dest += s_raw_w;
        src  += pDraw->iWidth;
    }
    return 1;
}

/* 面积平均采样 (缩小时) */
static uint16_t cover_avg_box(const uint16_t *src, int src_w, int x0, int x1,
                              int y0, int y1)
{
    uint32_t r = 0, g = 0, b = 0, cnt = 0;
    for (int y = y0; y <= y1; y++) {
        const uint16_t *row = src + (size_t)y * src_w;
        for (int x = x0; x <= x1; x++) {
            uint16_t p = row[x];
            r += (p >> 11) & 0x1F;
            g += (p >> 5)  & 0x3F;
            b += p & 0x1F;
            cnt++;
        }
    }
    r = (r + cnt / 2) / cnt;
    g = (g + cnt / 2) / cnt;
    b = (b + cnt / 2) / cnt;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* 居中裁方 + 缩放到 100x100 (标准 RGB565) */
static void cover_scale_crop_to_100(const uint16_t *src, int src_w, int src_h)
{
    int side = src_w < src_h ? src_w : src_h;
    int cx = (src_w - side) / 2;
    int cy = (src_h - side) / 2;

    for (int ty = 0; ty < COVER_SIZE; ty++) {
        uint16_t *dst = s_out_buf + (size_t)ty * COVER_SIZE;
        if (side >= COVER_SIZE) {
            int y0 = cy + (ty * side) / COVER_SIZE;
            int y1 = cy + ((ty + 1) * side) / COVER_SIZE - 1;
            if (y1 < y0) y1 = y0;
            for (int tx = 0; tx < COVER_SIZE; tx++) {
                int x0 = cx + (tx * side) / COVER_SIZE;
                int x1 = cx + ((tx + 1) * side) / COVER_SIZE - 1;
                if (x1 < x0) x1 = x0;
                dst[tx] = cover_avg_box(src, src_w, x0, x1, y0, y1);
            }
        } else {
            int sy = cy + (ty * side) / COVER_SIZE;
            for (int tx = 0; tx < COVER_SIZE; tx++) {
                int sx = cx + (tx * side) / COVER_SIZE;
                dst[tx] = src[(size_t)sy * src_w + sx];
            }
        }
    }
}

/* LV_COLOR_16_SWAP: 标准 RGB565 字节交换 */
static void cover_swap_rgb565(uint16_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        buf[i] = (uint16_t)((buf[i] << 8) | (buf[i] >> 8));
    }
}

static void cover_decode_job(const uint8_t *data, size_t size)
{
    if (!s_out_buf || !s_jpeg || !data || size == 0) return;

    if (JPEG_openRAM(s_jpeg, (uint8_t *)data, (int)size, cover_jpeg_draw) != 1) {
        ESP_LOGW(COVER_TAG, "openRAM 失败");
        return;
    }

    /* 渐进式 JPEG (SOF2, 0xc2) 不支持, 跳过 */
    if (s_jpeg->ucMode == 0xc2) {
        ESP_LOGW(COVER_TAG, "渐进式 JPEG, 跳过封面");
        JPEG_close(s_jpeg);
        return;
    }

    int w = JPEG_getWidth(s_jpeg);
    int h = JPEG_getHeight(s_jpeg);
    if (w <= 0 || h <= 0) {
        JPEG_close(s_jpeg);
        return;
    }

    int opt = 0, rw = w, rh = h;
    if (w / 8 >= COVER_SIZE && h / 8 >= COVER_SIZE) {
        opt = JPEG_SCALE_EIGHTH; rw = w / 8; rh = h / 8;
    } else if (w / 4 >= COVER_SIZE && h / 4 >= COVER_SIZE) {
        opt = JPEG_SCALE_QUARTER; rw = w / 4; rh = h / 4;
    } else if (w / 2 >= COVER_SIZE && h / 2 >= COVER_SIZE) {
        opt = JPEG_SCALE_HALF; rw = w / 2; rh = h / 2;
    }

    /* +16 边缘缓冲, 避免 MCU 块越界 */
    size_t raw_bytes = (size_t)(rw + 16) * (rh + 16) * sizeof(uint16_t);
    uint16_t *raw = (uint16_t *)heap_caps_malloc(raw_bytes, MALLOC_CAP_SPIRAM);
    if (!raw) {
        ESP_LOGW(COVER_TAG, "中间缓冲分配失败");
        JPEG_close(s_jpeg);
        return;
    }
    s_raw = raw;
    s_raw_w = rw;
    s_raw_h = rh;

    JPEG_setPixelType(s_jpeg, RGB565_LITTLE_ENDIAN);
    int rc = JPEG_decode(s_jpeg, 0, 0, opt);
    JPEG_close(s_jpeg);
    s_raw = NULL;

    if (rc != 1) {
        ESP_LOGW(COVER_TAG, "解码失败");
        heap_caps_free(raw);
        return;
    }

    cover_scale_crop_to_100(raw, rw, rh);
    heap_caps_free(raw);

    cover_swap_rgb565(s_out_buf, COVER_PIXELS);

    if (s_app_cmd_queue) {
        app_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = APP_CMD_COVER_READY;
        void *buf = s_out_buf;
        memcpy(cmd.param, &buf, sizeof(buf));
        xQueueSend(s_app_cmd_queue, &cmd, 0);
    }
}

static void cover_task(void *arg)
{
    for (;;) {
        if (!xSemaphoreTake(s_job_sem, portMAX_DELAY)) continue;

        uint8_t *data;
        size_t size;
        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        data = s_job_data;
        size = s_job_size;
        s_job_data = NULL;
        s_job_size = 0;
        xSemaphoreGive(s_slot_mutex);

        if (data && size > 0) {
            cover_decode_job(data, size);
            heap_caps_free(data);
        }
    }
}

void cover_init(QueueHandle_t app_cmd_queue)
{
    s_app_cmd_queue = app_cmd_queue;
    s_job_sem = xSemaphoreCreateCounting(4, 0);
    s_slot_mutex = xSemaphoreCreateMutex();

    s_out_buf = (uint16_t *)heap_caps_malloc(COVER_PIXELS * sizeof(uint16_t),
                                             MALLOC_CAP_SPIRAM);
    s_jpeg = (JPEGIMAGE *)heap_caps_malloc(sizeof(JPEGIMAGE), MALLOC_CAP_SPIRAM);
    s_stack = (StackType_t *)heap_caps_malloc(COVER_STACK_WORDS * sizeof(StackType_t),
                                              MALLOC_CAP_SPIRAM);
    s_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!s_out_buf || !s_jpeg || !s_stack || !s_tcb) {
        ESP_LOGE(COVER_TAG, "PSRAM 分配失败, 封面功能禁用");
        return;
    }

    s_task = xTaskCreateStaticPinnedToCore(cover_task, "cover", COVER_STACK_WORDS,
                                           NULL, 2, s_stack, s_tcb, 1);
    ESP_LOGI(COVER_TAG, "封面解码任务已创建 (PSRAM 栈)");
}

void cover_submit_job(const uint8_t *jpg, size_t size)
{
    if (!jpg || size == 0 || !s_task || !s_job_sem || !s_slot_mutex) return;

    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    if (s_job_data) {
        heap_caps_free(s_job_data);  /* 丢弃未取走的旧 job */
    }
    s_job_data = (uint8_t *)jpg;
    s_job_size = size;
    xSemaphoreGive(s_slot_mutex);

    xSemaphoreGive(s_job_sem);
}
