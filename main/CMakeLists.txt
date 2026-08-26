idf_component_register(SRCS "touch_cst816.c" "lcd_jd9853.c" "bt_a2dp.c"
                            "main.c"
                            "sys_serial.c"
                            "lvgl_task.c"
                            "audio_task.c"
                            "sdmmc_disk.c"
                    PRIV_REQUIRES bt nvs_flash fatfs esp_lcd_jd9853 esp_lcd driver
                    INCLUDE_DIRS ".")
