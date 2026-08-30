idf_component_register(SRCS "puck_power.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bt
                       PRIV_REQUIRES esp_pm esp_hw_support esp_timer driver)
