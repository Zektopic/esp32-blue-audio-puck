idf_component_register(SRCS "puck_avrcp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bt
                       PRIV_REQUIRES bt_core audio_sink)
