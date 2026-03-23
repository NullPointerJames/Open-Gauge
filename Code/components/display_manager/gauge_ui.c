#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gauge_ui.h"
#include "config_manager.h"
#include "app_config.h"

// ─── Fixed palette (not user-configurable) ────────────────────────────────────
#define C_BG   0x000000
#define C_DIM  0x666666
#define C_DIV  0x2A2A2A

// Config-driven colours — call cfg->colors.* at runtime
#define COL_NORMAL(c)  lv_color_hex((c)->colors.normal_rgb)
#define COL_WARN(c)    lv_color_hex((c)->colors.warn_rgb)
#define COL_DANGER(c)  lv_color_hex((c)->colors.danger_rgb)
#define COL_TEXT(c)    lv_color_hex((c)->colors.text_rgb)
#define COL_ARC_BG(c)  lv_color_hex((c)->colors.arc_bg_rgb)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// EMA smoothing factor for the needle / indicator arc position.
// 0.80 = 80% previous + 20% new each frame.  At 30 fps this gives a
// ~160 ms settling time (5 frames to ~67%) which removes CAN-tick jitter
// while staying visually snappy on genuine fast changes.
// Text labels and threshold/peak logic always use the raw unsmoothed value.
#define DIAL_SMOOTH_ALPHA 0.80f

// ─── Module state ─────────────────────────────────────────────────────────────
static lv_obj_t *s_screen     = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_arc_obj    = NULL;
static lv_obj_t *s_val_labels[FACE_MAX_SLOTS + 1];

static int  s_face_idx     = 0;
static int  s_active[GAUGE_FACES_MAX];
static int  s_active_count = 0;

static volatile bool s_faces_dirty = false;
static face_layout_t s_cur_layout  = FACE_LAYOUT_RPM_ARC;

// ─── Dial face state ──────────────────────────────────────────────────────────
static lv_obj_t  *s_dial_arc_ind;      // indicator arc (0→value)
static lv_obj_t  *s_dial_arc_peak;     // peak hold marker (thin arc tick)
static lv_obj_t  *s_dial_needle;       // needle lv_line (standard mode)
static lv_obj_t  *s_dial_val_lbl;      // large value text
static lv_obj_t  *s_dial_peak_lbl;     // small peak text
static lv_point_precise_t s_needle_pts[2]; // persistent points for lv_line

// Custom image widgets (set when user has uploaded images to SPIFFS)
static lv_obj_t *s_dial_bg_img;        // custom background PNG
static lv_obj_t *s_dial_needle_img;    // custom needle PNG (rotated each frame)

// Tick canvas — single ARGB8888 canvas replaces individual lv_line/lv_label objects.
// Buffer is PSRAM-allocated at build time and freed before every face switch.
static uint8_t *s_tick_canvas_data;

// Delta tracking — avoid invalidating LVGL objects when the value hasn't changed.
// arc_val range is 0-1000; -1 sentinel forces a full update on first frame.
static int32_t s_dial_last_arc_val;   // -1 = unset
static bool    s_dial_last_above;     // last rendered above-threshold state
// EMA smoothed needle position (0..1); -1.0 = unset, snaps on first frame.
static float   s_dial_smoothed_pct;

// Text delta tracking — lv_label_set_text always invalidates even when the
// string is identical, so we cache the last rendered strings and skip the
// call when nothing changed.  This prevents flooding the SPI DMA queue.
static char    s_dial_last_vbuf[16];   // last text shown in val label
static char    s_dial_last_pbuf[32];   // last text shown in peak label
static int32_t s_dial_last_peak_pa;    // last peak arc angle (-1 = hidden)

// Peak hold runtime state
static float    s_dial_peak_val;       // NaN = no active peak
static uint32_t s_dial_peak_drop_ms;   // tick count (ms) when value dropped below threshold
static bool     s_dial_above_thresh;   // was above threshold on previous frame

// ─── Warning overlay state ────────────────────────────────────────────────────
// Full-screen black panel created on top of every face.  Hidden when no
// high-priority warning is active; shown (and cycled every 2 s when multiple
// warnings fire) otherwise.  Destroyed and rebuilt with the face on every
// lv_obj_clean() call.
static lv_obj_t *s_warn_overlay   = NULL;
static lv_obj_t *s_warn_lbl_head  = NULL;  // "! WARNING !"
static lv_obj_t *s_warn_lbl_name  = NULL;  // warning label / field name
static lv_obj_t *s_warn_lbl_val   = NULL;  // current value
static int        s_warn_active_idx = -1;  // warning index currently on screen
static uint32_t   s_warn_cycle_ms  = 0;    // last cycle timestamp (ms)

// Delta tracking for the status label tint (low-priority warnings)
static bool       s_warn_lo_last   = false;

// ─── Forward declarations ─────────────────────────────────────────────────────
static void build_face(lv_obj_t *scr, const gauge_face_cfg_t *f);
static void build_warn_overlay(lv_obj_t *scr);

// ─── Field helpers ────────────────────────────────────────────────────────────
static float field_value(gauge_field_t f, const gauge_data_t *d)
{
    switch (f) {
        case GAUGE_FIELD_RPM:            return d->rpm;
        case GAUGE_FIELD_COOLANT_C:      return d->coolant_temp_c;
        case GAUGE_FIELD_OIL_TEMP_C:     return d->oil_temp_c;
        case GAUGE_FIELD_OIL_PRESS_BAR:  return d->oil_pressure_bar;
        case GAUGE_FIELD_TPS_PCT:        return d->tps_pct;
        case GAUGE_FIELD_MAP_KPA:        return d->map_kpa;
        case GAUGE_FIELD_LAMBDA:         return d->lambda1;
        case GAUGE_FIELD_BATTERY_V:      return d->battery_v;
        case GAUGE_FIELD_SPEED_KPH:      return d->vehicle_speed_kph;
        // Haltech extended
        case GAUGE_FIELD_FUEL_PRESS_BAR: return d->fuel_pressure_bar;
        case GAUGE_FIELD_IAT_C:          return d->iat_c;
        case GAUGE_FIELD_LAMBDA2:        return d->lambda2;
        case GAUGE_FIELD_INJ_DUTY_PRI:   return d->inj_duty_primary_pct;
        case GAUGE_FIELD_INJ_DUTY_SEC:   return d->inj_duty_secondary_pct;
        case GAUGE_FIELD_IGN_ANGLE:      return d->ignition_angle_leading;
        case GAUGE_FIELD_GEAR:           return (float)d->gear;
        case GAUGE_FIELD_INTAKE_CAM1:    return d->intake_cam_angle_1;
        case GAUGE_FIELD_INTAKE_CAM2:    return d->intake_cam_angle_2;
        case GAUGE_FIELD_TARGET_BOOST:   return d->target_boost_kpa;
        case GAUGE_FIELD_BARO_KPA:       return d->baro_kpa;
        case GAUGE_FIELD_EGT1:           return d->egt_c[0];
        case GAUGE_FIELD_EGT2:           return d->egt_c[1];
        case GAUGE_FIELD_EGT3:           return d->egt_c[2];
        case GAUGE_FIELD_EGT4:           return d->egt_c[3];
        case GAUGE_FIELD_EGT5:           return d->egt_c[4];
        case GAUGE_FIELD_EGT6:           return d->egt_c[5];
        case GAUGE_FIELD_EGT7:           return d->egt_c[6];
        case GAUGE_FIELD_EGT8:           return d->egt_c[7];
        case GAUGE_FIELD_EGT9:           return d->egt_c[8];
        case GAUGE_FIELD_EGT10:          return d->egt_c[9];
        case GAUGE_FIELD_EGT11:          return d->egt_c[10];
        case GAUGE_FIELD_EGT12:          return d->egt_c[11];
        case GAUGE_FIELD_FUEL_TEMP_C:    return d->fuel_temp_c;
        case GAUGE_FIELD_FUEL_CONSUMP:   return d->fuel_consumption_lhr;
        case GAUGE_FIELD_FUEL_ECONOMY:   return d->avg_fuel_economy_l;
        default:                         return 0.0f;
    }
}

static const char *field_label(gauge_field_t f)
{
    switch (f) {
        case GAUGE_FIELD_RPM:            return "RPM";
        case GAUGE_FIELD_COOLANT_C:      return "COOLANT";
        case GAUGE_FIELD_OIL_TEMP_C:     return "OIL T";
        case GAUGE_FIELD_OIL_PRESS_BAR:  return "OIL P";
        case GAUGE_FIELD_TPS_PCT:        return "TPS";
        case GAUGE_FIELD_MAP_KPA:        return "MAP";
        case GAUGE_FIELD_LAMBDA:         return "LAMBDA";
        case GAUGE_FIELD_BATTERY_V:      return "BATT";
        case GAUGE_FIELD_SPEED_KPH:      return "SPEED";
        // Haltech extended
        case GAUGE_FIELD_FUEL_PRESS_BAR: return "FUEL P";
        case GAUGE_FIELD_IAT_C:          return "IAT";
        case GAUGE_FIELD_LAMBDA2:        return "LAM 2";
        case GAUGE_FIELD_INJ_DUTY_PRI:   return "INJ PRI";
        case GAUGE_FIELD_INJ_DUTY_SEC:   return "INJ SEC";
        case GAUGE_FIELD_IGN_ANGLE:      return "IGN";
        case GAUGE_FIELD_GEAR:           return "GEAR";
        case GAUGE_FIELD_INTAKE_CAM1:    return "CAM 1";
        case GAUGE_FIELD_INTAKE_CAM2:    return "CAM 2";
        case GAUGE_FIELD_TARGET_BOOST:   return "T.BOOST";
        case GAUGE_FIELD_BARO_KPA:       return "BARO";
        case GAUGE_FIELD_EGT1:           return "EGT 1";
        case GAUGE_FIELD_EGT2:           return "EGT 2";
        case GAUGE_FIELD_EGT3:           return "EGT 3";
        case GAUGE_FIELD_EGT4:           return "EGT 4";
        case GAUGE_FIELD_EGT5:           return "EGT 5";
        case GAUGE_FIELD_EGT6:           return "EGT 6";
        case GAUGE_FIELD_EGT7:           return "EGT 7";
        case GAUGE_FIELD_EGT8:           return "EGT 8";
        case GAUGE_FIELD_EGT9:           return "EGT 9";
        case GAUGE_FIELD_EGT10:          return "EGT 10";
        case GAUGE_FIELD_EGT11:          return "EGT 11";
        case GAUGE_FIELD_EGT12:          return "EGT 12";
        case GAUGE_FIELD_FUEL_TEMP_C:    return "FUEL T";
        case GAUGE_FIELD_FUEL_CONSUMP:   return "FUEL/HR";
        case GAUGE_FIELD_FUEL_ECONOMY:   return "ECONOMY";
        default:                         return "--";
    }
}

static const char *field_unit(gauge_field_t f, const app_config_t *cfg)
{
    switch (f) {
        case GAUGE_FIELD_RPM:            return "RPM";
        case GAUGE_FIELD_COOLANT_C:
        case GAUGE_FIELD_OIL_TEMP_C:
        case GAUGE_FIELD_IAT_C:
        case GAUGE_FIELD_FUEL_TEMP_C:
        case GAUGE_FIELD_EGT1:  case GAUGE_FIELD_EGT2:  case GAUGE_FIELD_EGT3:
        case GAUGE_FIELD_EGT4:  case GAUGE_FIELD_EGT5:  case GAUGE_FIELD_EGT6:
        case GAUGE_FIELD_EGT7:  case GAUGE_FIELD_EGT8:  case GAUGE_FIELD_EGT9:
        case GAUGE_FIELD_EGT10: case GAUGE_FIELD_EGT11: case GAUGE_FIELD_EGT12:
            return cfg->display_temp_f ? "\xC2\xB0""F" : "\xC2\xB0""C";
        case GAUGE_FIELD_OIL_PRESS_BAR:
        case GAUGE_FIELD_FUEL_PRESS_BAR:
            return cfg->display_psi ? "PSI" : "bar";
        case GAUGE_FIELD_TPS_PCT:        return "%";
        case GAUGE_FIELD_MAP_KPA:
        case GAUGE_FIELD_TARGET_BOOST:
        case GAUGE_FIELD_BARO_KPA:
            return cfg->display_psi ? "PSI" : "kPa";
        case GAUGE_FIELD_LAMBDA:
        case GAUGE_FIELD_LAMBDA2:
            return cfg->display_afr ? "AFR" : "\xCE\xBB";
        case GAUGE_FIELD_BATTERY_V:      return "V";
        case GAUGE_FIELD_SPEED_KPH:      return cfg->display_mph ? "mph" : "km/h";
        case GAUGE_FIELD_INJ_DUTY_PRI:   return "%";
        case GAUGE_FIELD_INJ_DUTY_SEC:   return "%";
        case GAUGE_FIELD_IGN_ANGLE:      return "\xC2\xB0";
        case GAUGE_FIELD_GEAR:           return "";
        case GAUGE_FIELD_INTAKE_CAM1:    return "\xC2\xB0";
        case GAUGE_FIELD_INTAKE_CAM2:    return "\xC2\xB0";
        case GAUGE_FIELD_FUEL_CONSUMP:   return "L/hr";
        case GAUGE_FIELD_FUEL_ECONOMY:   return "L";
        default:                         return "";
    }
}

// Apply display unit conversion to a raw field value.
// Arc/needle position arithmetic always uses the raw CAN value — this is only
// for text labels and dial tick marks.
static float convert_value(gauge_field_t f, float v, const app_config_t *cfg)
{
    switch (f) {
        // Temperatures — °C → °F
        case GAUGE_FIELD_COOLANT_C:
        case GAUGE_FIELD_OIL_TEMP_C:
        case GAUGE_FIELD_IAT_C:
        case GAUGE_FIELD_FUEL_TEMP_C:
        case GAUGE_FIELD_EGT1:  case GAUGE_FIELD_EGT2:  case GAUGE_FIELD_EGT3:
        case GAUGE_FIELD_EGT4:  case GAUGE_FIELD_EGT5:  case GAUGE_FIELD_EGT6:
        case GAUGE_FIELD_EGT7:  case GAUGE_FIELD_EGT8:  case GAUGE_FIELD_EGT9:
        case GAUGE_FIELD_EGT10: case GAUGE_FIELD_EGT11: case GAUGE_FIELD_EGT12:
            return cfg->display_temp_f ? v * 9.0f / 5.0f + 32.0f : v;
        // kPa pressures → PSI  (1 kPa = 0.14504 PSI)
        case GAUGE_FIELD_MAP_KPA:
        case GAUGE_FIELD_TARGET_BOOST:
        case GAUGE_FIELD_BARO_KPA:
            return cfg->display_psi ? v * 0.14504f : v;
        // bar pressures → PSI  (1 bar = 14.5038 PSI)
        case GAUGE_FIELD_OIL_PRESS_BAR:
        case GAUGE_FIELD_FUEL_PRESS_BAR:
            return cfg->display_psi ? v * 14.5038f : v;
        // Speed — km/h → mph
        case GAUGE_FIELD_SPEED_KPH:
            return cfg->display_mph ? v * 0.621371f : v;
        // Lambda → AFR  (gasoline stoich = 14.7)
        case GAUGE_FIELD_LAMBDA:
        case GAUGE_FIELD_LAMBDA2:
            return cfg->display_afr ? v * 14.7f : v;
        default:
            return v;
    }
}

static void format_value(gauge_field_t f, float v, char *buf, size_t sz,
                         const app_config_t *cfg)
{
    float cv = convert_value(f, v, cfg);
    switch (f) {
        case GAUGE_FIELD_RPM:
            snprintf(buf, sz, "%d", (int)v); break;
        case GAUGE_FIELD_COOLANT_C:
        case GAUGE_FIELD_OIL_TEMP_C:
        case GAUGE_FIELD_IAT_C:
        case GAUGE_FIELD_FUEL_TEMP_C:
            snprintf(buf, sz, "%.0f\xC2\xB0", cv); break;
        case GAUGE_FIELD_OIL_PRESS_BAR:
            snprintf(buf, sz, "%.1f", cv); break;
        case GAUGE_FIELD_TPS_PCT:
            snprintf(buf, sz, "%.0f%%", v); break;
        case GAUGE_FIELD_MAP_KPA:
            snprintf(buf, sz, cfg->display_psi ? "%.1f" : "%.0f", cv); break;
        case GAUGE_FIELD_LAMBDA:
            snprintf(buf, sz, cfg->display_afr ? "%.2f" : "%.3f", cv); break;
        case GAUGE_FIELD_BATTERY_V:
            snprintf(buf, sz, "%.1f", v); break;
        case GAUGE_FIELD_SPEED_KPH:
            snprintf(buf, sz, "%d", (int)cv); break;
        // Haltech extended
        case GAUGE_FIELD_FUEL_PRESS_BAR:
            snprintf(buf, sz, cfg->display_psi ? "%.1f" : "%.2f", cv); break;
        case GAUGE_FIELD_LAMBDA2:
            snprintf(buf, sz, cfg->display_afr ? "%.2f" : "%.3f", cv); break;
        case GAUGE_FIELD_INJ_DUTY_PRI:   snprintf(buf, sz, "%.1f%%", v);  break;
        case GAUGE_FIELD_INJ_DUTY_SEC:   snprintf(buf, sz, "%.1f%%", v);  break;
        case GAUGE_FIELD_IGN_ANGLE:      snprintf(buf, sz, "%.1f",   v);  break;
        case GAUGE_FIELD_GEAR:           snprintf(buf, sz, "%d",     (int)v); break;
        case GAUGE_FIELD_INTAKE_CAM1:    snprintf(buf, sz, "%.1f",   v);  break;
        case GAUGE_FIELD_INTAKE_CAM2:    snprintf(buf, sz, "%.1f",   v);  break;
        case GAUGE_FIELD_TARGET_BOOST:
            snprintf(buf, sz, cfg->display_psi ? "%.1f" : "%.0f", cv); break;
        case GAUGE_FIELD_BARO_KPA:
            snprintf(buf, sz, "%.1f", cv); break;
        case GAUGE_FIELD_EGT1:  case GAUGE_FIELD_EGT2:  case GAUGE_FIELD_EGT3:
        case GAUGE_FIELD_EGT4:  case GAUGE_FIELD_EGT5:  case GAUGE_FIELD_EGT6:
        case GAUGE_FIELD_EGT7:  case GAUGE_FIELD_EGT8:  case GAUGE_FIELD_EGT9:
        case GAUGE_FIELD_EGT10: case GAUGE_FIELD_EGT11: case GAUGE_FIELD_EGT12:
            snprintf(buf, sz, "%.0f\xC2\xB0", cv); break;
        case GAUGE_FIELD_FUEL_CONSUMP:   snprintf(buf, sz, "%.1f",   v);  break;
        case GAUGE_FIELD_FUEL_ECONOMY:   snprintf(buf, sz, "%.1f",   v);  break;
        default:                         snprintf(buf, sz, "--");          break;
    }
}

static lv_color_t field_color(gauge_field_t f, float v, const app_config_t *cfg)
{
    switch (f) {
        case GAUGE_FIELD_RPM:
            if (v >= cfg->rpm_redline)             return COL_DANGER(cfg);
            if (v >= cfg->rpm_redline * 0.85f)     return COL_WARN(cfg);
            return COL_NORMAL(cfg);
        case GAUGE_FIELD_COOLANT_C:
            if (v >= cfg->coolant_warn_c + 5.0f)   return COL_DANGER(cfg);
            if (v >= cfg->coolant_warn_c)           return COL_WARN(cfg);
            return COL_TEXT(cfg);
        case GAUGE_FIELD_OIL_PRESS_BAR:
            if (v < cfg->oil_press_warn_bar * 0.75f) return COL_DANGER(cfg);
            if (v < cfg->oil_press_warn_bar)          return COL_WARN(cfg);
            return COL_TEXT(cfg);
        default:
            return COL_TEXT(cfg);
    }
}

// ─── Widget helpers ───────────────────────────────────────────────────────────

static lv_obj_t *make_cell(lv_obj_t *parent, const char *key, int cx, int y,
                            const app_config_t *cfg)
{
    lv_obj_t *k = lv_label_create(parent);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(k, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(k, 110);
    lv_obj_set_pos(k, cx - 55, y);
    lv_obj_add_flag(k, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(k, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_color(v, COL_TEXT(cfg), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(v, 110);
    lv_obj_set_pos(v, cx - 55, y + 15);
    lv_obj_add_flag(v, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    return v;
}

static void add_status_label(lv_obj_t *parent, const app_config_t *cfg)
{
    s_status_lbl = lv_label_create(parent);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_color(s_status_lbl, COL_NORMAL(cfg), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_status_lbl, 240);
    lv_obj_set_pos(s_status_lbl, DISP_CENTER_X - 120, 52);
    lv_obj_add_flag(s_status_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_status_lbl, LV_OBJ_FLAG_CLICKABLE);
}

static void add_ring(lv_obj_t *parent, int rotation, int start_a, int end_a, int width,
                     const app_config_t *cfg)
{
    lv_obj_t *ring = lv_arc_create(parent);
    lv_obj_set_size(ring, 418, 418);
    lv_obj_set_pos(ring, 24, 24);
    lv_arc_set_rotation(ring, rotation);
    lv_arc_set_bg_angles(ring, start_a, end_a);
    lv_arc_set_range(ring, 0, 100);
    lv_arc_set_value(ring, 100);
    lv_arc_set_mode(ring, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_arc_color(ring, COL_ARC_BG(cfg), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, COL_NORMAL(cfg), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ring, width, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_flag(ring, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
}

// ─── SPIFFS helper ────────────────────────────────────────────────────────────

static bool spiffs_file_exists(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

// ─── Dial tick marks ──────────────────────────────────────────────────────────
// All ticks and scale labels are pre-rendered into a single ARGB8888 canvas at
// build time.  One image blit per dirty frame instead of 31+ anti-aliased lines,
// and ~38 lv_obj_create calls reduced to one — making face-switch much faster.

static void build_dial_ticks(lv_obj_t *scr, const gauge_face_cfg_t *f,
                              const app_config_t *cfg)
{
    float range = f->dial_max - f->dial_min;
    if (range < 1e-6f) return;

    // Pick a "nice" major-tick interval targeting ~6 major ticks
    float rough = range / 6.0f;
    float mag   = powf(10.0f, floorf(log10f(rough)));
    float norm  = rough / mag;
    float major_step;
    if      (norm < 1.5f) major_step = 1.0f  * mag;
    else if (norm < 3.5f) major_step = 2.0f  * mag;
    else if (norm < 7.5f) major_step = 5.0f  * mag;
    else                  major_step = 10.0f * mag;

    float minor_step = major_step / 5.0f;

    // Tick radii (arc inner edge ≈ 197, so ticks sit just inside it)
    const float R_OUTER = 194.0f;
    const float R_MAJOR = 170.0f;
    const float R_MINOR = 184.0f;
    const float R_LABEL = 152.0f;

    // Allocate PSRAM buffer for the ARGB8888 canvas (466×466×4 ≈ 848 KB).
    // memset to 0 = fully transparent — ticks composite over the arcs below.
    size_t buf_sz = (size_t)DISP_WIDTH * DISP_HEIGHT * 4;
    s_tick_canvas_data = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM);
    if (!s_tick_canvas_data) return;   // out of PSRAM — skip ticks gracefully
    memset(s_tick_canvas_data, 0, buf_sz);

    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, s_tick_canvas_data,
                         DISP_WIDTH, DISP_HEIGHT, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    // lv_draw_label stores a pointer — it does NOT copy the text string.
    // All label strings must remain valid until lv_canvas_finish_layer returns.
    // Use a persistent pool; nice-scale targets ~6 major ticks so 20 is ample.
    char label_strs[20][12];
    int  label_cnt = 0;

    float v0 = ceilf(f->dial_min / minor_step) * minor_step;

    for (float v = v0; v <= f->dial_max + minor_step * 0.01f; v += minor_step) {
        float pct = (v - f->dial_min) / range;
        float angle_rad = (135.0f + pct * 270.0f) * (float)M_PI / 180.0f;
        float ca = cosf(angle_rad);
        float sa = sinf(angle_rad);

        bool  is_major = (fabsf(fmodf(v - f->dial_min, major_step)) < major_step * 0.02f);
        float r_inner  = is_major ? R_MAJOR : R_MINOR;

        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = lv_color_hex(0xAAAAAA);
        line_dsc.width = is_major ? 2 : 1;
        line_dsc.opa   = LV_OPA_COVER;
        line_dsc.p1.x  = (lv_value_precise_t)(DISP_CENTER_X + R_OUTER * ca);
        line_dsc.p1.y  = (lv_value_precise_t)(DISP_CENTER_Y + R_OUTER * sa);
        line_dsc.p2.x  = (lv_value_precise_t)(DISP_CENTER_X + r_inner * ca);
        line_dsc.p2.y  = (lv_value_precise_t)(DISP_CENTER_Y + r_inner * sa);
        lv_draw_line(&layer, &line_dsc);

        if (is_major && label_cnt < 20) {
            float lx = DISP_CENTER_X + R_LABEL * ca;
            float ly = DISP_CENTER_Y + R_LABEL * sa;

            char *tbuf = label_strs[label_cnt++];
            float vd = convert_value(f->slots[0], v, cfg);
            if (fabsf(vd - roundf(vd)) < 0.05f)
                snprintf(tbuf, 12, "%d", (int)roundf(vd));
            else
                snprintf(tbuf, 12, "%.1f", vd);

            lv_draw_label_dsc_t lbl_dsc;
            lv_draw_label_dsc_init(&lbl_dsc);
            lbl_dsc.color = lv_color_hex(0xAAAAAA);
            lbl_dsc.font  = &lv_font_montserrat_12;
            lbl_dsc.align = LV_TEXT_ALIGN_CENTER;
            lbl_dsc.text  = tbuf;
            lbl_dsc.opa   = LV_OPA_COVER;
            lv_area_t coords = {
                (lv_coord_t)((int)lx - 24), (lv_coord_t)((int)ly - 9),
                (lv_coord_t)((int)lx + 23), (lv_coord_t)((int)ly + 8)
            };
            lv_draw_label(&layer, &lbl_dsc, &coords);
        }
    }

    lv_canvas_finish_layer(canvas, &layer);
}

// ─── Face builders ────────────────────────────────────────────────────────────

static void build_rpm_arc_face(lv_obj_t *scr, const gauge_face_cfg_t *f,
                                const app_config_t *cfg)
{
    s_arc_obj = lv_arc_create(scr);
    lv_obj_set_size(s_arc_obj, 430, 430);
    lv_obj_set_pos(s_arc_obj, 18, 18);
    lv_arc_set_rotation(s_arc_obj, 135);
    lv_arc_set_bg_angles(s_arc_obj, 0, 270);
    lv_arc_set_range(s_arc_obj, 0, (int32_t)cfg->rpm_redline);
    lv_arc_set_value(s_arc_obj, 0);
    lv_arc_set_mode(s_arc_obj, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_arc_color(s_arc_obj, COL_ARC_BG(cfg), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_obj, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_obj, COL_NORMAL(cfg), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc_obj, 18, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_arc_obj, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_flag(s_arc_obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_arc_obj, LV_OBJ_FLAG_CLICKABLE);

    s_val_labels[0] = lv_label_create(scr);
    lv_label_set_text(s_val_labels[0], "0");
    lv_obj_set_style_text_color(s_val_labels[0], COL_TEXT(cfg), 0);
    lv_obj_set_style_text_font(s_val_labels[0], &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_val_labels[0], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_val_labels[0], 200);
    lv_obj_set_pos(s_val_labels[0], DISP_CENTER_X - 100, 193);
    lv_obj_add_flag(s_val_labels[0], LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_val_labels[0], LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *unit = lv_label_create(scr);
    lv_label_set_text(unit, "RPM");
    lv_obj_set_style_text_color(unit, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(unit, 200);
    lv_obj_set_pos(unit, DISP_CENTER_X - 100, 230);
    lv_obj_add_flag(unit, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(unit, LV_OBJ_FLAG_CLICKABLE);

    add_status_label(scr, cfg);

    static const int cx[] = { 116, 350 };
    static const int cy[] = { 330, 330 };
    for (int i = 0; i < 2; i++) {
        gauge_field_t fld = f->slots[i];
        s_val_labels[i + 1] = make_cell(scr, field_label(fld), cx[i], cy[i], cfg);
    }
}

static void build_single_face(lv_obj_t *scr, const gauge_face_cfg_t *f,
                               const app_config_t *cfg)
{
    gauge_field_t fld = f->slots[0];

    add_ring(scr, 135, 0, 270, 4, cfg);
    add_status_label(scr, cfg);

    lv_obj_t *key = lv_label_create(scr);
    lv_label_set_text(key, field_label(fld));
    lv_obj_set_style_text_color(key, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(key, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(key, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(key, 300);
    lv_obj_set_pos(key, DISP_CENTER_X - 150, 178);
    lv_obj_add_flag(key, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(key, LV_OBJ_FLAG_CLICKABLE);

    s_val_labels[0] = lv_label_create(scr);
    lv_label_set_text(s_val_labels[0], "--");
    lv_obj_set_style_text_color(s_val_labels[0], COL_NORMAL(cfg), 0);
    lv_obj_set_style_text_font(s_val_labels[0], &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_val_labels[0], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_val_labels[0], 300);
    lv_obj_set_pos(s_val_labels[0], DISP_CENTER_X - 150, 210);
    lv_obj_add_flag(s_val_labels[0], LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_val_labels[0], LV_OBJ_FLAG_CLICKABLE);
}

static void build_dual_face(lv_obj_t *scr, const gauge_face_cfg_t *f,
                             const app_config_t *cfg)
{
    add_ring(scr, 0, 0, 360, 4, cfg);
    add_status_label(scr, cfg);

    lv_obj_t *div = lv_obj_create(scr);
    lv_obj_set_size(div, 320, 2);
    lv_obj_set_pos(div, DISP_CENTER_X - 160, DISP_CENTER_Y - 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(C_DIV), 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);
    lv_obj_set_style_pad_all(div, 0, 0);
    lv_obj_add_flag(div, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    {
        lv_obj_t *k = lv_label_create(scr);
        lv_label_set_text(k, field_label(f->slots[0]));
        lv_obj_set_style_text_color(k, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(k, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(k, 280);
        lv_obj_set_pos(k, DISP_CENTER_X - 140, 128);
        lv_obj_add_flag(k, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(k, LV_OBJ_FLAG_CLICKABLE);

        s_val_labels[0] = lv_label_create(scr);
        lv_label_set_text(s_val_labels[0], "--");
        lv_obj_set_style_text_color(s_val_labels[0], COL_NORMAL(cfg), 0);
        lv_obj_set_style_text_font(s_val_labels[0], &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_align(s_val_labels[0], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_val_labels[0], 280);
        lv_obj_set_pos(s_val_labels[0], DISP_CENTER_X - 140, 152);
        lv_obj_add_flag(s_val_labels[0], LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(s_val_labels[0], LV_OBJ_FLAG_CLICKABLE);
    }

    {
        lv_obj_t *k = lv_label_create(scr);
        lv_label_set_text(k, field_label(f->slots[1]));
        lv_obj_set_style_text_color(k, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(k, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(k, 280);
        lv_obj_set_pos(k, DISP_CENTER_X - 140, 258);
        lv_obj_add_flag(k, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(k, LV_OBJ_FLAG_CLICKABLE);

        s_val_labels[1] = lv_label_create(scr);
        lv_label_set_text(s_val_labels[1], "--");
        lv_obj_set_style_text_color(s_val_labels[1], COL_NORMAL(cfg), 0);
        lv_obj_set_style_text_font(s_val_labels[1], &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_align(s_val_labels[1], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_val_labels[1], 280);
        lv_obj_set_pos(s_val_labels[1], DISP_CENTER_X - 140, 282);
        lv_obj_add_flag(s_val_labels[1], LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(s_val_labels[1], LV_OBJ_FLAG_CLICKABLE);
    }
}

static void build_quad_face(lv_obj_t *scr, const gauge_face_cfg_t *f,
                             const app_config_t *cfg)
{
    add_ring(scr, 0, 0, 360, 4, cfg);
    add_status_label(scr, cfg);

    static const int qx[] = { 130, 336, 130, 336 };
    static const int qy[] = { 148, 148, 293, 293 };
    for (int i = 0; i < 4; i++) {
        gauge_field_t fld = f->slots[i];
        s_val_labels[i] = make_cell(scr, field_label(fld), qx[i], qy[i], cfg);
    }
}

// ─── Dial face ────────────────────────────────────────────────────────────────

// Compute needle tip / tail coords from value percentage (0..1).
// Gauge rotation: 135° → 7:30 (start) sweeping clockwise 270° → 1:30 (end).
// At 0%: tip at lower-left; at 50%: tip at top; at 100%: tip at lower-right.
static void dial_needle_points(float pct, int tip_r, int tail_r, lv_point_precise_t pts[2])
{
    float angle_rad = (135.0f + pct * 270.0f) * (float)M_PI / 180.0f;
    float ca = cosf(angle_rad);
    float sa = sinf(angle_rad);
    // Tail goes opposite to needle
    pts[0].x = (lv_value_precise_t)(DISP_CENTER_X - (int)(tail_r * ca));
    pts[0].y = (lv_value_precise_t)(DISP_CENTER_Y - (int)(tail_r * sa));
    pts[1].x = (lv_value_precise_t)(DISP_CENTER_X + (int)(tip_r  * ca));
    pts[1].y = (lv_value_precise_t)(DISP_CENTER_Y + (int)(tip_r  * sa));
}

static void build_dial_face(lv_obj_t *scr, const gauge_face_cfg_t *f,
                             const app_config_t *cfg)
{
    gauge_field_t fld = f->slots[0];

    bool has_custom_bg     = spiffs_file_exists("/spiffs/custom_bg.png");
    bool has_custom_needle = spiffs_file_exists("/spiffs/custom_needle.png");

    float range = f->dial_max - f->dial_min;
    if (range < 1e-6f) range = 1.0f;

    if (has_custom_bg) {
        // ── Custom background image replaces all arcs ─────────────────────
        s_dial_bg_img = lv_image_create(scr);
        lv_image_set_src(s_dial_bg_img, "S:/custom_bg.png");
        lv_obj_set_pos(s_dial_bg_img, 0, 0);
        lv_obj_add_flag(s_dial_bg_img, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(s_dial_bg_img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        // ── Arc background (full scale track) ─────────────────────────────
        lv_obj_t *arc_bg = lv_arc_create(scr);
        lv_obj_set_size(arc_bg, 430, 430);
        lv_obj_set_pos(arc_bg, 18, 18);
        lv_arc_set_rotation(arc_bg, 135);
        lv_arc_set_bg_angles(arc_bg, 0, 270);
        lv_arc_set_range(arc_bg, 0, 1000);
        lv_arc_set_value(arc_bg, 0);
        lv_arc_set_mode(arc_bg, LV_ARC_MODE_NORMAL);
        lv_obj_set_style_arc_color(arc_bg, COL_ARC_BG(cfg), LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc_bg, 14, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc_bg, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc_bg, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_add_flag(arc_bg, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(arc_bg, LV_OBJ_FLAG_CLICKABLE);

        // ── Warning zone arc (threshold → max, danger colour) ─────────────
        float warn_pct = (f->dial_warn_threshold - f->dial_min) / range;
        warn_pct = warn_pct < 0.0f ? 0.0f : (warn_pct > 1.0f ? 1.0f : warn_pct);
        int warn_angle = (int)(warn_pct * 270.0f);

        lv_obj_t *arc_warn = lv_arc_create(scr);
        lv_obj_set_size(arc_warn, 430, 430);
        lv_obj_set_pos(arc_warn, 18, 18);
        lv_arc_set_rotation(arc_warn, 135);
        lv_arc_set_bg_angles(arc_warn, 0, 270);
        lv_arc_set_angles(arc_warn, warn_angle, 270);
        lv_obj_set_style_arc_opa(arc_warn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc_warn, 14, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc_warn, COL_DANGER(cfg), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc_warn, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_add_flag(arc_warn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(arc_warn, LV_OBJ_FLAG_CLICKABLE);

        // ── Value indicator arc (0 → current value, dynamic colour) ───────
        s_dial_arc_ind = lv_arc_create(scr);
        lv_obj_set_size(s_dial_arc_ind, 430, 430);
        lv_obj_set_pos(s_dial_arc_ind, 18, 18);
        lv_arc_set_rotation(s_dial_arc_ind, 135);
        lv_arc_set_bg_angles(s_dial_arc_ind, 0, 270);
        lv_arc_set_range(s_dial_arc_ind, 0, 1000);
        lv_arc_set_value(s_dial_arc_ind, 0);
        lv_arc_set_mode(s_dial_arc_ind, LV_ARC_MODE_NORMAL);
        lv_obj_set_style_arc_opa(s_dial_arc_ind, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_dial_arc_ind, 14, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_dial_arc_ind, COL_NORMAL(cfg), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_dial_arc_ind, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_add_flag(s_dial_arc_ind, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(s_dial_arc_ind, LV_OBJ_FLAG_CLICKABLE);

        // ── Peak hold marker (narrow tick, white) ─────────────────────────
        s_dial_arc_peak = lv_arc_create(scr);
        lv_obj_set_size(s_dial_arc_peak, 430, 430);
        lv_obj_set_pos(s_dial_arc_peak, 18, 18);
        lv_arc_set_rotation(s_dial_arc_peak, 135);
        lv_arc_set_bg_angles(s_dial_arc_peak, 0, 270);
        lv_arc_set_angles(s_dial_arc_peak, 0, 4);
        lv_obj_set_style_arc_opa(s_dial_arc_peak, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_dial_arc_peak, 14, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_dial_arc_peak, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_dial_arc_peak, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_add_flag(s_dial_arc_peak, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_dial_arc_peak, LV_OBJ_FLAG_CLICKABLE);

        // ── Tick marks + scale labels ──────────────────────────────────────
        build_dial_ticks(scr, f, cfg);
    }

    if (has_custom_needle) {
        // ── Custom needle image (rotated each frame around image centre) ───
        // Image must have the needle pointing straight DOWN (6 o'clock).
        // The image is centred on the display; pivot is the image centre.
        // Rotation formula: angle_tenths = (pct * 270 + 45) * 10
        s_dial_needle_img = lv_image_create(scr);
        lv_image_set_src(s_dial_needle_img, "S:/custom_needle.png");
        lv_obj_align(s_dial_needle_img, LV_ALIGN_CENTER, 0, 0);
        lv_image_set_pivot(s_dial_needle_img, LV_PCT(50), LV_PCT(50));
        lv_image_set_rotation(s_dial_needle_img, 450); // pct=0 initial
        lv_obj_add_flag(s_dial_needle_img, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(s_dial_needle_img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        // ── Standard lv_line needle ────────────────────────────────────────
        dial_needle_points(0.0f, 195, 22, s_needle_pts);

        s_dial_needle = lv_line_create(scr);
        lv_line_set_points(s_dial_needle, s_needle_pts, 2);
        lv_obj_set_style_line_color(s_dial_needle, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_line_width(s_dial_needle, 3, 0);
        lv_obj_set_style_line_rounded(s_dial_needle, true, 0);
        lv_obj_add_flag(s_dial_needle, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(s_dial_needle, LV_OBJ_FLAG_CLICKABLE);

        // ── Needle pivot circle ────────────────────────────────────────────
        lv_obj_t *pivot = lv_obj_create(scr);
        lv_obj_set_size(pivot, 16, 16);
        lv_obj_set_pos(pivot, DISP_CENTER_X - 8, DISP_CENTER_Y - 8);
        lv_obj_set_style_bg_color(pivot, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_radius(pivot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(pivot, 0, 0);
        lv_obj_set_style_pad_all(pivot, 0, 0);
        lv_obj_add_flag(pivot, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(pivot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    // ── Status label ─────────────────────────────────────────────────────────
    add_status_label(scr, cfg);

    // ── Value label (bottom dead zone, y≈318..393) ───────────────────────────
    // The needle sweeps top-half of the display; bottom dead zone is safe for text.
    s_dial_val_lbl = lv_label_create(scr);
    lv_label_set_text(s_dial_val_lbl, "--");
    lv_obj_set_style_text_color(s_dial_val_lbl, COL_TEXT(cfg), 0);
    lv_obj_set_style_text_font(s_dial_val_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_dial_val_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_dial_val_lbl, 200);
    lv_obj_set_pos(s_dial_val_lbl, DISP_CENTER_X - 100, 318);
    lv_obj_add_flag(s_dial_val_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_dial_val_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *unit_lbl = lv_label_create(scr);
    lv_label_set_text(unit_lbl, field_unit(fld, cfg));
    lv_obj_set_style_text_color(unit_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(unit_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(unit_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(unit_lbl, 160);
    lv_obj_set_pos(unit_lbl, DISP_CENTER_X - 80, 357);
    lv_obj_add_flag(unit_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(unit_lbl, LV_OBJ_FLAG_CLICKABLE);

    // ── Peak label ("PK 2.1", danger colour, hidden until active) ────────────
    s_dial_peak_lbl = lv_label_create(scr);
    lv_label_set_text(s_dial_peak_lbl, "");
    lv_obj_set_style_text_color(s_dial_peak_lbl, COL_DANGER(cfg), 0);
    lv_obj_set_style_text_font(s_dial_peak_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_dial_peak_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_dial_peak_lbl, 160);
    lv_obj_set_pos(s_dial_peak_lbl, DISP_CENTER_X - 80, 385);
    lv_obj_add_flag(s_dial_peak_lbl, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_dial_peak_lbl, LV_OBJ_FLAG_CLICKABLE);

    // ── Reset peak state ─────────────────────────────────────────────────────
    s_dial_peak_val     = NAN;
    s_dial_peak_drop_ms = 0;
    s_dial_above_thresh = false;
}

// ─── Build dispatcher ─────────────────────────────────────────────────────────
static void build_face(lv_obj_t *scr, const gauge_face_cfg_t *f)
{
    app_config_t *cfg = config_manager_get();

    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    s_arc_obj         = NULL;
    s_status_lbl      = NULL;
    s_dial_arc_ind    = NULL;
    s_dial_arc_peak   = NULL;
    s_dial_needle     = NULL;
    s_dial_val_lbl    = NULL;
    s_dial_peak_lbl   = NULL;
    s_dial_bg_img        = NULL;
    s_dial_needle_img    = NULL;
    s_tick_canvas_data   = NULL;   // caller freed before lv_obj_clean
    s_dial_last_arc_val  = -1;
    s_dial_last_above    = false;
    s_dial_smoothed_pct  = -1.0f;
    s_dial_last_vbuf[0]  = '\0';
    s_dial_last_pbuf[0]  = '\0';
    s_dial_last_peak_pa  = -1;
    s_warn_overlay    = NULL;
    s_warn_lbl_head   = NULL;
    s_warn_lbl_name   = NULL;
    s_warn_lbl_val    = NULL;
    s_warn_active_idx = -1;
    s_warn_lo_last    = false;
    memset(s_val_labels, 0, sizeof(s_val_labels));
    s_cur_layout = f->layout;

    switch (f->layout) {
        case FACE_LAYOUT_SINGLE: build_single_face(scr, f, cfg); break;
        case FACE_LAYOUT_DUAL:   build_dual_face(scr, f, cfg);   break;
        case FACE_LAYOUT_QUAD:   build_quad_face(scr, f, cfg);   break;
        case FACE_LAYOUT_DIAL:   build_dial_face(scr, f, cfg);   break;
        default:                 build_rpm_arc_face(scr, f, cfg); break;
    }
}

// ─── Active face list ─────────────────────────────────────────────────────────
static void rebuild_active_list(void)
{
    app_config_t *cfg = config_manager_get();
    s_active_count = 0;
    for (int i = 0; i < GAUGE_FACES_MAX; i++) {
        if (cfg->faces[i].enabled) {
            s_active[s_active_count++] = i;
        }
    }
    if (s_active_count == 0) {
        s_active[0]    = 0;
        s_active_count = 1;
    }
    if (s_face_idx >= s_active_count) s_face_idx = 0;
}

// ─── Public API ───────────────────────────────────────────────────────────────

void gauge_ui_create(lv_obj_t *parent)
{
    s_screen   = parent;
    s_face_idx = 0;
    rebuild_active_list();

    app_config_t *cfg = config_manager_get();
    build_face(parent, &cfg->faces[s_active[0]]);
    build_warn_overlay(parent);
}

void gauge_ui_next_face(void)
{
    if (!s_screen) return;

    s_face_idx = (s_face_idx + 1) % s_active_count;

    s_arc_obj         = NULL;
    s_status_lbl      = NULL;
    s_dial_arc_ind    = NULL;
    s_dial_arc_peak   = NULL;
    s_dial_needle     = NULL;
    s_dial_val_lbl    = NULL;
    s_dial_peak_lbl   = NULL;
    s_dial_bg_img     = NULL;
    s_dial_needle_img = NULL;
    memset(s_val_labels, 0, sizeof(s_val_labels));

    // Free tick canvas buffer before deleting LVGL objects (we hold the lock,
    // so the LVGL task is not rendering — safe to free while canvas still exists).
    if (s_tick_canvas_data) {
        heap_caps_free(s_tick_canvas_data);
        s_tick_canvas_data = NULL;
    }

    lv_obj_set_style_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clean(s_screen);

    app_config_t *cfg = config_manager_get();
    build_face(s_screen, &cfg->faces[s_active[s_face_idx]]);
    build_warn_overlay(s_screen);
}

void gauge_ui_mark_dirty(void)
{
    s_faces_dirty = true;
}

// ─── Update helpers ───────────────────────────────────────────────────────────

static void update_label(lv_obj_t *lbl, gauge_field_t fld, const gauge_data_t *d,
                         const app_config_t *cfg)
{
    if (!lbl) return;
    if (!d->valid) {
        // Only update if text actually changed — lv_label_set_text always
        // invalidates regardless of whether the content differs.
        if (strcmp(lv_label_get_text(lbl), "--") != 0) {
            lv_label_set_text(lbl, "--");
            lv_obj_set_style_text_color(lbl, lv_color_hex(C_DIM), 0);
        }
        return;
    }
    float v = field_value(fld, d);
    char buf[16];
    format_value(fld, v, buf, sizeof(buf), cfg);
    if (strcmp(lv_label_get_text(lbl), buf) != 0) {
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, field_color(fld, v, cfg), 0);
    }
}

static void update_dial_face(const gauge_data_t *d, const gauge_face_cfg_t *f,
                              const app_config_t *cfg)
{
    if (!s_dial_val_lbl) return;

    // ── Value ─────────────────────────────────────────────────────────────────
    float val = d->valid ? field_value(f->slots[0], d) : 0.0f;

    char vbuf[16];
    if (d->valid) {
        format_value(f->slots[0], val, vbuf, sizeof(vbuf), cfg);
    } else {
        snprintf(vbuf, sizeof(vbuf), "--");
    }
    if (strcmp(vbuf, s_dial_last_vbuf) != 0) {
        memcpy(s_dial_last_vbuf, vbuf, sizeof(s_dial_last_vbuf));
        lv_label_set_text(s_dial_val_lbl, vbuf);
    }

    float range = f->dial_max - f->dial_min;
    if (range < 1e-6f) range = 1.0f;
    float pct = (val - f->dial_min) / range;
    pct = pct < 0.0f ? 0.0f : (pct > 1.0f ? 1.0f : pct);

    // ── EMA smoothing — only for the visual position (needle + arc sweep) ────
    // Text labels, threshold detection, and peak hold all use the raw value so
    // they stay accurate; only the needle position is smoothed to hide CAN jitter.
    if (s_dial_smoothed_pct < 0.0f)
        s_dial_smoothed_pct = pct;   // first frame: snap immediately
    else
        s_dial_smoothed_pct = s_dial_smoothed_pct * DIAL_SMOOTH_ALPHA
                              + pct * (1.0f - DIAL_SMOOTH_ALPHA);
    float spct = s_dial_smoothed_pct;

    // ── Change detection — skip LVGL invalidation when nothing has moved ─────
    // arc_val is in [0, 1000]; s_dial_last_arc_val = -1 forces update on first frame.
    bool   above_thresh = d->valid && (val >= f->dial_warn_threshold);
    int32_t arc_val     = (int32_t)(spct * 1000.0f);
    bool pct_moved      = (arc_val != s_dial_last_arc_val);
    bool col_changed    = (above_thresh != s_dial_last_above);

    if (pct_moved || col_changed) {
        s_dial_last_arc_val = arc_val;
        s_dial_last_above   = above_thresh;

        // ── Indicator arc (only in standard mode — no arc with custom bg) ─────
        if (s_dial_arc_ind) {
            if (pct_moved)
                lv_arc_set_value(s_dial_arc_ind, arc_val);
            if (col_changed)
                lv_obj_set_style_arc_color(s_dial_arc_ind,
                    above_thresh ? COL_DANGER(cfg) : COL_NORMAL(cfg),
                    LV_PART_INDICATOR);
        }

        // ── Needle ────────────────────────────────────────────────────────────
        if (s_dial_needle_img) {
            // Custom needle image — only rotation changes with spct.
            int32_t lvgl_rotation = (int32_t)((spct * 270.0f + 45.0f) * 10.0f);
            lv_image_set_rotation(s_dial_needle_img, lvgl_rotation);
        } else if (s_dial_needle) {
            if (pct_moved) {
                dial_needle_points(spct, 195, 22, s_needle_pts);
                lv_line_set_points(s_dial_needle, s_needle_pts, 2);
            }
            if (col_changed)
                lv_obj_set_style_line_color(s_dial_needle,
                    above_thresh ? COL_DANGER(cfg) : lv_color_hex(0xFFFFFF), 0);
        }
    }

    // ── Peak hold logic ───────────────────────────────────────────────────────
    if (!d->valid) return;

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool above = (val >= f->dial_warn_threshold);

    if (above) {
        // Value is above threshold — update running peak
        if (isnan(s_dial_peak_val) || val > s_dial_peak_val) {
            s_dial_peak_val = val;
        }
        s_dial_above_thresh = true;
    } else {
        if (s_dial_above_thresh) {
            // Just dropped below threshold — start the hold countdown
            s_dial_peak_drop_ms = now_ms;
            s_dial_above_thresh = false;
        }
        // Check expiry (peak_hold_ms == 0 means hold until re-crossed)
        if (!isnan(s_dial_peak_val) && f->dial_peak_hold_ms > 0) {
            uint32_t elapsed = now_ms - s_dial_peak_drop_ms;
            if (elapsed >= f->dial_peak_hold_ms) {
                s_dial_peak_val = NAN;   // expire
            }
        }
    }

    // ── Peak marker & label ───────────────────────────────────────────────────
    // All LVGL calls are guarded by delta checks: arc angles and label text are
    // only updated when they actually change, and hide/show only when the
    // visibility state transitions.  This prevents redundant invalidations.
    if (!isnan(s_dial_peak_val) && s_dial_arc_peak && s_dial_peak_lbl) {
        float peak_pct = (s_dial_peak_val - f->dial_min) / range;
        peak_pct = peak_pct < 0.0f ? 0.0f : (peak_pct > 1.0f ? 1.0f : peak_pct);
        int pa = (int)(peak_pct * 270.0f);
        if (pa != s_dial_last_peak_pa) {
            s_dial_last_peak_pa = pa;
            int pa_start = pa - 2 < 0   ? 0   : pa - 2;
            int pa_end   = pa + 2 > 270 ? 270 : pa + 2;
            lv_arc_set_angles(s_dial_arc_peak, (uint16_t)pa_start, (uint16_t)pa_end);
            lv_obj_clear_flag(s_dial_arc_peak, LV_OBJ_FLAG_HIDDEN);

            char pbuf[24];
            format_value(f->slots[0], s_dial_peak_val, pbuf, sizeof(pbuf), cfg);
            char plbl[32];
            snprintf(plbl, sizeof(plbl), "PK %s", pbuf);
            if (strcmp(plbl, s_dial_last_pbuf) != 0) {
                memcpy(s_dial_last_pbuf, plbl, sizeof(s_dial_last_pbuf));
                lv_label_set_text(s_dial_peak_lbl, plbl);
            }
            lv_obj_clear_flag(s_dial_peak_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_dial_arc_peak && s_dial_peak_lbl) {
        if (s_dial_last_peak_pa != -1) {
            s_dial_last_peak_pa = -1;
            lv_obj_add_flag(s_dial_arc_peak, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_dial_peak_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ─── Warning overlay ──────────────────────────────────────────────────────────

// Creates the full-screen black overlay on top of the current face widgets.
// Must be called after every build_face() so the overlay is the topmost child.
static void build_warn_overlay(lv_obj_t *scr)
{
    app_config_t *cfg = config_manager_get();

    s_warn_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_warn_overlay, DISP_WIDTH, DISP_HEIGHT);
    lv_obj_set_pos(s_warn_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_warn_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_warn_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_warn_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_warn_overlay, 0, 0);
    lv_obj_set_style_radius(s_warn_overlay, 0, 0);
    lv_obj_add_flag(s_warn_overlay, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_warn_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // "! WARNING !" header
    s_warn_lbl_head = lv_label_create(s_warn_overlay);
    lv_label_set_text(s_warn_lbl_head, "! WARNING !");
    lv_obj_set_style_text_color(s_warn_lbl_head, COL_DANGER(cfg), 0);
    lv_obj_set_style_text_font(s_warn_lbl_head, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_warn_lbl_head, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_warn_lbl_head, DISP_WIDTH);
    lv_obj_set_pos(s_warn_lbl_head, 0, 148);

    // Warning name / user label
    s_warn_lbl_name = lv_label_create(s_warn_overlay);
    lv_label_set_text(s_warn_lbl_name, "");
    lv_obj_set_style_text_color(s_warn_lbl_name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_warn_lbl_name, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_warn_lbl_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_warn_lbl_name, DISP_WIDTH);
    lv_obj_set_pos(s_warn_lbl_name, 0, 208);

    // Current value in warn yellow
    s_warn_lbl_val = lv_label_create(s_warn_overlay);
    lv_label_set_text(s_warn_lbl_val, "");
    lv_obj_set_style_text_color(s_warn_lbl_val, COL_WARN(cfg), 0);
    lv_obj_set_style_text_font(s_warn_lbl_val, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_warn_lbl_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_warn_lbl_val, DISP_WIDTH);
    lv_obj_set_pos(s_warn_lbl_val, 0, 264);
}

// Evaluates all warning slots every frame and manages the overlay visibility.
// Called at the end of gauge_ui_update() after the face update.
static void evaluate_warnings(const gauge_data_t *data)
{
    if (!s_warn_overlay) return;

    app_config_t *cfg = config_manager_get();

    // Collect indices of all triggered high-priority warnings
    int  hi_idx[WARNINGS_MAX];
    int  hi_count = 0;
    bool any_lo   = false;

    for (int i = 0; i < WARNINGS_MAX; i++) {
        const warning_cfg_t *w = &cfg->warnings[i];
        if (!w->enabled || w->field == GAUGE_FIELD_NONE) continue;
        if (!data->valid) continue;

        float v = field_value(w->field, data);
        bool triggered = (w->lower_threshold != 0.0f && v < w->lower_threshold) ||
                         (w->upper_threshold != 0.0f && v > w->upper_threshold);
        if (!triggered) continue;

        if (w->high_priority) hi_idx[hi_count++] = i;
        else                  any_lo = true;
    }

    if (hi_count > 0) {
        // Show overlay; cycle through active warnings every 2 s
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (s_warn_active_idx < 0 || (now_ms - s_warn_cycle_ms) >= 2000) {
            // Find next slot in the active list
            int slot = 0;
            if (s_warn_active_idx >= 0) {
                for (int j = 0; j < hi_count; j++) {
                    if (hi_idx[j] == s_warn_active_idx) {
                        slot = (j + 1) % hi_count;
                        break;
                    }
                }
            }
            s_warn_active_idx = hi_idx[slot];
            s_warn_cycle_ms   = now_ms;

            const warning_cfg_t *w = &cfg->warnings[s_warn_active_idx];
            float v = field_value(w->field, data);

            const char *name = (w->label[0] != '\0') ? w->label : field_label(w->field);
            if (strcmp(lv_label_get_text(s_warn_lbl_name), name) != 0)
                lv_label_set_text(s_warn_lbl_name, name);

            char vbuf[24];
            format_value(w->field, v, vbuf, sizeof(vbuf), cfg);
            if (strcmp(lv_label_get_text(s_warn_lbl_val), vbuf) != 0)
                lv_label_set_text(s_warn_lbl_val, vbuf);
        } else if (s_warn_active_idx >= 0) {
            // Update the value text even if we're not cycling the slot
            const warning_cfg_t *w = &cfg->warnings[s_warn_active_idx];
            float v = field_value(w->field, data);
            char vbuf[24];
            format_value(w->field, v, vbuf, sizeof(vbuf), cfg);
            if (strcmp(lv_label_get_text(s_warn_lbl_val), vbuf) != 0)
                lv_label_set_text(s_warn_lbl_val, vbuf);
        }

        if (lv_obj_has_flag(s_warn_overlay, LV_OBJ_FLAG_HIDDEN))
            lv_obj_clear_flag(s_warn_overlay, LV_OBJ_FLAG_HIDDEN);

    } else {
        // No high-priority warnings
        if (!lv_obj_has_flag(s_warn_overlay, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_warn_overlay, LV_OBJ_FLAG_HIDDEN);
            s_warn_active_idx = -1;
        }

        // Low-priority: tint status label yellow; restore when clear
        if (any_lo != s_warn_lo_last) {
            s_warn_lo_last = any_lo;
            if (s_status_lbl) {
                lv_obj_set_style_text_color(s_status_lbl,
                    any_lo ? COL_WARN(cfg) : COL_NORMAL(cfg), 0);
            }
        }
    }
}

// ─── Main update ──────────────────────────────────────────────────────────────

void gauge_ui_update(const gauge_data_t *data)
{
    if (s_faces_dirty) {
        s_faces_dirty = false;
        rebuild_active_list();
        s_face_idx = 0;
        if (s_screen) {
            s_arc_obj         = NULL;
            s_status_lbl      = NULL;
            s_dial_arc_ind    = NULL;
            s_dial_arc_peak   = NULL;
            s_dial_needle     = NULL;
            s_dial_val_lbl    = NULL;
            s_dial_peak_lbl   = NULL;
            s_dial_bg_img     = NULL;
            s_dial_needle_img = NULL;
            memset(s_val_labels, 0, sizeof(s_val_labels));
            if (s_tick_canvas_data) {
                heap_caps_free(s_tick_canvas_data);
                s_tick_canvas_data = NULL;
            }
            lv_obj_clean(s_screen);
            app_config_t *cfg = config_manager_get();
            build_face(s_screen, &cfg->faces[s_active[0]]);
            build_warn_overlay(s_screen);
        }
        return;
    }

    app_config_t *cfg = config_manager_get();
    const gauge_face_cfg_t *f = &cfg->faces[s_active[s_face_idx]];

    switch (s_cur_layout) {

        case FACE_LAYOUT_RPM_ARC: {
            if (!s_arc_obj || !s_val_labels[0]) break;
            lv_color_t arc_col = field_color(GAUGE_FIELD_RPM, data->rpm, cfg);
            lv_obj_set_style_arc_color(s_arc_obj, arc_col, LV_PART_INDICATOR);
            lv_arc_set_value(s_arc_obj, (int32_t)data->rpm);
            if (data->valid) {
                lv_label_set_text_fmt(s_val_labels[0], "%d", (int)data->rpm);
                lv_obj_set_style_text_color(s_val_labels[0], arc_col, 0);
            } else {
                lv_label_set_text(s_val_labels[0], "---");
                lv_obj_set_style_text_color(s_val_labels[0], COL_TEXT(cfg), 0);
            }
            for (int i = 0; i < 2; i++) {
                update_label(s_val_labels[i + 1], f->slots[i], data, cfg);
            }
            break;
        }

        case FACE_LAYOUT_SINGLE:
            update_label(s_val_labels[0], f->slots[0], data, cfg);
            break;

        case FACE_LAYOUT_DUAL:
            update_label(s_val_labels[0], f->slots[0], data, cfg);
            update_label(s_val_labels[1], f->slots[1], data, cfg);
            break;

        case FACE_LAYOUT_QUAD:
            for (int i = 0; i < 4; i++) {
                update_label(s_val_labels[i], f->slots[i], data, cfg);
            }
            break;

        case FACE_LAYOUT_DIAL:
            update_dial_face(data, f, cfg);
            break;

        default: break;
    }

    evaluate_warnings(data);
}

void gauge_ui_set_status(const char *protocol_name, bool wifi_connected)
{
    if (!s_status_lbl) return;
    char tbuf[64];
    if (wifi_connected) {
        snprintf(tbuf, sizeof(tbuf), "%s \xE2\x97\x8F", protocol_name);
    } else {
        snprintf(tbuf, sizeof(tbuf), "%s", protocol_name);
    }
    if (strcmp(lv_label_get_text(s_status_lbl), tbuf) != 0) {
        lv_label_set_text(s_status_lbl, tbuf);
    }
}
