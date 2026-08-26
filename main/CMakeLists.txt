idf_component_register(SRCS "cover.cpp" "volume.c" "last_song.c" "ui_list.c" "chinese_16.c" "touch_cst816.c" "lcd_jd9853.c" "bt_a2dp.c"
                            "sys_monitor.c"
                            "music_scanner.c"
                            "last_song.c"
                            "volume.c"
                            "cover.cpp"
                            "main.c"
                            "sys_serial.c"
                            "lvgl_task.c"
                            "audio_task.cpp"
                            "decoder_mp3.cpp"
                            "pcm_pipeline.cpp"
                    PRIV_REQUIRES bt nvs_flash fatfs esp_lcd_jd9853 esp_lcd driver micro-mp3 esp-audio-libs esp_adc esp_psram jpegdec
                    INCLUDE_DIRS ".")
