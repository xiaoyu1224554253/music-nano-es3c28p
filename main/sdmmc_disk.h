#pragma once

#include <stdbool.h>
#include "esp_err.h"

void sdmmc_disk_init(void);
void sdmmc_disk_deinit(void);
bool sdmmc_disk_is_mounted(void);

typedef void (*sd_event_cb_t)(const char *event, void *user_data);
void sdmmc_disk_set_event_callback(sd_event_cb_t cb, void *user_data);
