#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_lvgl_port.h"
#include "freertos/semphr.h"
#include "display_manager.h"
#include "gauge_ui.h"
#include "app_config.h"

static const char *TAG = "display";

static lv_display_t            *s_disp   = NULL;
static i2c_master_dev_handle_t  s_tp_dev = NULL;
static SemaphoreHandle_t        s_tp_sem = NULL;
static TickType_t               s_tp_last_switch_tick = 0;

// ─── SH8601 custom init sequence ─────────────────────────────────────────────
// Key lines:
//   0x3A/0x55  → pixel format RGB565
//   0x51/0xFF  → AMOLED brightness = maximum  ← required or screen stays black
//   0x53/0x20  → write-control-display (enable backlight)
//   0x63/0xFF  → HBM brightness = maximum
//   0x2A/…    → column address 6..477 (472 px)
//   0x2B/…    → row address 0..465   (466 px)
//   0x11       → sleep-out (120 ms)
//   0x29       → display-on
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t []){0x00}, 0, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0x35, (uint8_t []){0x00}, 0, 10},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x51, (uint8_t []){0xFF}, 1, 10},
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x2A, (uint8_t []){0x00, 0x06, 0x01, 0xDD}, 4, 0},
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 0},
};

// ─── Rounder: SH8601 requires even-pixel-boundary transfers ──────────────────
static void rounder_cb(lv_area_t *area)
{
    if (area->x1 & 1) area->x1--;
    if (!(area->x2 & 1)) area->x2++;
    if (area->y1 & 1) area->y1--;
    if (!(area->y2 & 1)) area->y2++;
}

// ─── QSPI + SH8601 Panel Init ────────────────────────────────────────────────

static void panel_init(esp_lcd_panel_io_handle_t *io_out,
                       esp_lcd_panel_handle_t    *panel_out)
{
    // LCD_EN — power enable for the AMOLED supply rail
    gpio_config_t en_cfg = {
        .pin_bit_mask = (1ULL << LCD_EN),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&en_cfg));
    gpio_set_level(LCD_EN, 1);

    // QSPI bus — four data lines
    spi_bus_config_t buscfg = {
        .sclk_io_num     = LCD_PCLK,
        .data0_io_num    = LCD_DATA0,
        .data1_io_num    = LCD_DATA1,
        .data2_io_num    = LCD_DATA2,
        .data3_io_num    = LCD_DATA3,
        .max_transfer_sz = (size_t)LCD_H_RES * LCD_V_RES * LCD_BIT_PER_PIXEL / 8,
        .flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "QSPI bus initialised");

    // Panel IO — QSPI mode; SH8601 encodes cmd/data in the packet, no DC pin
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num       = LCD_CS,
        .dc_gpio_num       = -1,         // No DC line for QSPI
        .pclk_hz           = LCD_SPI_CLK_HZ,
        .lcd_cmd_bits      = 32,         // SH8601 QSPI command phase width
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 40,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, io_out));

    // Vendor config — MUST set use_qspi_interface=1.
    // Without it the driver defaults to single-wire SPI and nothing is displayed.
    sh8601_vendor_config_t vendor_config = {
        .init_cmds      = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(sh8601_lcd_init_cmd_t),
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    // SH8601 panel — 466×466 round AMOLED
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config  = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(*io_out, &panel_config, panel_out));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(*panel_out));
    ESP_ERROR_CHECK(esp_lcd_panel_init(*panel_out));

    // Apply the 6-column hardware offset
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(*panel_out, LCD_COL_OFFSET, LCD_ROW_OFFSET));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*panel_out, true));

    ESP_LOGI(TAG, "SH8601 panel ready");
}

// ─── Direct I2C touch — no registry component needed ─────────────────────────

// ISR: fires on falling edge of TP_INT.  Gives semaphore to wake tp_task.
static void IRAM_ATTR tp_int_isr(void *arg)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_tp_sem, &hp);
    portYIELD_FROM_ISR(hp);
}

// Task: reads the CST816S registers on every INT pulse to determine whether
// the event is a finger-down (press) or finger-up (release), then cycles the
// gauge face only on genuine presses.
//
// The CST816S fires INT on BOTH press and release.  Rather than trying to
// suppress the release via timing drains (which breaks on slow face builds),
// we read reg 0x02 (TD_STATUS / finger count): 0 = no touch = release, skip.
// Debounce is kept as a belt-and-suspenders guard against glitches.
static void tp_task(void *pvarg)
{
    uint8_t reg = 0x00;
    uint8_t buf[7];
    const TickType_t debounce_ticks = pdMS_TO_TICKS(300);

    while (1) {
        if (xSemaphoreTake(s_tp_sem, portMAX_DELAY) != pdTRUE) continue;

        // Read registers to clear INT and identify event type.
        // buf[2] = TD_STATUS (finger count): 0 on release, 1 on press.
        memset(buf, 0, sizeof(buf));
        i2c_master_transmit_receive(s_tp_dev, &reg, 1, buf, sizeof(buf), 20);

        // Ignore release events — only act on finger-down.
        // buf[2] = TD_STATUS (finger count): 0 on release for most firmware.
        // buf[3] bits 7:6 = event flag: 0x00=down, 0x40=up — reliable on all variants.
        // Check both so either one alone is sufficient to identify a release.
        if (buf[2] == 0 || (buf[3] & 0xC0) == 0x40) continue;

        // Debounce guard
        TickType_t now = xTaskGetTickCount();
        if ((now - s_tp_last_switch_tick) < debounce_ticks) continue;
        s_tp_last_switch_tick = now;

        // Cycle to the next enabled gauge face
        if (lvgl_port_lock(100)) {
            gauge_ui_next_face();
            lvgl_port_unlock();
        }

        // The dial face build calls lv_canvas_finish_layer which does synchronous
        // SW rendering into PSRAM while holding the LVGL lock — this can take
        // >300 ms.  By the time we reach here the original 300 ms debounce window
        // (started at T_press) may have already expired.  Without the drain below,
        // the release INT that fired during the build sits in the semaphore and
        // the next loop iteration sees it, passes both the register filter (stale
        // press data) and the expired debounce, and triggers a second face switch.
        //
        // Fix: drain any events queued during the build, then anchor the debounce
        // window to build-completion time.  This is safe to combine with the
        // register filter: genuine release events hit `continue` before reaching
        // s_tp_last_switch_tick so they never corrupt the timer.
        while (xSemaphoreTake(s_tp_sem, 0) == pdTRUE) {}
        s_tp_last_switch_tick = xTaskGetTickCount();
    }
}

// Probe for the touch IC and set up the GPIO interrupt.
// Hynitron ICs commonly respond at 0x15, 0x5A, or 0x2E.
// Non-fatal — the display works fine without touch.
static void touch_init(void)
{
    // Give the touch IC time to power up (it shares the AMOLED supply rail)
    vTaskDelay(pdMS_TO_TICKS(200));

    // ── I2C master bus ────────────────────────────────────────────────────────
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_HOST_ID,
        .sda_io_num        = TP_SDA,
        .scl_io_num        = TP_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, // add margin if board pull-ups are weak
    };
    i2c_master_bus_handle_t i2c_bus;
    if (i2c_new_master_bus(&bus_cfg, &i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "touch: I2C bus init failed — running without touch");
        return;
    }

    // ── Auto-detect touch IC address ─────────────────────────────────────────
    const uint8_t probe_addrs[] = {0x15, 0x5A, 0x2E};
    uint8_t tp_addr = 0;
    for (int i = 0; i < (int)(sizeof(probe_addrs)); i++) {
        if (i2c_master_probe(i2c_bus, probe_addrs[i], 20) == ESP_OK) {
            tp_addr = probe_addrs[i];
            ESP_LOGI(TAG, "touch: IC found at I2C addr 0x%02X", tp_addr);
            break;
        }
    }
    if (tp_addr == 0) {
        ESP_LOGW(TAG, "touch: no IC found — running without touch");
        i2c_del_master_bus(i2c_bus);
        return;
    }

    // ── Register I2C device ───────────────────────────────────────────────────
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = tp_addr,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_tp_dev) != ESP_OK) {
        ESP_LOGW(TAG, "touch: device registration failed");
        i2c_del_master_bus(i2c_bus);
        return;
    }

    // ── GPIO falling-edge interrupt on TP_INT ─────────────────────────────────
    s_tp_sem = xSemaphoreCreateBinary();
    if (!s_tp_sem) {
        ESP_LOGW(TAG, "touch: semaphore alloc failed");
        return;
    }

    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << TP_INT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&int_cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)TP_INT, tp_int_isr, NULL));

    // ── Touch task on core 0 ──────────────────────────────────────────────────
    xTaskCreatePinnedToCore(tp_task, "tp_task", 4096, NULL, 3, NULL, 1);

    ESP_LOGI(TAG, "touch: ready (addr=0x%02X, INT=GPIO%d)", tp_addr, TP_INT);
}

// ─── Public API ───────────────────────────────────────────────────────────────

lv_display_t *display_manager_init(void)
{
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t    panel_handle;
    panel_init(&io_handle, &panel_handle);

    touch_init();

    // LVGL port
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_priority   = 4;
    lvgl_cfg.task_stack      = 8192;
    lvgl_cfg.task_affinity   = 1;   // Core 1 — keeps CAN and WiFi on core 0
    lvgl_cfg.timer_period_ms = 5;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

#if LV_USE_LODEPNG
    lv_lodepng_init();   // PNG decoder for custom dial images
#endif

    // Add display — buffers in PSRAM (large 466×466 frame)
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io_handle,
        .panel_handle  = panel_handle,
        .buffer_size   = (uint32_t)DISP_WIDTH * LVGL_BUF_LINES,
        .double_buffer = true,
        .hres          = DISP_WIDTH,
        .vres          = DISP_HEIGHT,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rounder_cb    = rounder_cb,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,    // AMOLED buffer is ~430 KB — use PSRAM
            .swap_bytes  = true,    // SH8601 expects big-endian RGB565
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return NULL;
    }

    ESP_LOGI(TAG, "LVGL display ready: %dx%d (round AMOLED)", DISP_WIDTH, DISP_HEIGHT);
    return s_disp;
}

void display_show_splash(void)
{
    if (!s_disp) return;

    if (lvgl_port_lock(portMAX_DELAY)) {
        lv_obj_t *scr = lv_display_get_screen_active(s_disp);
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

        lv_obj_t *title = lv_label_create(scr);
        lv_label_set_text(title, "OpenGauge");
        lv_obj_set_style_text_color(title, lv_color_hex(0x00B4D8), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

        lv_obj_t *sub = lv_label_create(scr);
        lv_label_set_text(sub, "Initialising...");
        lv_obj_set_style_text_color(sub, lv_color_hex(0x555555), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 14);

        lvgl_port_unlock();
    }

    vTaskDelay(pdMS_TO_TICKS(1500));
}

void display_show_gauge_screen(void)
{
    if (!s_disp) return;

    if (lvgl_port_lock(portMAX_DELAY)) {
        lv_obj_t *scr = lv_display_get_screen_active(s_disp);
        lv_obj_clean(scr);
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        gauge_ui_create(scr);
        lvgl_port_unlock();
    }
}
