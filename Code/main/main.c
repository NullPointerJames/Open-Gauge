#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"

#include "app_config.h"
#include "config_manager.h"
#include "can_manager.h"
#include "protocol_decoder.h"
#include "display_manager.h"
#include "gauge_ui.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

void app_main(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGW(TAG, "Reset reason: %s (%d)", reset_reason_str(rr), (int)rr);
    ESP_LOGI(TAG, "OpenGauge v%s starting on T-Circle-S3", APP_VERSION);

    /* NVS flash (required by WiFi and config_manager) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS wiped and re-initialised");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Persistent config */
    ESP_ERROR_CHECK(config_manager_init());

    /* Display: QSPI SH8601 AMOLED + CST816S touch + LVGL (all inside display_manager) */
    lv_display_t *disp = display_manager_init();
    if (!disp) {
        ESP_LOGE(TAG, "Display manager init failed — halting");
        return;
    }
    display_show_splash();

    /* CAN / TWAI */
    app_config_t *cfg = config_manager_get();
    ESP_LOGI(TAG, "CAN config: TX=%u RX=%u speed=%lu protocol=%d",
             cfg->can_tx_pin, cfg->can_rx_pin,
             (unsigned long)cfg->can_speed_bps, (int)cfg->protocol);
    ESP_ERROR_CHECK(can_manager_init(cfg->can_tx_pin, cfg->can_rx_pin, CAN_SPEED_500K));

    /* Protocol decoder — registers itself as CAN RX callback */
    protocol_decoder_init();

    /* WiFi SoftAP + HTTP config server */
    wifi_manager_init(cfg->wifi_ssid, cfg->wifi_password);

    ESP_LOGI(TAG, "All subsystems ready");
    display_show_gauge_screen();

    /* Main loop: copy decoded CAN data to LVGL at ~30 fps */
    gauge_data_t data = {0};
    while (1) {
        protocol_decoder_get_data(&data);

        if (lvgl_port_lock(10)) {
            gauge_ui_update(&data);
            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
