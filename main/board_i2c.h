#pragma once

#include "driver/i2c_master.h"

/* 板载 I2C 总线 (SDA16/SCL15): 触摸 FT6336 与音频 codec ES8311 共用.
 * 使用 IDF 新版 i2c_master 驱动, 首次调用时创建, 之后返回同一句柄. */
i2c_master_bus_handle_t board_i2c_bus(void);
