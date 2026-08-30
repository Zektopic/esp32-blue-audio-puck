idf_component_register(SRCS "bt_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bt
                       PRIV_REQUIRES nvs_flash esp_timer)
