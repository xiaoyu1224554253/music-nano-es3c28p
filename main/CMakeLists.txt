idf_component_register(SRCS "ui_list.c" "chinese_16.c" "touch_cst816.c" "lcd_jd9853.c" "bt_a2dp.c"
                            "sys_monitor.c"
                            "music_scanner.c"
                            "main.c"
                            "sys_serial.c"
                            "lvgl_task.c"
                            "audio_task.cpp"
                            "decoder_mp3.cpp"
                    PRIV_REQUIRES bt nvs_flash fatfs esp_lcd_jd9853 esp_lcd driver micro-mp3 esp_adc esp_psram
                    INCLUDE_DIRS ".")
