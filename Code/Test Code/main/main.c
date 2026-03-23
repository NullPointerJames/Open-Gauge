#include <stdio.h>
#include <math.h> 
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h" 
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h" 
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_cst9217.h" 

static const char *TAG = "main";

// --- PIN DEFINITIONS ---
#define LCD_CS          10
#define LCD_PCLK        12 
#define LCD_DATA0       11 
#define LCD_DATA1       13 
#define LCD_DATA2       14 
#define LCD_DATA3       15 
#define LCD_RST         17
#define LCD_EN          16 

#define TP_SDA          7
#define TP_SCL          6
#define TP_INT          9
#define TP_RST          -1
#define I2C_HOST_ID     I2C_NUM_0

#define LCD_H_RES       472 
#define LCD_V_RES       466 
#define LCD_COL_OFFSET  6   
#define LCD_ROW_OFFSET  0
#define LCD_BIT_PER_PIXEL 16

// --- UI GLOBALS & STATE ---
static lv_display_t *disp = NULL;
static uint32_t flush_count = 0;

// UI Objects
static lv_obj_t *scale;
static lv_obj_t *scale_inner; // New Inner Scale
static lv_obj_t *arc;
static lv_obj_t *arc_inner; 
static lv_obj_t *label;
static lv_obj_t *fps_label;
static lv_obj_t *needle_line; 

// Style Definitions
enum GaugeStyle {
    STYLE_STANDARD = 0,
    STYLE_SCIFI,
    STYLE_HEAT,
    STYLE_MINIMAL,
    STYLE_RAINBOW,
    STYLE_ULTRA_THIN,
    STYLE_SEGMENTED,
    STYLE_BLOCKY,
    STYLE_NEEDLE, 
    STYLE_MAX
};

static int current_style = STYLE_STANDARD;

// --- INITIALIZATION COMMANDS ---
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t []){0x00}, 0, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0}, 
    {0x35, (uint8_t []){0x00}, 0, 10},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x51, (uint8_t []){0xFF}, 1, 10}, 
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x2A, (uint8_t []){0x00,0x06,0x01,0xDD}, 4, 0}, 
    {0x2B, (uint8_t []){0x00,0x00,0x01,0xD1}, 4, 0}, 
    {0x11, (uint8_t []){0x00}, 0, 120}, 
    {0x29, (uint8_t []){0x00}, 0, 0},   
};

// --- LVGL FLUSH & ROUNDER ---
static void rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    if (area->x1 & 1) area->x1--;
    if (!(area->x2 & 1)) area->x2++;
    if (area->y1 & 1) area->y1--;
    if (!(area->y2 & 1)) area->y2++;
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_display_t *disp_drv = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp_drv);
    return true;
}

static void lvgl_flush_cb(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *color_map) {
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp_drv);
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    
    // Fast Byte Swap
    uint32_t len = (offsetx2 - offsetx1 + 1) * (offsety2 - offsety1 + 1);
    uint32_t *buf32 = (uint32_t *)color_map;
    uint32_t i = 0;
    for (; i < len / 2; i++) {
        uint32_t x = buf32[i];
        buf32[i] = ((x & 0x00FF00FF) << 8) | ((x & 0xFF00FF00) >> 8);
    }
    if (len % 2) {
        uint16_t *buf16 = (uint16_t *)color_map;
        buf16[len - 1] = __builtin_bswap16(buf16[len - 1]);
    }
    
    flush_count++;
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
    if (err != ESP_OK) {
        lv_display_flush_ready(disp_drv);
    }
}

static void lvgl_touch_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)lv_indev_get_user_data(indev);
    
    esp_lcd_touch_read_data(tp);
    
    uint8_t touch_cnt = 0;
    esp_lcd_touch_point_data_t tp_points[1]; 
    
    esp_lcd_touch_get_data(tp, tp_points, &touch_cnt, 1);

    if (touch_cnt > 0) {
        data->point.x = tp_points[0].x;
        data->point.y = tp_points[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lv_tick_inc_cb(void *arg) {
    lv_tick_inc(1);
}

// --- STYLE HELPERS ---

void apply_static_style(int style) {
    // Reset common visibility
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(arc_inner, LV_OBJ_FLAG_HIDDEN); 
    lv_obj_add_flag(scale_inner, LV_OBJ_FLAG_HIDDEN); // Default inner scale hidden
    lv_obj_add_flag(needle_line, LV_OBJ_FLAG_HIDDEN); 
    
    lv_obj_set_style_pad_all(scale, 0, LV_PART_MAIN);

    switch (style) {
        case STYLE_STANDARD:
            lv_obj_set_style_arc_width(arc, 20, LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc, 20, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc, lv_color_hex(0x333333), LV_PART_MAIN); 
            lv_obj_set_style_length(scale, 10, LV_PART_ITEMS);
            lv_obj_set_style_length(scale, 20, LV_PART_INDICATOR);
            
            // Show Inner Arc & Scale for Standard
            lv_obj_remove_flag(arc_inner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(scale_inner, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_set_style_arc_width(arc_inner, 15, LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc_inner, 15, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc_inner, lv_color_hex(0x333333), LV_PART_MAIN);
            break;
            
        case STYLE_SCIFI:
            lv_obj_set_style_arc_width(arc, 30, LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc, 30, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc, lv_color_hex(0x003333), LV_PART_MAIN); 
            lv_obj_set_style_length(scale, 5, LV_PART_ITEMS);
            lv_obj_set_style_length(scale, 10, LV_PART_INDICATOR);
            break;
        case STYLE_HEAT:
            lv_obj_set_style_arc_width(arc, 25, LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc, 25, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc, lv_color_hex(0x442200), LV_PART_MAIN); 
            lv_obj_set_style_length(scale, 10, LV_PART_ITEMS);
            lv_obj_set_style_length(scale, 20, LV_PART_INDICATOR);
            break;
        case STYLE_MINIMAL:
            lv_obj_set_style_arc_width(arc, 5, LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc, 5, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc, lv_color_hex(0x222222), LV_PART_MAIN); 
            lv_obj_set_style_length(scale, 2, LV_PART_ITEMS);
            lv_obj_set_style_length(scale, 5, LV_PART_INDICATOR);
            break;
        case STYLE_RAINBOW:
            lv_obj_set_style_arc_width(arc, 20, LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc, 20, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc, lv_color_hex(0x333333), LV_PART_MAIN);
            lv_obj_set_style_length(scale, 10, LV_PART_ITEMS);
            lv_obj_set_style_length(scale, 20, LV_PART_INDICATOR);
            break;
        case STYLE_ULTRA_THIN:
            lv_obj_set_style_arc_width(arc, 2, LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc, 2, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc, lv_color_hex(0x222222), LV_PART_MAIN);
            lv_obj_set_style_length(scale, 2, LV_PART_ITEMS);
            lv_obj_set_style_length(scale, 4, LV_PART_INDICATOR);
            break;
        case STYLE_SEGMENTED:
            lv_obj_set_style_arc_width(arc, 15, LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc, 15, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc, lv_color_hex(0x444444), LV_PART_MAIN);
            lv_obj_set_style_length(scale, 5, LV_PART_ITEMS);
            lv_obj_set_style_length(scale, 10, LV_PART_INDICATOR);
            lv_obj_set_style_pad_all(scale, 5, LV_PART_MAIN); 
            break;
        case STYLE_BLOCKY:
            lv_obj_set_style_arc_width(arc, 40, LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc, 40, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc, lv_color_hex(0x222222), LV_PART_MAIN);
            lv_obj_set_style_length(scale, 0, LV_PART_ITEMS);
            lv_obj_set_style_length(scale, 0, LV_PART_INDICATOR);
            lv_obj_set_style_pad_all(scale, 0, LV_PART_MAIN);
            break;
        case STYLE_NEEDLE:
            lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(needle_line, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_set_style_length(scale, 10, LV_PART_ITEMS);
            lv_obj_set_style_length(scale, 20, LV_PART_INDICATOR);
            lv_obj_set_style_text_color(scale, lv_color_white(), LV_PART_MAIN);
            break;
    }
    lv_obj_invalidate(scale); 
}

static void screen_touch_event_cb(lv_event_t * e) {
    current_style++;
    if (current_style >= STYLE_MAX) current_style = 0;
    
    ESP_LOGI(TAG, "Switched to Style: %d", current_style);
    apply_static_style(current_style);
}

static void gauge_anim_cb(void * var, int32_t v) {
    lv_obj_t * obj = (lv_obj_t*)var;
    lv_arc_set_value(obj, v);
    
    lv_color_t c;
    
    switch (current_style) {
        case STYLE_STANDARD:
            if(v < 34) c = lv_palette_main(LV_PALETTE_GREEN);
            else if (v < 67) c = lv_palette_main(LV_PALETTE_ORANGE);
            else c = lv_palette_main(LV_PALETTE_RED);
            lv_obj_set_style_arc_color(obj, c, LV_PART_INDICATOR);
            
            // --- Update Inner Arc (Zero on Right) ---
            int32_t start_angle = 45 - (v * 270) / 100;
            while(start_angle < 0) start_angle += 360;
            
            lv_arc_set_angles(arc_inner, start_angle, 45);
            lv_obj_set_style_arc_color(arc_inner, c, LV_PART_INDICATOR);
            break;
            
        case STYLE_SCIFI:
            lv_obj_set_style_arc_color(obj, lv_palette_main(LV_PALETTE_CYAN), LV_PART_INDICATOR);
            break;
            
        case STYLE_HEAT:
            if(v < 50) c = lv_palette_main(LV_PALETTE_YELLOW);
            else c = lv_palette_main(LV_PALETTE_RED);
            lv_obj_set_style_arc_color(obj, c, LV_PART_INDICATOR);
            break;
            
        case STYLE_MINIMAL:
            lv_obj_set_style_arc_color(obj, lv_color_white(), LV_PART_INDICATOR);
            break;
            
        case STYLE_RAINBOW:
            if(v < 20) c = lv_palette_main(LV_PALETTE_RED);
            else if (v < 40) c = lv_palette_main(LV_PALETTE_ORANGE);
            else if (v < 60) c = lv_palette_main(LV_PALETTE_YELLOW);
            else if (v < 80) c = lv_palette_main(LV_PALETTE_GREEN);
            else c = lv_palette_main(LV_PALETTE_BLUE);
            lv_obj_set_style_arc_color(obj, c, LV_PART_INDICATOR);
            break;
        
        case STYLE_ULTRA_THIN:
            lv_obj_set_style_arc_color(obj, lv_palette_main(LV_PALETTE_LIGHT_BLUE), LV_PART_INDICATOR);
            break;

        case STYLE_SEGMENTED:
            if(v < 34) c = lv_palette_main(LV_PALETTE_BLUE);
            else if (v < 67) c = lv_palette_main(LV_PALETTE_PURPLE);
            else c = lv_palette_main(LV_PALETTE_DEEP_PURPLE);
            lv_obj_set_style_arc_color(obj, c, LV_PART_INDICATOR);
            break;

        case STYLE_BLOCKY:
            lv_obj_set_style_arc_color(obj, lv_palette_main(LV_PALETTE_ORANGE), LV_PART_INDICATOR);
            break;

        case STYLE_NEEDLE:
            int32_t angle = 1350 + (v * 2700) / 100;
            lv_obj_set_style_transform_rotation(needle_line, angle, 0);
            break;
    }

    if(label) {
        lv_label_set_text_fmt(label, "%d%%", (int)v);
    }
}

static void fps_timer_cb(lv_timer_t * t) {
    if (fps_label) {
        lv_label_set_text_fmt(fps_label, "FPS: %lu", flush_count);
        flush_count = 0;
    }
}

// --- SETUP UI ---
void create_ui(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    
    lv_obj_add_event_cb(scr, screen_touch_event_cb, LV_EVENT_CLICKED, NULL);

    // 1. Arc (Outer) - Now child of scr
    arc = lv_arc_create(scr);
    lv_obj_set_size(arc, 466, 466);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_add_flag(arc, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 2. Arc (Inner) - Now child of scr, smaller
    arc_inner = lv_arc_create(scr);
    lv_obj_set_size(arc_inner, 380, 380);
    lv_obj_center(arc_inner);
    lv_arc_set_rotation(arc_inner, 0); 
    lv_arc_set_bg_angles(arc_inner, 135, 45); 
    lv_arc_set_value(arc_inner, 0);
    lv_obj_remove_style(arc_inner, NULL, LV_PART_KNOB);
    lv_obj_add_flag(arc_inner, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(arc_inner, LV_OBJ_FLAG_HIDDEN); 

    // 3. Scale (Outer) - Now child of scr, inside inner arc
    scale = lv_scale_create(scr);
    lv_obj_set_size(scale, 380, 380);
    lv_obj_center(scale);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_rotation(scale, 135);
    lv_scale_set_angle_range(scale, 270);
    lv_scale_set_range(scale, 0, 100);
    lv_obj_set_style_line_color(scale, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_length(scale, 10, LV_PART_ITEMS);
    lv_obj_set_style_length(scale, 20, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(scale, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_flag(scale, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 4. Scale (Inner) - Now child of scr, inside outer scale
    scale_inner = lv_scale_create(scr);
    lv_obj_set_size(scale_inner, 260, 260);
    lv_obj_center(scale_inner);
    lv_scale_set_mode(scale_inner, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_rotation(scale_inner, 135);
    lv_scale_set_angle_range(scale_inner, 270);
    lv_scale_set_range(scale_inner, 100, 0); 
    lv_obj_set_style_line_color(scale_inner, lv_color_make(200, 200, 200), LV_PART_ITEMS);
    lv_obj_set_style_length(scale_inner, 8, LV_PART_ITEMS);
    lv_obj_set_style_length(scale_inner, 15, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(scale_inner, lv_color_make(200, 200, 200), LV_PART_MAIN);
    lv_obj_add_flag(scale_inner, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(scale_inner, LV_OBJ_FLAG_HIDDEN);

    // 5. Needle (Child of scr to be on top)
    needle_line = lv_line_create(scr);
    static lv_point_precise_t line_points[] = { {0, 0}, {140, 0} }; 
    lv_line_set_points(needle_line, line_points, 2);
    
    lv_obj_set_style_line_width(needle_line, 6, 0);
    lv_obj_set_style_line_rounded(needle_line, true, 0);
    lv_obj_set_style_line_color(needle_line, lv_palette_main(LV_PALETTE_RED), 0);
    
    lv_obj_align(needle_line, LV_ALIGN_CENTER, 70, 0); 
    lv_obj_set_style_transform_pivot_x(needle_line, 0, 0);
    lv_obj_set_style_transform_pivot_y(needle_line, 3, 0);
    
    lv_obj_add_flag(needle_line, LV_OBJ_FLAG_HIDDEN);
    
    // ---

    apply_static_style(STYLE_STANDARD);

    // Label - Now child of scale_inner (innermost) or scr
    // If we make it child of scale_inner, it inherits center.
    // scale_inner is 260x260.
    label = lv_label_create(scale_inner);
    lv_label_set_text(label, "0%");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0); 
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // FPS
    fps_label = lv_label_create(scr);
    lv_label_set_text(fps_label, "FPS: -");
    lv_obj_set_style_text_color(fps_label, lv_color_hex(0x888888), 0);
    lv_obj_align(fps_label, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_timer_create(fps_timer_cb, 1000, NULL);
    lv_obj_add_flag(fps_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Animation
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, gauge_anim_cb);
    lv_anim_set_time(&a, 4000);        
    lv_anim_set_playback_time(&a, 4000); 
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_start(&a);
}

// --- MAIN TASK ---
void gui_task(void *arg) {
    // 1. Power On
    gpio_config_t pwr_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << LCD_EN
    };
    ESP_ERROR_CHECK(gpio_config(&pwr_conf));
    gpio_set_level(LCD_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 2. Touch Init
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TP_SDA,
        .scl_io_num = TP_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_HOST_ID, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_HOST_ID, i2c_conf.mode, 0, 0, 0));

    // 3. LVGL Init
    lv_init();
    disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, disp);

    // 4. Buffer
    size_t buf_size = (LCD_H_RES * LCD_V_RES / 4) * sizeof(uint16_t);
    uint8_t *buf1 = heap_caps_aligned_alloc(64, buf_size, MALLOC_CAP_SPIRAM); 
    uint8_t *buf2 = heap_caps_aligned_alloc(64, buf_size, MALLOC_CAP_SPIRAM); 
    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 5. SPI Bus
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PCLK,
        .data0_io_num = LCD_DATA0,
        .data1_io_num = LCD_DATA1,
        .data2_io_num = LCD_DATA2,
        .data3_io_num = LCD_DATA3,
        .max_transfer_sz = buf_size + 100,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // 6. Panel IO
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 80 * 1000 * 1000, 
        .trans_queue_depth = 40, 
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = { .quad_mode = 1 },
        .on_color_trans_done = notify_lvgl_flush_ready,
        .user_ctx = disp,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io_handle));

    // 7. Panel Driver
    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(sh8601_lcd_init_cmd_t),
        .flags = { .use_qspi_interface = 1 }
    };
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, LCD_COL_OFFSET, LCD_ROW_OFFSET));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    lv_display_set_user_data(disp, panel_handle);

    // 8. Touch Driver
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(I2C_HOST_ID, &tp_io_config, &tp_io_handle));
    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TP_RST,
        .int_gpio_num = TP_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, &tp));

    lv_indev_t *lv_touch_indev = lv_indev_create();
    lv_indev_set_type(lv_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(lv_touch_indev, tp);
    lv_indev_set_read_cb(lv_touch_indev, lvgl_touch_cb);

    // 9. UI & Timer
    create_ui();
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lv_tick_inc_cb,
        .name = "lv_tick"
    };
    esp_timer_handle_t lvgl_tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 1000));

    // 10. Loop
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void) {
    xTaskCreate(gui_task, "gui_task", 32 * 1024, NULL, 5, NULL);
}