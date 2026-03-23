#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ─── Gauge Face Configuration ─────────────────────────────────────────────────

// Data fields that can be assigned to any face slot
// ── Core (all protocols) ──────────────────────────────────────────────────────
#define GAUGE_FIELD_NONE          0
#define GAUGE_FIELD_RPM           1
#define GAUGE_FIELD_COOLANT_C     2
#define GAUGE_FIELD_OIL_TEMP_C    3
#define GAUGE_FIELD_OIL_PRESS_BAR 4
#define GAUGE_FIELD_TPS_PCT       5
#define GAUGE_FIELD_MAP_KPA       6
#define GAUGE_FIELD_LAMBDA        7   // Lambda 1
#define GAUGE_FIELD_BATTERY_V     8
#define GAUGE_FIELD_SPEED_KPH     9
// ── Haltech extended ─────────────────────────────────────────────────────────
#define GAUGE_FIELD_FUEL_PRESS_BAR  10
#define GAUGE_FIELD_IAT_C           11  // Intake air temperature
#define GAUGE_FIELD_LAMBDA2         12
#define GAUGE_FIELD_INJ_DUTY_PRI    13  // Primary injector duty %
#define GAUGE_FIELD_INJ_DUTY_SEC    14  // Secondary injector duty %
#define GAUGE_FIELD_IGN_ANGLE       15  // Ignition angle leading °
#define GAUGE_FIELD_GEAR            16
#define GAUGE_FIELD_INTAKE_CAM1     17  // Intake cam angle 1 °
#define GAUGE_FIELD_INTAKE_CAM2     18  // Intake cam angle 2 °
#define GAUGE_FIELD_TARGET_BOOST    19  // Target boost kPa
#define GAUGE_FIELD_BARO_KPA        20  // Barometric pressure kPa
#define GAUGE_FIELD_EGT1            21
#define GAUGE_FIELD_EGT2            22
#define GAUGE_FIELD_EGT3            23
#define GAUGE_FIELD_EGT4            24
#define GAUGE_FIELD_EGT5            25
#define GAUGE_FIELD_EGT6            26
#define GAUGE_FIELD_EGT7            27
#define GAUGE_FIELD_EGT8            28
#define GAUGE_FIELD_EGT9            29
#define GAUGE_FIELD_EGT10           30
#define GAUGE_FIELD_EGT11           31
#define GAUGE_FIELD_EGT12           32
#define GAUGE_FIELD_FUEL_TEMP_C     33
#define GAUGE_FIELD_FUEL_CONSUMP    34  // Fuel consumption L/hr
#define GAUGE_FIELD_FUEL_ECONOMY    35  // Average fuel economy L
#define GAUGE_FIELD_COUNT           36

typedef uint8_t gauge_field_t;

// Screen layout types
// Slots used per layout:
//   RPM_ARC  → slots[0..1] = 2 info cells   (arc always shows RPM, 3 values total)
//   SINGLE   → slots[0]    = one large value
//   DUAL     → slots[0..1] = top / bottom halves
//   QUAD     → slots[0..3] = 2×2 grid
//   DIAL     → slots[0]    = primary field, needle + arc scale + peak hold
#define FACE_LAYOUT_RPM_ARC  0
#define FACE_LAYOUT_SINGLE   1
#define FACE_LAYOUT_DUAL     2
#define FACE_LAYOUT_QUAD     3
#define FACE_LAYOUT_DIAL     4
#define FACE_LAYOUT_COUNT    5

typedef uint8_t face_layout_t;

#define FACE_MAX_SLOTS  6
#define GAUGE_FACES_MAX 5

typedef struct {
    face_layout_t layout;
    gauge_field_t slots[FACE_MAX_SLOTS];
    uint8_t       enabled;
    // Dial-face parameters (used only when layout == FACE_LAYOUT_DIAL)
    float    dial_min;            // scale minimum value
    float    dial_max;            // scale maximum value
    float    dial_warn_threshold; // above this level → record peak
    uint16_t dial_peak_hold_ms;   // 0 = hold until re-crossed, else hold for N ms after drop
} gauge_face_cfg_t;

// ─── Gauge Colour Scheme ──────────────────────────────────────────────────────

typedef struct {
    uint32_t normal_rgb;  // arc / needle normal colour  (default 0x00B4D8)
    uint32_t warn_rgb;    // warning colour               (default 0xFFD60A)
    uint32_t danger_rgb;  // danger / over-threshold      (default 0xFF3333)
    uint32_t text_rgb;    // primary text colour          (default 0xFFFFFF)
    uint32_t arc_bg_rgb;  // arc track background colour  (default 0x1E1E1E)
} gauge_colors_t;

// ─── Warning Configuration ────────────────────────────────────────────────────

#define WARNINGS_MAX 8

typedef struct {
    gauge_field_t field;           // channel to monitor (GAUGE_FIELD_*)
    bool          enabled;         // whether this slot is active
    float         lower_threshold; // warn if value < this (0.0f = disabled)
    float         upper_threshold; // warn if value > this (0.0f = disabled)
    bool          high_priority;   // true = full-screen overlay when triggered
    char          label[24];       // user-defined name shown on overlay
} warning_cfg_t;

// ─── Protocol Selection ───────────────────────────────────────────────────────

typedef enum {
    PROTOCOL_LINK_ECU = 0,
    PROTOCOL_HALTECH  = 1,
    PROTOCOL_OBD2     = 2,
} can_protocol_t;

// ─── CAN Bus Speed ────────────────────────────────────────────────────────────

typedef enum {
    CAN_SPEED_250K  = 250000,
    CAN_SPEED_500K  = 500000,
    CAN_SPEED_1M    = 1000000,
} can_speed_t;

// ─── Application Configuration ───────────────────────────────────────────────

typedef struct {
    // CAN bus
    can_protocol_t protocol;
    uint32_t       can_speed_bps;  // 250000 / 500000 / 1000000
    uint8_t        can_tx_pin;
    uint8_t        can_rx_pin;

    // Link ECU custom CAN base ID (default 0x518)
    uint32_t       link_base_id;
    // Haltech CAN base ID (default 0x360)
    uint32_t       haltech_base_id;

    // WiFi AP
    char wifi_ssid[32];
    char wifi_password[32];

    // Display preferences
    bool show_rpm;
    bool show_coolant;
    bool show_oil_temp;
    bool show_oil_pressure;
    bool show_tps;
    bool show_map;
    bool show_lambda;
    bool show_battery;
    bool show_speed;

    // Gauge limits
    uint16_t rpm_redline;        // RPM — default 7000
    float    coolant_warn_c;     // °C  — default 100
    float    oil_press_warn_bar; // bar — default 2.0

    // Gauge faces (up to 5 configurable screen faces)
    gauge_face_cfg_t faces[GAUGE_FACES_MAX];

    // Global colour scheme
    gauge_colors_t colors;

    // Display unit preferences (applied to text labels; dial arc/scale is always native)
    bool display_temp_f;    // show temperatures in °F instead of °C
    bool display_psi;       // show pressures in PSI instead of native kPa/bar
    bool display_mph;       // show speed in mph instead of km/h
    bool display_afr;       // show lambda as AFR (×14.7) instead of λ

    // Configurable warning slots (up to 8)
    warning_cfg_t warnings[WARNINGS_MAX];

    // Bump CONFIG_VERSION in config_manager.c whenever the struct layout changes
    // so that stale NVS blobs are discarded and fresh defaults are applied.
    uint8_t config_version;
} app_config_t;

// ─── API ──────────────────────────────────────────────────────────────────────

esp_err_t    config_manager_init(void);
app_config_t *config_manager_get(void);
esp_err_t    config_manager_save(void);
esp_err_t    config_manager_reset(void);
