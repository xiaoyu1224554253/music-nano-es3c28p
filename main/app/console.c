#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_debug_helpers.h"
#include "esp_private/freertos_debug.h"
#include "xtensa_context.h"
#include "app.h"
#include "sys_monitor.h"

#define SERIAL_TAG    "SYS_SERIAL"
#define LINE_BUF_SIZE 64

static QueueHandle_t s_app_cmd_queue = NULL;

static TaskStatus_t *s_stats_prev       = NULL;
static UBaseType_t   s_stats_prev_count = 0;
static uint32_t      s_stats_prev_total = 0;
static TickType_t    s_stats_prev_tick  = 0;
static portMUX_TYPE  s_stats_lock       = portMUX_INITIALIZER_UNLOCKED;

static void stats_timer_cb(TimerHandle_t xTimer)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *snap = malloc(n * sizeof(TaskStatus_t));
    if (!snap) return;

    uint32_t total = 0;
    n = uxTaskGetSystemState(snap, n, &total);

    portENTER_CRITICAL(&s_stats_lock);
    if (s_stats_prev) free(s_stats_prev);
    s_stats_prev       = snap;
    s_stats_prev_count = n;
    s_stats_prev_total = total;
    s_stats_prev_tick  = xTaskGetTickCount();
    portEXIT_CRITICAL(&s_stats_lock);
}

static void cmd_stats(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    TaskStatus_t *prev = s_stats_prev;
    UBaseType_t   prev_count  = s_stats_prev_count;
    uint32_t      prev_total  = s_stats_prev_total;
    TickType_t    prev_tick   = s_stats_prev_tick;
    portEXIT_CRITICAL(&s_stats_lock);

    if (!prev) {
        printf("[stats] 暂无数据，请等待下一次采样\n");
        return;
    }

    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *cur = malloc(n * sizeof(TaskStatus_t));
    if (!cur) {
        printf("[stats] 内存不足\n");
        return;
    }

    uint32_t cur_total = 0;
    n = uxTaskGetSystemState(cur, n, &cur_total);

    TickType_t now = xTaskGetTickCount();
    uint32_t delta_ms = (now - prev_tick) * portTICK_PERIOD_MS;
    uint32_t delta_total = cur_total - prev_total;

    if (delta_total == 0) {
        printf("[stats] 测不到有效差值, 请稍后再试\n");
        free(cur);
        return;
    }

    printf("\n===== CPU 占用 (~%"PRIu32"ms 窗口) =====\n", delta_ms);
    printf("%-20s %6s  %4s  %s\n", "任务名", "CPU%", "Prio", "Core");

    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t delta_task = 0;
        for (UBaseType_t j = 0; j < prev_count; j++) {
            if (cur[i].xHandle == prev[j].xHandle) {
                if (cur[i].ulRunTimeCounter > prev[j].ulRunTimeCounter) {
                    delta_task = cur[i].ulRunTimeCounter -
                                 prev[j].ulRunTimeCounter;
                }
                break;
            }
        }
        float pct = (float)delta_task / (float)delta_total * 100.0f;
        printf("%-20s %5.1f%%  %4u  %d\n",
               cur[i].pcTaskName, pct, (unsigned)cur[i].uxCurrentPriority, cur[i].xCoreID);
    }
    printf("===============================\n");

    free(cur);
}

static void cmd_free(void)
{
    uint32_t total = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    printf("[RAM] 总空闲: %"PRIu32"KB  最大连续块: %"PRIu32"KB\n",
           total / 1024, largest / 1024);
}

static void cmd_psram(void)
{
    if (!esp_psram_is_initialized()) {
        printf("[PSRAM] 未启用\n");
        return;
    }
    uint32_t total = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    printf("[PSRAM] 总空闲: %"PRIu32"KB  最大连续块: %"PRIu32"KB\n",
           total / 1024, largest / 1024);
}

/* 打印指定 RTOS 任务的 backtrace (带任务名).
 * 阻塞/挂起任务: 用 TCB 保存的栈顶 pxTopOfStack (pc/a1/a0) 回溯其自身栈, 精确.
 * 正在运行的任务: 快照为最近一次被切出时的上下文 (略旧, 但栈内容仍在).
 * 本任务(自身): 用实时上下文. */
static void cmd_backtrace(const char *name)
{
    TaskHandle_t h = xTaskGetHandle(name);
    if (!h) {
        printf("[bt] 未找到任务: %s\n", name);
        return;
    }

    esp_backtrace_frame_t fr = {0};

    if (h == xTaskGetCurrentTaskHandle()) {
        esp_backtrace_get_start(&fr.pc, &fr.sp, &fr.next_pc);
        printf("[bt] %s (current task)\n", name);
        esp_backtrace_print_from_frame(50, &fr, false);
        return;
    }

    TaskSnapshot_t snap;
    vTaskSuspendAll();                       /* 冻结调度, 与 esp_backtrace_print_all_tasks 一致 */
    BaseType_t ok = vTaskGetSnapshot(h, &snap);
    xTaskResumeAll();

    if (ok != pdTRUE) {
        printf("[bt] 任务快照失败: %s\n", name);
        return;
    }

    XtExcFrame *f = (XtExcFrame *)snap.pxTopOfStack;
    fr.pc = f->pc;
    fr.sp = f->a1;
    fr.next_pc = f->a0;
    printf("[bt] %s (saved stack)\n", name);
    esp_backtrace_print_from_frame(50, &fr, false);
}

static void cmd_backtrace_all(void)
{
    printf("[bt] 所有任务 backtrace:\n");
    esp_backtrace_print_all_tasks(50);
}

static void console_task(void *arg)
{
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    printf("\n=== 音乐播放器 ===\n");
    printf("系统命令: stats | ram | psram | vbat | temp | bt [任务名]\n");
    printf("应用命令: scan | conn <名称> | disconn | play | stop | pause | info\n");
    printf("命令> ");

    char line[LINE_BUF_SIZE];
    int  line_pos = 0;

    while (1) {
        char ch;
        while (read(STDIN_FILENO, &ch, 1) > 0) {
            if (ch == '\n' || ch == '\r') {
                if (line_pos > 0) {
                    line[line_pos] = '\0';

                    if (strcmp(line, "stats") == 0) {
                        cmd_stats();
                    } else if (strcmp(line, "ram") == 0) {
                        cmd_free();
                    } else if (strcmp(line, "vbat") == 0) {
                        float v = atomic_load_float(&g_vbat);
                        printf("[电池] %.2f V\n", v);
                    } else if (strcmp(line, "temp") == 0) {
                        float t = atomic_load_float(&g_cpu_temp);
                        printf("[CPU温度] %.1f C\n", t);
                    } else if (strcmp(line, "psram") == 0) {
                        cmd_psram();
                    } else if (strcmp(line, "bt") == 0) {
                        cmd_backtrace_all();
                    } else if (strncmp(line, "bt ", 3) == 0) {
                        cmd_backtrace(line + 3);
                    } else {
                        app_cmd_t cmd;
                        memset(&cmd, 0, sizeof(cmd));

                        if (strcmp(line, "scan") == 0) {
                            cmd.type = APP_CMD_BT_SCAN;
                        } else if (strncmp(line, "conn ", 5) == 0) {
                            cmd.type = APP_CMD_BT_CONNECT;
                            size_t plen = strnlen(line + 5, sizeof(cmd.param) - 1);
                            memcpy(cmd.param, line + 5, plen);
                            cmd.param[plen] = '\0';
                        } else if (strcmp(line, "disconn") == 0) {
                            cmd.type = APP_CMD_BT_DISCONNECT;
                        } else if (strcmp(line, "play") == 0) {
                            cmd.type = APP_CMD_PLAY;
                        } else if (strcmp(line, "stop") == 0) {
                            cmd.type = APP_CMD_STOP;
                        } else if (strcmp(line, "pause") == 0) {
                            cmd.type = APP_CMD_PAUSE;
                        } else if (strcmp(line, "info") == 0) {
                            cmd.type = APP_CMD_INFO;
                        } else {
                            printf("未知命令: %s\n", line);
                            line_pos = 0;
                            printf("命令> ");
                            continue;
                        }

                        xQueueSend(s_app_cmd_queue, &cmd, 0);
                    }

                    line_pos = 0;
                    printf("命令> ");
                }
            } else if (ch == '\b' || ch == 127) {
                if (line_pos > 0) line_pos--;
            } else if (line_pos < LINE_BUF_SIZE - 1) {
                line[line_pos++] = ch;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void console_init(QueueHandle_t app_cmd_queue)
{
    s_app_cmd_queue = app_cmd_queue;

    TimerHandle_t timer = xTimerCreate("stats", pdMS_TO_TICKS(1000),
                                       pdTRUE, NULL, stats_timer_cb);
    xTimerStart(timer, 0);

    xTaskCreatePinnedToCore(console_task, "sys_serial", 2048, NULL, 1, NULL, 1);
}
