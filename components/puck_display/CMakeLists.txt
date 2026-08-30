idf_component_register(SRCS "puck_display.c" "ssd1306.c" "font5x7.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2c bt_core puck_avrcp puck_battery)
