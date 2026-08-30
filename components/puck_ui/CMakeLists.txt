idf_component_register(SRCS "puck_ui.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_timer)
