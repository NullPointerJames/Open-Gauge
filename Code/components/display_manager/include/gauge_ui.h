#pragma once

#include "lvgl.h"
#include "protocol_decoder.h"

/**
 * @brief Build the gauge UI on the active LVGL screen (face 0 by default).
 *        Must be called inside lvgl_port_lock()/lvgl_port_unlock().
 */
void gauge_ui_create(lv_obj_t *parent);

/**
 * @brief Push new gauge data to the currently visible face.
 *        Must be called inside lvgl_port_lock()/lvgl_port_unlock().
 */
void gauge_ui_update(const gauge_data_t *data);

/**
 * @brief Update the status bar text (protocol name, WiFi indicator).
 */
void gauge_ui_set_status(const char *protocol_name, bool wifi_connected);

/**
 * @brief Animate to the next enabled face (wraps around).
 *        Safe to call from LVGL task context (inside lvgl_port_lock).
 */
void gauge_ui_next_face(void);

/**
 * @brief Signal that face configuration has changed.
 *        gauge_ui_update() will rebuild the active face list on the next call.
 *        Thread-safe (can be called from any task).
 */
void gauge_ui_mark_dirty(void);
