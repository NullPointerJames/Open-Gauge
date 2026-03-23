#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "config_manager.h"
#include "app_config.h"

static const char *TAG       = "config_mgr";
static const char *NVS_NS    = "opengauge";

#define CONFIG_VERSION 5

static app_config_t s_config;
static bool         s_initialised = false;

static bool sanitize_can_config(app_config_t *cfg)
{
    bool changed = false;

    if (cfg->can_speed_bps != CAN_SPEED_500K) {
        ESP_LOGW(TAG, "Forcing CAN speed to 500000 bps (was %lu)",
                 (unsigned long)cfg->can_speed_bps);
        cfg->can_speed_bps = CAN_SPEED_500K;
        changed = true;
    }

    if ((cfg->can_tx_pin == 1 && cfg->can_rx_pin == 2) ||
        cfg->can_tx_pin == cfg->can_rx_pin) {
        ESP_LOGW(TAG, "Replacing invalid/stale CAN pins TX=%u RX=%u with defaults TX=%u RX=%u",
                 cfg->can_tx_pin, cfg->can_rx_pin, DEFAULT_CAN_TX, DEFAULT_CAN_RX);
        cfg->can_tx_pin = DEFAULT_CAN_TX;
        cfg->can_rx_pin = DEFAULT_CAN_RX;
        changed = true;
    }

    return changed;
}

// ─── Factory Defaults ─────────────────────────────────────────────────────────

static void apply_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->protocol          = PROTOCOL_HALTECH;
    cfg->can_speed_bps     = CAN_SPEED_500K;
    cfg->can_tx_pin        = DEFAULT_CAN_TX;
    cfg->can_rx_pin        = DEFAULT_CAN_RX;
    cfg->link_base_id      = 0x518;
    cfg->haltech_base_id   = 0x360;

    strncpy(cfg->wifi_ssid,     "OpenGauge",     sizeof(cfg->wifi_ssid) - 1);
    strncpy(cfg->wifi_password, "opengauge123",  sizeof(cfg->wifi_password) - 1);

    cfg->show_rpm          = true;
    cfg->show_coolant      = true;
    cfg->show_oil_temp     = true;
    cfg->show_oil_pressure = true;
    cfg->show_tps          = true;
    cfg->show_map          = true;
    cfg->show_lambda       = true;
    cfg->show_battery      = true;
    cfg->show_speed        = true;

    cfg->rpm_redline        = 7000;
    cfg->coolant_warn_c     = 100.0f;
    cfg->oil_press_warn_bar = 2.0f;

    // Face 0: RPM arc + 2 configurable info cells (3 values total)
    cfg->faces[0] = (gauge_face_cfg_t){
        .layout  = FACE_LAYOUT_RPM_ARC,
        .enabled = 1,
        .slots   = { GAUGE_FIELD_COOLANT_C, GAUGE_FIELD_OIL_PRESS_BAR },
    };
    // Face 1: single large value
    cfg->faces[1] = (gauge_face_cfg_t){
        .layout  = FACE_LAYOUT_SINGLE,
        .enabled = 1,
        .slots   = { GAUGE_FIELD_RPM },
    };
    // Face 2: dual — RPM top, coolant bottom
    cfg->faces[2] = (gauge_face_cfg_t){
        .layout  = FACE_LAYOUT_DUAL,
        .enabled = 1,
        .slots   = { GAUGE_FIELD_RPM, GAUGE_FIELD_COOLANT_C },
    };
    // Face 3: quad 2x2 grid (disabled by default)
    cfg->faces[3] = (gauge_face_cfg_t){
        .layout  = FACE_LAYOUT_QUAD,
        .enabled = 0,
        .slots   = { GAUGE_FIELD_RPM, GAUGE_FIELD_COOLANT_C,
                     GAUGE_FIELD_OIL_PRESS_BAR, GAUGE_FIELD_BATTERY_V },
    };
    // Face 4: dial — boost / MAP gauge (enabled by default)
    cfg->faces[4] = (gauge_face_cfg_t){
        .layout             = FACE_LAYOUT_DIAL,
        .enabled            = 1,
        .slots              = { GAUGE_FIELD_MAP_KPA },
        .dial_min           = 0.0f,
        .dial_max           = 300.0f,   // kPa — 0 to ~2 bar boost
        .dial_warn_threshold = 200.0f,  // kPa — ~1 bar above atmospheric
        .dial_peak_hold_ms  = 3000,     // hold peak 3 s after dropping below threshold
    };

    // Display unit preferences — all off by default (metric / native units)
    cfg->display_temp_f = false;
    cfg->display_psi    = false;
    cfg->display_mph    = false;
    cfg->display_afr    = false;

    // Colour scheme defaults
    cfg->colors = (gauge_colors_t){
        .normal_rgb  = 0x00B4D8,
        .warn_rgb    = 0xFFD60A,
        .danger_rgb  = 0xFF3333,
        .text_rgb    = 0xFFFFFF,
        .arc_bg_rgb  = 0x1E1E1E,
    };

    cfg->config_version = CONFIG_VERSION;
}

// ─── NVS Helpers ─────────────────────────────────────────────────────────────

static esp_err_t nvs_load(app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t sz = sizeof(*cfg);
    err = nvs_get_blob(h, "config", cfg, &sz);
    nvs_close(h);

    if (err == ESP_OK && sz != sizeof(*cfg)) {
        // Struct size changed (firmware update) — use defaults
        return ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && cfg->config_version != CONFIG_VERSION) {
        // Config format changed — use defaults
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

static esp_err_t nvs_store(const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, "config", cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ─── Public API ───────────────────────────────────────────────────────────────

esp_err_t config_manager_init(void)
{
    apply_defaults(&s_config);

    esp_err_t err = nvs_load(&s_config);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGW(TAG, "No saved config found — using factory defaults");
        apply_defaults(&s_config);
        nvs_store(&s_config);   // Persist defaults
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS load error: %s — using factory defaults", esp_err_to_name(err));
        apply_defaults(&s_config);
    } else {
        ESP_LOGI(TAG, "Config loaded from NVS (protocol=%d, speed=%lu bps)",
                 s_config.protocol, (unsigned long)s_config.can_speed_bps);
    }

    if (sanitize_can_config(&s_config)) {
        ESP_LOGW(TAG, "Persisting corrected CAN config (TX=%u RX=%u speed=%lu)",
                 s_config.can_tx_pin, s_config.can_rx_pin, (unsigned long)s_config.can_speed_bps);
        nvs_store(&s_config);
    }

    s_initialised = true;
    return ESP_OK;
}

app_config_t *config_manager_get(void)
{
    return &s_config;
}

esp_err_t config_manager_save(void)
{
    esp_err_t err = nvs_store(&s_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved to NVS");
    } else {
        ESP_LOGE(TAG, "Config save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_manager_reset(void)
{
    apply_defaults(&s_config);
    return config_manager_save();
}
