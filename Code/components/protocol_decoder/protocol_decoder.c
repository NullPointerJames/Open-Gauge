#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "protocol_decoder.h"
#include "config_manager.h"
#include "can_manager.h"
#include "link_ecu.h"
#include "haltech_ecu.h"
#include "obd2_decoder.h"

static const char *TAG = "decoder";

// Shared gauge data — protected by mutex
static gauge_data_t      s_data;
static SemaphoreHandle_t s_mutex;

// Active protocol — read from config, changeable at runtime
static can_protocol_t s_protocol;
static uint32_t       s_link_base_id;
static uint32_t       s_haltech_base_id;

// OBD2 poll task handle
static TaskHandle_t s_obd2_task  = NULL;
static TaskHandle_t s_debug_task = NULL;

// Timeout before marking data invalid (ms)
#define DATA_VALID_TIMEOUT_MS   3000
#define DECODE_DEBUG_LOG_EVERY_N 200
// How long /api/status keeps "live mode" alive after last poll (ms)
#define LIVE_MODE_TIMEOUT_MS    5000

// Decoder debug counters
static uint32_t s_rx_total_frames   = 0;
static uint32_t s_rx_matched_frames = 0;
static uint32_t s_rx_unmatched_log  = 0;
static uint32_t s_last_frame_id     = 0;

// ─── Haltech selective-decode state ──────────────────────────────────────────

// Bitmask of Haltech frames that should be decoded for the active gauge faces.
// Bit 0=0x360, 1=0x361, 2=0x362, 3=0x368, 4=0x369, 5=0x370,
//     6=0x372, 7=0x373, 8=0x374, 9=0x375, 10=0x3E0, 11=0x3E2
#define HALTECH_ALL_FRAMES_MASK  0x0FFFu
static uint32_t s_haltech_frame_mask = HALTECH_ALL_FRAMES_MASK;

// When non-zero, live mode is active and all frames are decoded.
static uint32_t s_live_until_ms = 0;

// Map a Haltech frame offset (id - base_id) to its bit position.
// Returns -1 for unknown offsets.
static int haltech_frame_bit(uint32_t offset)
{
    switch (offset) {
        case 0:     return 0;   // 0x360 RPM/MAP/TPS
        case 1:     return 1;   // 0x361 Fuel/Oil pressure
        case 2:     return 2;   // 0x362 Inj duty / ign angle
        case 8:     return 3;   // 0x368 Lambda 1 & 2
        case 9:     return 4;   // 0x369 Diagnostics
        case 16:    return 5;   // 0x370 Speed / gear / cam
        case 18:    return 6;   // 0x372 Battery / boost / baro
        case 19:    return 7;   // 0x373 EGT 1-4
        case 20:    return 8;   // 0x374 EGT 5-8
        case 21:    return 9;   // 0x375 EGT 9-12
        case 0x80:  return 10;  // 0x3E0 Temperatures
        case 0x82:  return 11;  // 0x3E2 Fuel economy
        default:    return -1;
    }
}

// Map a GAUGE_FIELD_* constant to the Haltech frame bitmask it requires.
static uint32_t field_to_haltech_mask(gauge_field_t f)
{
    switch (f) {
        case GAUGE_FIELD_RPM:
        case GAUGE_FIELD_MAP_KPA:
        case GAUGE_FIELD_TPS_PCT:
            return (1u << 0);   // 0x360

        case GAUGE_FIELD_FUEL_PRESS_BAR:
        case GAUGE_FIELD_OIL_PRESS_BAR:
            return (1u << 1);   // 0x361

        case GAUGE_FIELD_INJ_DUTY_PRI:
        case GAUGE_FIELD_INJ_DUTY_SEC:
        case GAUGE_FIELD_IGN_ANGLE:
            return (1u << 2);   // 0x362

        case GAUGE_FIELD_LAMBDA:
        case GAUGE_FIELD_LAMBDA2:
            return (1u << 3);   // 0x368

        // 0x369 diagnostics have no gauge field — included only in live mode

        case GAUGE_FIELD_SPEED_KPH:
        case GAUGE_FIELD_GEAR:
        case GAUGE_FIELD_INTAKE_CAM1:
        case GAUGE_FIELD_INTAKE_CAM2:
            return (1u << 5);   // 0x370

        case GAUGE_FIELD_BATTERY_V:
        case GAUGE_FIELD_TARGET_BOOST:
        case GAUGE_FIELD_BARO_KPA:
            return (1u << 6);   // 0x372

        case GAUGE_FIELD_EGT1:
        case GAUGE_FIELD_EGT2:
        case GAUGE_FIELD_EGT3:
        case GAUGE_FIELD_EGT4:
            return (1u << 7);   // 0x373

        case GAUGE_FIELD_EGT5:
        case GAUGE_FIELD_EGT6:
        case GAUGE_FIELD_EGT7:
        case GAUGE_FIELD_EGT8:
            return (1u << 8);   // 0x374

        case GAUGE_FIELD_EGT9:
        case GAUGE_FIELD_EGT10:
        case GAUGE_FIELD_EGT11:
        case GAUGE_FIELD_EGT12:
            return (1u << 9);   // 0x375

        case GAUGE_FIELD_COOLANT_C:
        case GAUGE_FIELD_IAT_C:
        case GAUGE_FIELD_FUEL_TEMP_C:
        case GAUGE_FIELD_OIL_TEMP_C:
            return (1u << 10);  // 0x3E0

        case GAUGE_FIELD_FUEL_CONSUMP:
        case GAUGE_FIELD_FUEL_ECONOMY:
            return (1u << 11);  // 0x3E2

        default:
            return 0;
    }
}

// ─── Internal helpers ─────────────────────────────────────────────────────────

static const char *protocol_name(can_protocol_t p)
{
    switch (p) {
        case PROTOCOL_LINK_ECU: return "LINK";
        case PROTOCOL_HALTECH:  return "HALTECH";
        case PROTOCOL_OBD2:     return "OBD2";
        default:                return "UNKNOWN";
    }
}

static bool frame_matches_active_protocol(const can_frame_t *frame)
{
    switch (s_protocol) {
        case PROTOCOL_LINK_ECU:
            return (frame->id >= s_link_base_id) &&
                   (frame->id <= (s_link_base_id + 3));

        case PROTOCOL_HALTECH: {
            if (frame->id < s_haltech_base_id) return false;
            uint32_t offset = frame->id - s_haltech_base_id;
            int bit = haltech_frame_bit(offset);
            if (bit < 0) return false;

            // Live mode: decode everything
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms < s_live_until_ms) return true;

            // Otherwise only decode frames the gauge faces require
            return ((s_haltech_frame_mask >> bit) & 1u) != 0;
        }

        case PROTOCOL_OBD2:
            return frame->id == OBD2_RESPONSE_ID &&
                   frame->dlc >= 4 &&
                   frame->data[1] == 0x41;

        default:
            return false;
    }
}

// ─── CAN RX Callback ─────────────────────────────────────────────────────────

static void on_can_frame(const can_frame_t *frame)
{
    if (!xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5))) return;

    s_rx_total_frames++;
    s_last_frame_id = frame->id;
    bool matched = frame_matches_active_protocol(frame);
    if (matched) {
        s_rx_matched_frames++;
        s_data.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000);
    } else {
        // Suppress warnings for Haltech frames within the ECU's own ID range —
        // the ECU broadcasts more frames than we decode; those are silently ignored.
        bool in_ecu_range = false;
        if (s_protocol == PROTOCOL_HALTECH &&
            frame->id >= s_haltech_base_id &&
            frame->id <  s_haltech_base_id + 0x200u) {
            in_ecu_range = true;
        } else if (s_protocol == PROTOCOL_LINK_ECU &&
                   frame->id >= s_link_base_id &&
                   frame->id <= s_link_base_id + 3u) {
            in_ecu_range = true;
        }

        if (!in_ecu_range &&
            (s_rx_unmatched_log < 10 ||
             (s_rx_total_frames % DECODE_DEBUG_LOG_EVERY_N) == 0)) {
            s_rx_unmatched_log++;
            ESP_LOGW(TAG,
                     "Unknown CAN frame id=0x%03lx dlc=%u proto=%s "
                     "(link_base=0x%03lx haltech_base=0x%03lx)",
                     (unsigned long)frame->id,
                     frame->dlc,
                     protocol_name(s_protocol),
                     (unsigned long)s_link_base_id,
                     (unsigned long)s_haltech_base_id);
        }
    }

    if (matched) {
        switch (s_protocol) {
            case PROTOCOL_LINK_ECU:
                link_ecu_decode(frame, s_link_base_id, &s_data);
                break;
            case PROTOCOL_HALTECH:
                haltech_decode(frame, s_haltech_base_id, &s_data);
                break;
            case PROTOCOL_OBD2:
                obd2_decode_response(frame, &s_data);
                break;
            default:
                break;
        }
    }

    xSemaphoreGive(s_mutex);
}

// ─── Background Tasks ────────────────────────────────────────────────────────

static void decode_debug_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        if (!xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20))) continue;

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t age_ms = now_ms - s_data.last_update_ms;
        bool live = (now_ms < s_live_until_ms);
        ESP_LOGI(TAG,
                 "CAN dbg: proto=%s total=%lu matched=%lu valid=%d "
                 "last_id=0x%03lx age_ms=%lu live=%d mask=0x%03lx",
                 protocol_name(s_protocol),
                 (unsigned long)s_rx_total_frames,
                 (unsigned long)s_rx_matched_frames,
                 s_data.valid ? 1 : 0,
                 (unsigned long)s_last_frame_id,
                 (unsigned long)age_ms,
                 live ? 1 : 0,
                 (unsigned long)s_haltech_frame_mask);

        xSemaphoreGive(s_mutex);
    }
}

static void staleness_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        if (!xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20))) continue;

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_data.valid &&
            (now_ms - s_data.last_update_ms) > DATA_VALID_TIMEOUT_MS) {
            ESP_LOGW(TAG, "CAN data timed out — marking invalid");
            s_data.valid = false;
        }

        xSemaphoreGive(s_mutex);
    }
}

// ─── Mask rebuild (internal, called with or without mutex) ────────────────────

static void rebuild_mask_locked(void)
{
    app_config_t *cfg = config_manager_get();
    uint32_t mask = 0;
    for (int i = 0; i < GAUGE_FACES_MAX; i++) {
        if (!cfg->faces[i].enabled) continue;
        for (int s = 0; s < FACE_MAX_SLOTS; s++) {
            mask |= field_to_haltech_mask(cfg->faces[i].slots[s]);
        }
    }
    // Always decode the core frame (RPM/MAP/TPS) if any face is active,
    // and always decode battery/temps when any face is enabled (cheap frames).
    for (int i = 0; i < GAUGE_FACES_MAX; i++) {
        if (cfg->faces[i].enabled) {
            mask |= (1u << 0);   // 0x360 always if gauges active
            break;
        }
    }
    s_haltech_frame_mask = mask ? mask : HALTECH_ALL_FRAMES_MASK;
    ESP_LOGI(TAG, "Haltech frame mask updated: 0x%03lx", (unsigned long)s_haltech_frame_mask);
}

// ─── Public API ───────────────────────────────────────────────────────────────

void protocol_decoder_init(void)
{
    memset(&s_data, 0, sizeof(s_data));
    s_data.gear = -1;

    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    app_config_t *cfg = config_manager_get();
    s_protocol        = cfg->protocol;
    s_link_base_id    = cfg->link_base_id;
    s_haltech_base_id = cfg->haltech_base_id;
    s_rx_total_frames = 0;
    s_rx_matched_frames = 0;
    s_rx_unmatched_log = 0;
    s_last_frame_id = 0;

    rebuild_mask_locked();

    can_manager_set_rx_callback(on_can_frame);

    if (s_protocol == PROTOCOL_OBD2) {
        xTaskCreatePinnedToCore(obd2_poll_task, "obd2_poll", 2048, NULL, 3,
                                &s_obd2_task, 0);
    }

    xTaskCreatePinnedToCore(staleness_task,  "can_stale", 2048, NULL, 2, NULL,       0);
    xTaskCreatePinnedToCore(decode_debug_task, "can_dbg", 3072, NULL, 2, &s_debug_task, 0);

    ESP_LOGI(TAG, "Protocol decoder ready (protocol=%s)", protocol_name(s_protocol));
}

void protocol_decoder_set_protocol(can_protocol_t protocol)
{
    if (!xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50))) return;
    s_protocol = protocol;
    app_config_t *cfg = config_manager_get();
    s_link_base_id    = cfg->link_base_id;
    s_haltech_base_id = cfg->haltech_base_id;

    rebuild_mask_locked();

    if (protocol == PROTOCOL_OBD2 && s_obd2_task == NULL) {
        xTaskCreatePinnedToCore(obd2_poll_task, "obd2_poll", 2048, NULL, 3,
                                &s_obd2_task, 0);
    } else if (protocol != PROTOCOL_OBD2 && s_obd2_task != NULL) {
        vTaskDelete(s_obd2_task);
        s_obd2_task = NULL;
    }

    memset(&s_data, 0, sizeof(s_data));
    s_data.gear = -1;
    s_rx_total_frames = 0;
    s_rx_matched_frames = 0;
    s_rx_unmatched_log = 0;
    s_last_frame_id = 0;

    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Protocol changed to %s", protocol_name(protocol));
}

void protocol_decoder_get_data(gauge_data_t *out)
{
    if (!xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10))) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_data;
    xSemaphoreGive(s_mutex);
}

void protocol_decoder_set_live_mode(void)
{
    if (!xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10))) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_live_until_ms = now_ms + LIVE_MODE_TIMEOUT_MS;
    xSemaphoreGive(s_mutex);
}

void protocol_decoder_rebuild_haltech_mask(void)
{
    if (!xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50))) return;
    rebuild_mask_locked();
    xSemaphoreGive(s_mutex);
}
