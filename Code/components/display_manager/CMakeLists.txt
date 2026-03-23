idf_component_register(
    SRCS
        "display_manager.c"
        "gauge_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_lcd
        driver
        log
        freertos
        protocol_decoder
        config_manager
        espressif__esp_lvgl_port
        espressif__esp_lcd_sh8601
)
