idf_component_register(SRCS "audio_sink.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2s esp_ringbuf esp_timer)
