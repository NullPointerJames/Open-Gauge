#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "cJSON.h"
#include "config_manager.h"
#include "can_manager.h"
#include "protocol_decoder.h"
#include "http_server.h"
#include "gauge_ui.h"

static const char *TAG    = "http";
static httpd_handle_t s_server = NULL;

#define SPIFFS_BASE  "/spiffs"
#define FILE_BUF_SZ  4096

// ─── SPIFFS Mount ────────────────────────────────────────────────────────────

static void mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = SPIFFS_BASE,
        .partition_label        = "spiffs",
        .max_files              = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    } else {
        size_t total, used;
        esp_spiffs_info("spiffs", &total, &used);
        ESP_LOGI(TAG, "SPIFFS: %zu KB total, %zu KB used", total / 1024, used / 1024);
    }
}

// ─── File Server Helper ──────────────────────────────────────────────────────

static esp_err_t serve_file(httpd_req_t *req, const char *path,
                             const char *content_type)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", path);
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");

    char *buf = malloc(FILE_BUF_SZ);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }

    size_t n;
    while ((n = fread(buf, 1, FILE_BUF_SZ, f)) > 0) {
        httpd_resp_send_chunk(req, buf, (ssize_t)n);
    }
    httpd_resp_send_chunk(req, NULL, 0);

    free(buf);
    fclose(f);
    return ESP_OK;
}

// ─── Handlers ────────────────────────────────────────────────────────────────

static esp_err_t handle_index(httpd_req_t *req)
{
    return serve_file(req, SPIFFS_BASE "/index.html", "text/html");
}

static esp_err_t handle_css(httpd_req_t *req)
{
    return serve_file(req, SPIFFS_BASE "/style.css", "text/css");
}

static esp_err_t handle_js(httpd_req_t *req)
{
    return serve_file(req, SPIFFS_BASE "/app.js", "application/javascript");
}

// Helpers for colour serialisation (uint32_t 0xRRGGBB ↔ "RRGGBB" hex string)
static void color_to_hex(uint32_t c, char out[7])
{
    snprintf(out, 7, "%06X", (unsigned)c);
}
static uint32_t hex_to_color(const char *s)
{
    return (uint32_t)strtoul(s, NULL, 16);
}

// GET /api/config — returns current config as JSON
static esp_err_t handle_get_config(httpd_req_t *req)
{
    app_config_t *cfg = config_manager_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "protocol",       cfg->protocol);
    cJSON_AddNumberToObject(root, "can_speed",      cfg->can_speed_bps);
    cJSON_AddNumberToObject(root, "can_tx_pin",     cfg->can_tx_pin);
    cJSON_AddNumberToObject(root, "can_rx_pin",     cfg->can_rx_pin);
    cJSON_AddNumberToObject(root, "link_base_id",   cfg->link_base_id);
    cJSON_AddNumberToObject(root, "haltech_base_id",cfg->haltech_base_id);
    cJSON_AddStringToObject(root, "wifi_ssid",      cfg->wifi_ssid);
    cJSON_AddNumberToObject(root, "rpm_redline",    cfg->rpm_redline);
    cJSON_AddNumberToObject(root, "coolant_warn_c", cfg->coolant_warn_c);
    cJSON_AddNumberToObject(root, "oil_press_warn", cfg->oil_press_warn_bar);

    cJSON_AddBoolToObject(root, "display_temp_f", cfg->display_temp_f);
    cJSON_AddBoolToObject(root, "display_psi",    cfg->display_psi);
    cJSON_AddBoolToObject(root, "display_mph",    cfg->display_mph);
    cJSON_AddBoolToObject(root, "display_afr",    cfg->display_afr);

    char hbuf[7];
    cJSON *colors = cJSON_CreateObject();
    color_to_hex(cfg->colors.normal_rgb,  hbuf); cJSON_AddStringToObject(colors, "normal",  hbuf);
    color_to_hex(cfg->colors.warn_rgb,    hbuf); cJSON_AddStringToObject(colors, "warn",    hbuf);
    color_to_hex(cfg->colors.danger_rgb,  hbuf); cJSON_AddStringToObject(colors, "danger",  hbuf);
    color_to_hex(cfg->colors.text_rgb,    hbuf); cJSON_AddStringToObject(colors, "text",    hbuf);
    color_to_hex(cfg->colors.arc_bg_rgb,  hbuf); cJSON_AddStringToObject(colors, "arc_bg",  hbuf);
    cJSON_AddItemToObject(root, "colors", colors);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// POST /api/config — update config from JSON body, save, restart CAN
static esp_err_t handle_post_config(httpd_req_t *req)
{
    char body[512] = {0};
    int  recv = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    body[recv] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    app_config_t *cfg = config_manager_get();

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "protocol")) && cJSON_IsNumber(v))
        cfg->protocol = (can_protocol_t)v->valueint;
    if ((v = cJSON_GetObjectItem(root, "can_speed")) && cJSON_IsNumber(v))
        cfg->can_speed_bps = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "can_tx_pin")) && cJSON_IsNumber(v))
        cfg->can_tx_pin = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(root, "can_rx_pin")) && cJSON_IsNumber(v))
        cfg->can_rx_pin = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(root, "link_base_id")) && cJSON_IsNumber(v))
        cfg->link_base_id = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "haltech_base_id")) && cJSON_IsNumber(v))
        cfg->haltech_base_id = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "wifi_ssid")) && cJSON_IsString(v))
        strncpy(cfg->wifi_ssid, v->valuestring, sizeof(cfg->wifi_ssid) - 1);
    if ((v = cJSON_GetObjectItem(root, "wifi_password")) && cJSON_IsString(v)
            && strlen(v->valuestring) > 0)
        strncpy(cfg->wifi_password, v->valuestring, sizeof(cfg->wifi_password) - 1);
    if ((v = cJSON_GetObjectItem(root, "rpm_redline")) && cJSON_IsNumber(v))
        cfg->rpm_redline = (uint16_t)v->valueint;
    if ((v = cJSON_GetObjectItem(root, "coolant_warn_c")) && cJSON_IsNumber(v))
        cfg->coolant_warn_c = (float)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "oil_press_warn")) && cJSON_IsNumber(v))
        cfg->oil_press_warn_bar = (float)v->valuedouble;

    if ((v = cJSON_GetObjectItem(root, "display_temp_f")) && cJSON_IsBool(v))
        cfg->display_temp_f = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(root, "display_psi")) && cJSON_IsBool(v))
        cfg->display_psi = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(root, "display_mph")) && cJSON_IsBool(v))
        cfg->display_mph = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(root, "display_afr")) && cJSON_IsBool(v))
        cfg->display_afr = cJSON_IsTrue(v);

    cJSON *colors = cJSON_GetObjectItem(root, "colors");
    if (cJSON_IsObject(colors)) {
        cJSON *c;
        if ((c = cJSON_GetObjectItem(colors, "normal"))  && cJSON_IsString(c))
            cfg->colors.normal_rgb  = hex_to_color(c->valuestring);
        if ((c = cJSON_GetObjectItem(colors, "warn"))    && cJSON_IsString(c))
            cfg->colors.warn_rgb    = hex_to_color(c->valuestring);
        if ((c = cJSON_GetObjectItem(colors, "danger"))  && cJSON_IsString(c))
            cfg->colors.danger_rgb  = hex_to_color(c->valuestring);
        if ((c = cJSON_GetObjectItem(colors, "text"))    && cJSON_IsString(c))
            cfg->colors.text_rgb    = hex_to_color(c->valuestring);
        if ((c = cJSON_GetObjectItem(colors, "arc_bg"))  && cJSON_IsString(c))
            cfg->colors.arc_bg_rgb  = hex_to_color(c->valuestring);
    }

    cJSON_Delete(root);

    config_manager_save();

    // Rebuild gauge face so unit changes take effect on tick marks + labels
    gauge_ui_mark_dirty();

    // Restart CAN manager with new settings
    can_manager_deinit();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t can_err = can_manager_init(cfg->can_tx_pin, cfg->can_rx_pin, cfg->can_speed_bps);
    if (can_err != ESP_OK) {
        ESP_LOGE(TAG, "CAN restart failed: %s", esp_err_to_name(can_err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"msg\":\"can_restart_failed\"}");
        return ESP_FAIL;
    }
    protocol_decoder_set_protocol(cfg->protocol);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// GET /api/status — full live gauge data as JSON.
// Calling this endpoint signals "live mode": all Haltech frames are decoded
// for the next LIVE_MODE_TIMEOUT_MS ms (5 s), refreshed on every poll.
static esp_err_t handle_get_status(httpd_req_t *req)
{
    protocol_decoder_set_live_mode();

    gauge_data_t data;
    protocol_decoder_get_data(&data);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "valid",           data.valid);

    // Core
    cJSON_AddNumberToObject(root, "rpm",             data.rpm);
    cJSON_AddNumberToObject(root, "map_kpa",         data.map_kpa);
    cJSON_AddNumberToObject(root, "tps_pct",         data.tps_pct);
    // Pressures
    cJSON_AddNumberToObject(root, "fuel_press_bar",  data.fuel_pressure_bar);
    cJSON_AddNumberToObject(root, "oil_press_bar",   data.oil_pressure_bar);
    // Injection / ignition
    cJSON_AddNumberToObject(root, "inj_duty_pri",    data.inj_duty_primary_pct);
    cJSON_AddNumberToObject(root, "inj_duty_sec",    data.inj_duty_secondary_pct);
    cJSON_AddNumberToObject(root, "ign_angle",       data.ignition_angle_leading);
    // Wideband
    cJSON_AddNumberToObject(root, "lambda1",         data.lambda1);
    cJSON_AddNumberToObject(root, "lambda2",         data.lambda2);
    // Diagnostics
    cJSON_AddNumberToObject(root, "miss_counter",    data.miss_counter);
    cJSON_AddNumberToObject(root, "trigger_counter", data.trigger_counter);
    // Dynamics
    cJSON_AddNumberToObject(root, "speed_kph",       data.vehicle_speed_kph);
    cJSON_AddNumberToObject(root, "gear",            data.gear);
    cJSON_AddNumberToObject(root, "intake_cam1",     data.intake_cam_angle_1);
    cJSON_AddNumberToObject(root, "intake_cam2",     data.intake_cam_angle_2);
    // Electrical / boost
    cJSON_AddNumberToObject(root, "battery_v",       data.battery_v);
    cJSON_AddNumberToObject(root, "target_boost",    data.target_boost_kpa);
    cJSON_AddNumberToObject(root, "baro_kpa",        data.baro_kpa);
    // EGTs — 12-element array
    cJSON *egt = cJSON_CreateArray();
    for (int i = 0; i < 12; i++)
        cJSON_AddItemToArray(egt, cJSON_CreateNumber(data.egt_c[i]));
    cJSON_AddItemToObject(root, "egt", egt);
    // Temperatures
    cJSON_AddNumberToObject(root, "coolant_c",       data.coolant_temp_c);
    cJSON_AddNumberToObject(root, "iat_c",           data.iat_c);
    cJSON_AddNumberToObject(root, "fuel_temp_c",     data.fuel_temp_c);
    cJSON_AddNumberToObject(root, "oil_temp_c",      data.oil_temp_c);
    // Fuel economy
    cJSON_AddNumberToObject(root, "fuel_consump",    data.fuel_consumption_lhr);
    cJSON_AddNumberToObject(root, "fuel_economy",    data.avg_fuel_economy_l);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// GET /api/faces — returns face configuration as JSON
static esp_err_t handle_get_faces(httpd_req_t *req)
{
    app_config_t *cfg = config_manager_get();

    cJSON *root  = cJSON_CreateObject();
    cJSON *faces = cJSON_CreateArray();

    for (int i = 0; i < GAUGE_FACES_MAX; i++) {
        cJSON *face  = cJSON_CreateObject();
        cJSON *slots = cJSON_CreateArray();

        cJSON_AddNumberToObject(face, "enabled", cfg->faces[i].enabled);
        cJSON_AddNumberToObject(face, "layout",  cfg->faces[i].layout);

        for (int s = 0; s < FACE_MAX_SLOTS; s++) {
            cJSON_AddItemToArray(slots, cJSON_CreateNumber(cfg->faces[i].slots[s]));
        }
        cJSON_AddItemToObject(face, "slots", slots);

        // Dial-specific fields (always present so the web UI can read them)
        cJSON_AddNumberToObject(face, "dial_min",       cfg->faces[i].dial_min);
        cJSON_AddNumberToObject(face, "dial_max",       cfg->faces[i].dial_max);
        cJSON_AddNumberToObject(face, "dial_warn",      cfg->faces[i].dial_warn_threshold);
        cJSON_AddNumberToObject(face, "dial_peak_hold", cfg->faces[i].dial_peak_hold_ms);

        cJSON_AddItemToArray(faces, face);
    }
    cJSON_AddItemToObject(root, "faces", faces);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// POST /api/faces — update face configuration, save, signal gauge to reload
static esp_err_t handle_post_faces(httpd_req_t *req)
{
    char body[2048] = {0};
    int  recv = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    body[recv] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    cJSON *faces = cJSON_GetObjectItem(root, "faces");
    if (!cJSON_IsArray(faces)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing faces array");
        return ESP_OK;
    }

    app_config_t *cfg = config_manager_get();
    int n = cJSON_GetArraySize(faces);
    if (n > GAUGE_FACES_MAX) n = GAUGE_FACES_MAX;

    for (int i = 0; i < n; i++) {
        cJSON *face = cJSON_GetArrayItem(faces, i);
        if (!cJSON_IsObject(face)) continue;

        cJSON *v;
        if ((v = cJSON_GetObjectItem(face, "enabled")) && cJSON_IsNumber(v))
            cfg->faces[i].enabled = (uint8_t)(v->valueint ? 1 : 0);
        if ((v = cJSON_GetObjectItem(face, "layout")) && cJSON_IsNumber(v))
            cfg->faces[i].layout = (face_layout_t)(v->valueint < FACE_LAYOUT_COUNT
                                                   ? v->valueint : 0);

        if ((v = cJSON_GetObjectItem(face, "dial_min")) && cJSON_IsNumber(v))
            cfg->faces[i].dial_min = (float)v->valuedouble;
        if ((v = cJSON_GetObjectItem(face, "dial_max")) && cJSON_IsNumber(v))
            cfg->faces[i].dial_max = (float)v->valuedouble;
        if ((v = cJSON_GetObjectItem(face, "dial_warn")) && cJSON_IsNumber(v))
            cfg->faces[i].dial_warn_threshold = (float)v->valuedouble;
        if ((v = cJSON_GetObjectItem(face, "dial_peak_hold")) && cJSON_IsNumber(v))
            cfg->faces[i].dial_peak_hold_ms = (uint16_t)v->valueint;

        cJSON *slots = cJSON_GetObjectItem(face, "slots");
        if (cJSON_IsArray(slots)) {
            int ns = cJSON_GetArraySize(slots);
            if (ns > FACE_MAX_SLOTS) ns = FACE_MAX_SLOTS;
            for (int s = 0; s < ns; s++) {
                cJSON *sv = cJSON_GetArrayItem(slots, s);
                if (cJSON_IsNumber(sv)) {
                    int fld = sv->valueint;
                    cfg->faces[i].slots[s] = (gauge_field_t)(fld < GAUGE_FIELD_COUNT ? fld : 0);
                }
            }
        }
    }

    cJSON_Delete(root);
    config_manager_save();
    gauge_ui_mark_dirty();
    // Recompute which Haltech frames are needed for the new face config
    protocol_decoder_rebuild_haltech_mask();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// GET /api/warnings — returns all 8 warning slots as JSON
static esp_err_t handle_get_warnings(httpd_req_t *req)
{
    app_config_t *cfg = config_manager_get();

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();

    for (int i = 0; i < WARNINGS_MAX; i++) {
        const warning_cfg_t *w = &cfg->warnings[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "field",           w->field);
        cJSON_AddBoolToObject  (item, "enabled",         w->enabled);
        cJSON_AddNumberToObject(item, "lower_threshold", w->lower_threshold);
        cJSON_AddNumberToObject(item, "upper_threshold", w->upper_threshold);
        cJSON_AddBoolToObject  (item, "high_priority",   w->high_priority);
        cJSON_AddStringToObject(item, "label",           w->label);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "warnings", arr);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// POST /api/warnings — update all warning slots from JSON body, save
static esp_err_t handle_post_warnings(httpd_req_t *req)
{
    char body[1024] = {0};
    int  recv = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    body[recv] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "warnings");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing warnings array");
        return ESP_OK;
    }

    app_config_t *cfg = config_manager_get();
    int n = cJSON_GetArraySize(arr);
    if (n > WARNINGS_MAX) n = WARNINGS_MAX;

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(item)) continue;

        warning_cfg_t *w = &cfg->warnings[i];
        cJSON *v;
        if ((v = cJSON_GetObjectItem(item, "field")) && cJSON_IsNumber(v))
            w->field = (gauge_field_t)(v->valueint < GAUGE_FIELD_COUNT ? v->valueint : 0);
        if ((v = cJSON_GetObjectItem(item, "enabled")) && cJSON_IsBool(v))
            w->enabled = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(item, "lower_threshold")) && cJSON_IsNumber(v))
            w->lower_threshold = (float)v->valuedouble;
        if ((v = cJSON_GetObjectItem(item, "upper_threshold")) && cJSON_IsNumber(v))
            w->upper_threshold = (float)v->valuedouble;
        if ((v = cJSON_GetObjectItem(item, "high_priority")) && cJSON_IsBool(v))
            w->high_priority = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(item, "label")) && cJSON_IsString(v))
            strncpy(w->label, v->valuestring, sizeof(w->label) - 1);
    }

    cJSON_Delete(root);
    config_manager_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ─── Image Upload / Query Handlers ───────────────────────────────────────────

// Receive a POST body and stream it directly to a SPIFFS file.
// Returns HTTP 400 if the body exceeds 512 KB; 500 on write errors.
static esp_err_t handle_upload_img(httpd_req_t *req, const char *dest_path)
{
    if (req->content_len > 512 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large (max 512 KB)");
        return ESP_OK;
    }

    FILE *f = fopen(dest_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for write", dest_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write file");
        return ESP_OK;
    }

    char *buf = malloc(FILE_BUF_SZ);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }

    int remaining = (int)req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int to_recv  = remaining < FILE_BUF_SZ ? remaining : FILE_BUF_SZ;
        int received = httpd_req_recv(req, buf, to_recv);
        if (received <= 0) { ok = false; break; }
        if (fwrite(buf, 1, received, f) != (size_t)received) { ok = false; break; }
        remaining -= received;
    }
    free(buf);
    fclose(f);

    if (!ok) {
        unlink(dest_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Image saved: %s (%d bytes)", dest_path, (int)req->content_len);
    gauge_ui_mark_dirty();   // rebuild dial face to pick up the new image

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t handle_upload_bg(httpd_req_t *req)
{
    return handle_upload_img(req, SPIFFS_BASE "/custom_bg.png");
}

static esp_err_t handle_upload_needle(httpd_req_t *req)
{
    return handle_upload_img(req, SPIFFS_BASE "/custom_needle.png");
}

// GET /api/images — reports which custom images are currently stored
static esp_err_t handle_get_images(httpd_req_t *req)
{
    FILE *f;
    f = fopen(SPIFFS_BASE "/custom_bg.png",     "r");
    bool has_bg = (f != NULL); if (f) fclose(f);
    f = fopen(SPIFFS_BASE "/custom_needle.png", "r");
    bool has_nd = (f != NULL); if (f) fclose(f);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "has_background", has_bg);
    cJSON_AddBoolToObject(root, "has_needle",     has_nd);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// DELETE /api/images/background  or  /api/images/needle
static esp_err_t handle_delete_bg(httpd_req_t *req)
{
    unlink(SPIFFS_BASE "/custom_bg.png");
    gauge_ui_mark_dirty();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t handle_delete_needle(httpd_req_t *req)
{
    unlink(SPIFFS_BASE "/custom_needle.png");
    gauge_ui_mark_dirty();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// ─── Server Lifecycle ─────────────────────────────────────────────────────────

esp_err_t http_server_start(void)
{
    mount_spiffs();

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_uri_handlers = 18;
    config.stack_size       = 10240;  // extra headroom for file-upload streaming

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "Server start failed");

    // Static files
    static const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET, .handler = handle_index },
        { .uri = "/style.css",  .method = HTTP_GET, .handler = handle_css   },
        { .uri = "/app.js",     .method = HTTP_GET, .handler = handle_js    },
        // REST API
        { .uri = "/api/config",               .method = HTTP_GET,    .handler = handle_get_config    },
        { .uri = "/api/config",               .method = HTTP_POST,   .handler = handle_post_config   },
        { .uri = "/api/status",               .method = HTTP_GET,    .handler = handle_get_status    },
        { .uri = "/api/faces",                .method = HTTP_GET,    .handler = handle_get_faces     },
        { .uri = "/api/faces",                .method = HTTP_POST,   .handler = handle_post_faces    },
        // Image management
        { .uri = "/api/images",               .method = HTTP_GET,    .handler = handle_get_images    },
        { .uri = "/api/upload/background",    .method = HTTP_POST,   .handler = handle_upload_bg     },
        { .uri = "/api/upload/needle",        .method = HTTP_POST,   .handler = handle_upload_needle },
        { .uri = "/api/images/background",    .method = HTTP_DELETE, .handler = handle_delete_bg     },
        { .uri = "/api/images/needle",        .method = HTTP_DELETE, .handler = handle_delete_needle },
        // Warnings
        { .uri = "/api/warnings",             .method = HTTP_GET,    .handler = handle_get_warnings  },
        { .uri = "/api/warnings",             .method = HTTP_POST,   .handler = handle_post_warnings },
    };

    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}
