#pragma once

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

void cover_init(QueueHandle_t app_cmd_queue);
void cover_submit_job(const uint8_t *jpg, size_t size);

#ifdef __cplusplus
}
#endif
