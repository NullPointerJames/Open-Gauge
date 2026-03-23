#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/twai.h"
#include "can_manager.h"

static const char *TAG = "can_mgr";

static can_rx_callback_t s_rx_cb      = NULL;
static bool              s_running    = false;
static TaskHandle_t      s_rx_task    = NULL;
static TaskHandle_t      s_alert_task = NULL;
static uint32_t          s_rx_count   = 0;
static const uint32_t    CAN_FIXED_SPEED_BPS = 500000;

#define CAN_DEBUG_STATUS_MS        2000

static const char *twai_state_name(twai_state_t state)
{
    switch (state) {
        case TWAI_STATE_STOPPED:    return "STOPPED";
        case TWAI_STATE_RUNNING:    return "RUNNING";
        case TWAI_STATE_BUS_OFF:    return "BUS_OFF";
        case TWAI_STATE_RECOVERING: return "RECOVERING";
        default:                    return "UNKNOWN";
    }
}

static void log_twai_status(const char *prefix)
{
    twai_status_info_t st = {0};
    if (twai_get_status_info(&st) != ESP_OK) {
        ESP_LOGW(TAG, "%s status unavailable", prefix);
        return;
    }

    ESP_LOGW(TAG,
             "%s state=%s tec=%lu rec=%lu rxq=%lu txq=%lu bus_err=%lu rx_missed=%lu rx_overrun=%lu arb_lost=%lu tx_failed=%lu",
             prefix,
             twai_state_name(st.state),
             (unsigned long)st.tx_error_counter,
             (unsigned long)st.rx_error_counter,
             (unsigned long)st.msgs_to_rx,
             (unsigned long)st.msgs_to_tx,
             (unsigned long)st.bus_error_count,
             (unsigned long)st.rx_missed_count,
             (unsigned long)st.rx_overrun_count,
             (unsigned long)st.arb_lost_count,
             (unsigned long)st.tx_failed_count);
}

// RX Task
static void can_rx_task(void *arg)
{
    twai_message_t msg;
    can_frame_t frame;
    TickType_t last_status_tick = xTaskGetTickCount();
    uint32_t last_rx_count = 0;

    while (s_running) {
        esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(100));
        if (err == ESP_OK) {
            s_rx_count++;
            if (s_rx_cb) {
                frame.id = msg.identifier;
                frame.dlc = msg.data_length_code;
                memcpy(frame.data, msg.data, frame.dlc);
                s_rx_cb(&frame);
            }
        } else if (err == ESP_ERR_TIMEOUT) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_status_tick) >= pdMS_TO_TICKS(CAN_DEBUG_STATUS_MS)) {
                if (s_rx_count == last_rx_count) {
                    log_twai_status("No RX traffic");
                }
                last_rx_count = s_rx_count;
                last_status_tick = now;
            }
        } else {
            ESP_LOGW(TAG, "TWAI receive error: %s", esp_err_to_name(err));
        }
    }
    vTaskDelete(NULL);
}

static void can_alert_task(void *arg)
{
    while (s_running) {
        uint32_t alerts = 0;
        esp_err_t err = twai_read_alerts(&alerts, pdMS_TO_TICKS(250));
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TWAI alert read error: %s", esp_err_to_name(err));
            continue;
        }
        if (alerts == 0) {
            continue;
        }

        ESP_LOGW(TAG, "TWAI alerts: 0x%08lx", (unsigned long)alerts);
        log_twai_status("Alert status");

        if (alerts & TWAI_ALERT_BUS_OFF) {
            ESP_LOGE(TAG, "TWAI BUS_OFF detected, initiating recovery");
            esp_err_t rec_err = twai_initiate_recovery();
            if (rec_err != ESP_OK) {
                ESP_LOGE(TAG, "twai_initiate_recovery failed: %s", esp_err_to_name(rec_err));
            }
        }
    }
    vTaskDelete(NULL);
}

esp_err_t can_manager_init(uint8_t tx_pin, uint8_t rx_pin, uint32_t speed_bps)
{
    if (s_running) {
        ESP_LOGW(TAG, "Already running - call can_manager_deinit() first");
        return ESP_ERR_INVALID_STATE;
    }

    (void)speed_bps;
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)tx_pin, (gpio_num_t)rx_pin, TWAI_MODE_NORMAL);
    g_config.rx_queue_len = 64;
    g_config.tx_queue_len = 8;
    g_config.alerts_enabled = TWAI_ALERT_RX_QUEUE_FULL |
                              TWAI_ALERT_RX_FIFO_OVERRUN |
                              TWAI_ALERT_BUS_ERROR |
                              TWAI_ALERT_ABOVE_ERR_WARN |
                              TWAI_ALERT_ERR_PASS |
                              TWAI_ALERT_ERR_ACTIVE |
                              TWAI_ALERT_BUS_OFF |
                              TWAI_ALERT_BUS_RECOVERED |
                              TWAI_ALERT_ARB_LOST |
                              TWAI_ALERT_TX_FAILED;

    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_RETURN_ON_ERROR(twai_driver_install(&g_config, &t_config, &f_config),
                        TAG, "TWAI driver install failed");
    ESP_RETURN_ON_ERROR(twai_start(), TAG, "TWAI start failed");

    s_running = true;
    s_rx_count = 0;
    xTaskCreatePinnedToCore(can_rx_task, "can_rx", 4096, NULL, 5, &s_rx_task, 0);
    xTaskCreatePinnedToCore(can_alert_task, "can_alert", 3072, NULL, 4, &s_alert_task, 0);

    ESP_LOGI(TAG, "TWAI started - TX=%d RX=%d speed=%lu bps (fixed)",
             tx_pin, rx_pin, (unsigned long)CAN_FIXED_SPEED_BPS);
    log_twai_status("TWAI initial status");
    return ESP_OK;
}

esp_err_t can_manager_autodetect_speed(uint8_t tx_pin, uint8_t rx_pin, uint32_t *detected_speed)
{
    (void)tx_pin;
    (void)rx_pin;
    if (detected_speed == NULL) return ESP_ERR_INVALID_ARG;
    *detected_speed = CAN_FIXED_SPEED_BPS;
    return ESP_OK;
}

esp_err_t can_manager_deinit(void)
{
    if (!s_running) return ESP_OK;

    s_running = false;
    if (s_rx_task || s_alert_task) {
        vTaskDelay(pdMS_TO_TICKS(250));
        s_rx_task = NULL;
        s_alert_task = NULL;
    }

    twai_stop();
    twai_driver_uninstall();
    ESP_LOGI(TAG, "TWAI stopped");
    return ESP_OK;
}

void can_manager_set_rx_callback(can_rx_callback_t cb)
{
    s_rx_cb = cb;
}

esp_err_t can_manager_transmit(const can_frame_t *frame)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;

    twai_message_t msg = {
        .identifier = frame->id,
        .data_length_code = frame->dlc,
        .extd = 0,
        .rtr = 0,
    };
    memcpy(msg.data, frame->data, frame->dlc);
    return twai_transmit(&msg, pdMS_TO_TICKS(10));
}

bool can_manager_is_running(void)
{
    return s_running;
}
