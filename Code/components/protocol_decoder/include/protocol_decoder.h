#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "config_manager.h"
#include "can_manager.h"

// ─── Decoded Gauge Data ───────────────────────────────────────────────────────

typedef struct {
    // ── Core engine ──────────────────────────────────────────────────────────
    float    rpm;              // Revs per minute           (0x360 b0-1)
    float    map_kpa;          // Manifold abs pressure kPa (0x360 b2-3)
    float    tps_pct;          // Throttle position 0–100%  (0x360 b4-5)

    // ── Pressures ─────────────────────────────────────────────────────────────
    float    fuel_pressure_bar;  // Fuel pressure bar       (0x361 b0-1)
    float    oil_pressure_bar;   // Oil pressure bar        (0x361 b2-3)

    // ── Injection / ignition ──────────────────────────────────────────────────
    float    inj_duty_primary_pct;    // Primary inj duty %   (0x362 b0-1)
    float    inj_duty_secondary_pct;  // Secondary inj duty % (0x362 b2-3)
    float    ignition_angle_leading;  // Ign angle leading °  (0x362 b4-5, s16)
    float    ignition_angle_trailing; // Ign angle trailing ° (0x362 b6-7, s16)

    // ── Wideband ──────────────────────────────────────────────────────────────
    float    lambda1;          // Lambda 1 (0x368 b0-1)
    float    lambda2;          // Lambda 2 (0x368 b2-3)

    // ── Diagnostic counters ───────────────────────────────────────────────────
    uint16_t miss_counter;       // (0x369 b0-1)
    uint16_t trigger_counter;    // (0x369 b2-3)
    uint16_t home_counter;       // (0x369 b4-5)
    uint16_t triggers_since_home;// (0x369 b6-7)

    // ── Vehicle dynamics ──────────────────────────────────────────────────────
    float    vehicle_speed_kph;  // km/h                    (0x370 b0-1)
    int8_t   gear;               // Gear (-1 = unknown)     (0x370 b2-3)
    float    intake_cam_angle_1; // Intake cam angle 1 °    (0x370 b4-5)
    float    intake_cam_angle_2; // Intake cam angle 2 °    (0x370 b6-7)

    // ── Electrical / boost ────────────────────────────────────────────────────
    float    battery_v;          // Battery voltage V       (0x372 b0-1)
    float    target_boost_kpa;   // Target boost kPa        (0x372 b4-5)
    float    baro_kpa;           // Barometric pressure kPa (0x372 b6-7)

    // ── Exhaust temps ─────────────────────────────────────────────────────────
    float    egt_c[12];          // EGT 1-12 in °C          (0x373-0x375, from K)

    // ── Coolant / air / oil temps ─────────────────────────────────────────────
    float    coolant_temp_c;     // °C  (0x3E0 b0-1, from K)
    float    iat_c;              // Intake air temp °C      (0x3E0 b2-3, from K)
    float    fuel_temp_c;        // Fuel temp °C            (0x3E0 b4-5, from K)
    float    oil_temp_c;         // Oil temp °C             (0x3E0 b6-7, from K)

    // ── Fuel economy ──────────────────────────────────────────────────────────
    float    fuel_consumption_lhr;  // L/hr  (0x3E2 b2-3)
    float    avg_fuel_economy_l;    // L     (0x3E2 b4-5)

    // ── Metadata ──────────────────────────────────────────────────────────────
    bool     valid;              // At least one valid frame received recently
    uint32_t last_update_ms;     // esp_timer timestamp of last update (ms)
} gauge_data_t;

// ─── API ──────────────────────────────────────────────────────────────────────

/**
 * @brief Initialise the decoder and register as CAN RX callback.
 *        Call after config_manager_init() and can_manager_init().
 */
void protocol_decoder_init(void);

/**
 * @brief Select which protocol to decode. Safe to call at runtime
 *        (protected by internal mutex).
 */
void protocol_decoder_set_protocol(can_protocol_t protocol);

/**
 * @brief Copy the latest decoded data into @p out.  Thread-safe.
 */
void protocol_decoder_get_data(gauge_data_t *out);

/**
 * @brief Signal that the live-data web page is actively polling.
 *        Extends the live-mode window by LIVE_MODE_TIMEOUT_MS so that all
 *        Haltech frames are decoded rather than just the gauge-face subset.
 *        Call from the HTTP /api/status handler on every request.
 */
void protocol_decoder_set_live_mode(void);

/**
 * @brief Recompute which Haltech frames need decoding from the current
 *        gauge-face configuration.  Call after face config changes.
 */
void protocol_decoder_rebuild_haltech_mask(void);
