#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Start WiFi in SoftAP mode and launch the HTTP config server.
 *
 * @param ssid      Access-point SSID  (max 31 chars)
 * @param password  Access-point password (min 8 chars for WPA2, or "" for open)
 */
esp_err_t wifi_manager_init(const char *ssid, const char *password);

/**
 * @brief Stop the HTTP server and WiFi driver.
 */
esp_err_t wifi_manager_deinit(void);

/**
 * @brief Returns true if at least one station is connected to the AP.
 */
bool wifi_manager_has_client(void);
