#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "http_server.h"

static const char *TAG = "wifi_mgr";

static int      s_client_count = 0;
static bool     s_initialised  = false;

// ─── WiFi Event Handler ───────────────────────────────────────────────────────

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *e = event_data;
            s_client_count++;
            ESP_LOGI(TAG, "Client connected: " MACSTR " (total: %d)",
                     MAC2STR(e->mac), s_client_count);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t *e = event_data;
            if (s_client_count > 0) s_client_count--;
            ESP_LOGI(TAG, "Client disconnected: " MACSTR " (total: %d)",
                     MAC2STR(e->mac), s_client_count);
        }
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

esp_err_t wifi_manager_init(const char *ssid, const char *password)
{
    if (s_initialised) return ESP_OK;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    (void)ap_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid,     ssid,     sizeof(wifi_config.ap.ssid) - 1);
    strncpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.ssid_len      = (uint8_t)strlen(ssid);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.channel        = 6;

    if (strlen(password) >= 8) {
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGW(TAG, "Password too short — starting open AP");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started — SSID: \"%s\"  IP: 192.168.4.1", ssid,password);

    // Start the HTTP config server
    ESP_ERROR_CHECK(http_server_start());

    s_initialised = true;
    return ESP_OK;
}

esp_err_t wifi_manager_deinit(void)
{
    if (!s_initialised) return ESP_OK;
    http_server_stop();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_initialised  = false;
    s_client_count = 0;
    return ESP_OK;
}

bool wifi_manager_has_client(void)
{
    return s_client_count > 0;
}
