idf_component_register(SRCS "puck_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt esp_system spi_flash audio_sink audio_dsp bt_core puck_avrcp puck_ui puck_power)
