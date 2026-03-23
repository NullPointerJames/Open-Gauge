#pragma once

#include "lvgl.h"
#include "protocol_decoder.h"

/**
 * @brief Initialise the QSPI bus, SH8601 panel, I2C touch, and LVGL port.
 *        All hardware init is self-contained here — main.c passes nothing.
 *
 * @return LVGL display pointer, or NULL on failure.
 */
lv_display_t *display_manager_init(void);

/** @brief Show splash screen. Returns after ~1.5 s. */
void display_show_splash(void);

/** @brief Transition to the main gauge screen. */
void display_show_gauge_screen(void);
