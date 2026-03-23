#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "obd2_decoder.h"
#include "can_manager.h"

static const char *TAG = "obd2";

// PIDs to poll in round-robin order
static const uint8_t s_poll_pids[] = {
    OBD2_PID_RPM,
    OBD2_PID_COOLANT_TEMP,
    OBD2_PID_TPS,
    OBD2_PID_SPEED,
    OBD2_PID_IAT,
    OBD2_PID_OIL_TEMP,
    OBD2_PID_ENGINE_LOAD,
};
#define POLL_PID_COUNT (sizeof(s_poll_pids) / sizeof(s_poll_pids[0]))

// ─── Response Decoder ─────────────────────────────────────────────────────────

void obd2_decode_response(const can_frame_t *frame, gauge_data_t *data)
{
    // ISO 15765-4 single-frame response layout:
    //  byte 0: PCI (0x04 = 4 bytes follow for mode 1 response)
    //  byte 1: mode + 0x40 (0x41 = mode 1 response)
    //  byte 2: PID
    //  bytes 3+: data A, B, C, D

    if (frame->id != OBD2_RESPONSE_ID) return;
    if (frame->dlc < 4) return;
    if (frame->data[1] != 0x41) return; // Not mode 01 response

    uint8_t pid = frame->data[2];
    uint8_t A   = frame->data[3];
    uint8_t B   = (frame->dlc > 4) ? frame->data[4] : 0;
    uint8_t C   = (frame->dlc > 5) ? frame->data[5] : 0;
    uint8_t D   = (frame->dlc > 6) ? frame->data[6] : 0;

    switch (pid) {
        case OBD2_PID_RPM:
            // Formula: ((A*256)+B)/4
            data->rpm   = ((A * 256U) + B) / 4.0f;
            data->valid = true;
            break;

        case OBD2_PID_COOLANT_TEMP:
            // Formula: A - 40
            data->coolant_temp_c = (float)A - 40.0f;
            break;

        case OBD2_PID_TPS:
            // Formula: A * 100 / 255
            data->tps_pct = A * 100.0f / 255.0f;
            break;

        case OBD2_PID_SPEED:
            // Formula: A (km/h)
            data->vehicle_speed_kph = (float)A;
            break;

        case OBD2_PID_IAT:
            // Formula: A - 40
            data->iat_c = (float)A - 40.0f;
            break;

        case OBD2_PID_OIL_TEMP:
            // Formula: A - 40
            data->oil_temp_c = (float)A - 40.0f;
            break;

        case OBD2_PID_ENGINE_LOAD:
            // Formula: A * 100 / 255 → use as MAP proxy
            data->map_kpa = A * 100.0f / 255.0f; // Not kPa but % load — close enough
            break;

        default:
            break;
    }
    (void)C; (void)D;
}

// ─── Poll Task ────────────────────────────────────────────────────────────────

void obd2_poll_task(void *arg)
{
    ESP_LOGI(TAG, "OBD2 poll task started");
    uint32_t idx = 0;

    while (1) {
        // Build ISO 15765-4 single-frame request
        can_frame_t req = {
            .id  = OBD2_REQUEST_ID,
            .dlc = 8,
            .data = {
                0x02,              // PCI: 2 data bytes follow
                0x01,              // Mode: current data
                s_poll_pids[idx],  // PID
                0x55, 0x55, 0x55, 0x55, 0x55  // padding
            }
        };

        esp_err_t err = can_manager_transmit(&req);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TX failed: %s", esp_err_to_name(err));
        }

        idx = (idx + 1) % POLL_PID_COUNT;
        vTaskDelay(pdMS_TO_TICKS(100)); // Poll each PID at ~10 Hz / number of PIDs
    }
}
