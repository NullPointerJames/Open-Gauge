#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/twai.h"

// ─── CAN Frame ───────────────────────────────────────────────────────────────

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} can_frame_t;

// ─── Callback ────────────────────────────────────────────────────────────────

/** Called from the CAN RX task for every received frame. */
typedef void (*can_rx_callback_t)(const can_frame_t *frame);

// ─── API ──────────────────────────────────────────────────────────────────────

/**
 * @brief Install the TWAI driver and start the RX task.
 *
 * @param tx_pin   GPIO for CAN TX (to transceiver TXD)
 * @param rx_pin   GPIO for CAN RX (from transceiver RXD)
 * @param speed_bps  Reserved; ignored. CAN is fixed to 500000 bps.
 */
esp_err_t can_manager_init(uint8_t tx_pin, uint8_t rx_pin, uint32_t speed_bps);

/**
 * @brief Compatibility API. Returns fixed 500000 bps.
 *
 * @param tx_pin         GPIO for CAN TX
 * @param rx_pin         GPIO for CAN RX
 * @param detected_speed Output for detected bitrate on success
 * @return ESP_OK and sets detected_speed=500000.
 */
esp_err_t can_manager_autodetect_speed(uint8_t tx_pin, uint8_t rx_pin, uint32_t *detected_speed);

/**
 * @brief Stop and uninstall the TWAI driver. Call before reinitialising
 *        with different settings.
 */
esp_err_t can_manager_deinit(void);

/**
 * @brief Register the frame receive callback (called from RX task context).
 */
void can_manager_set_rx_callback(can_rx_callback_t cb);

/**
 * @brief Transmit a CAN frame (used by OBD2 request poller).
 */
esp_err_t can_manager_transmit(const can_frame_t *frame);

/**
 * @brief Returns true if the TWAI driver is running.
 */
bool can_manager_is_running(void);
