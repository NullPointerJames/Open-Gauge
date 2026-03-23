idf_component_register(
    SRCS
        "protocol_decoder.c"
        "link_ecu.c"
        "haltech_ecu.c"
        "obd2_decoder.c"
    INCLUDE_DIRS "include"
    REQUIRES config_manager can_manager log freertos esp_timer
)
