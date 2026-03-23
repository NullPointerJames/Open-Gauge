#include <string.h>
#include "haltech_ecu.h"

// ─── Big-endian helpers ───────────────────────────────────────────────────────

static inline uint16_t be_u16(const uint8_t *d, int ofs)
{
    return ((uint16_t)d[ofs] << 8) | (uint16_t)d[ofs + 1];
}

static inline int16_t be_s16(const uint8_t *d, int ofs)
{
    return (int16_t)(((uint16_t)d[ofs] << 8) | (uint16_t)d[ofs + 1]);
}

// Convert raw 0.1-Kelvin unit to Celsius.  Returns 0 if raw is 0 (no sensor).
static inline float k_to_c(uint16_t raw)
{
    if (raw == 0) return 0.0f;
    return (float)raw * 0.1f - 273.15f;
}

// ─── Decoder ─────────────────────────────────────────────────────────────────

void haltech_decode(const can_frame_t *frame, uint32_t base_id, gauge_data_t *data)
{
    if (frame->dlc < 8) return;

    const uint32_t id = frame->id;
    const uint8_t *d  = frame->data;

    if (id == base_id) {
        // 0x360 @ 50Hz — RPM, MAP, TPS
        data->rpm     = (float)be_u16(d, 0);              // 1 RPM / unit
        data->map_kpa = be_u16(d, 2) * 0.1f;              // 0.1 kPa / unit
        data->tps_pct = be_u16(d, 4) * 0.1f;              // 0.1 % / unit
        data->valid   = true;

    } else if (id == base_id + 1) {
        // 0x361 @ 50Hz — Fuel Pressure, Oil Pressure (0.1 kPa → bar)
        data->fuel_pressure_bar = be_u16(d, 0) * 0.001f;  // 0.1 kPa / 100 = bar
        data->oil_pressure_bar  = be_u16(d, 2) * 0.001f;

    } else if (id == base_id + 2) {
        // 0x362 @ 50Hz — Injector Duty, Ignition Angle
        data->inj_duty_primary_pct   = be_u16(d, 0) * 0.1f;   // 0.1 % / unit
        data->inj_duty_secondary_pct = be_u16(d, 2) * 0.1f;
        data->ignition_angle_leading  = be_s16(d, 4) * 0.1f;  // 0.1 ° / unit, signed
        data->ignition_angle_trailing = be_s16(d, 6) * 0.1f;

    } else if (id == base_id + 8) {
        // 0x368 @ 20Hz — Lambda 1 & 2
        data->lambda1 = be_u16(d, 0) * 0.001f;  // 0.001 λ / unit
        data->lambda2 = be_u16(d, 2) * 0.001f;

    } else if (id == base_id + 9) {
        // 0x369 @ 20Hz — Trigger / miss diagnostics
        data->miss_counter        = be_u16(d, 0);
        data->trigger_counter     = be_u16(d, 2);
        data->home_counter        = be_u16(d, 4);
        data->triggers_since_home = be_u16(d, 6);

    } else if (id == base_id + 16) {
        // 0x370 @ 20Hz — Wheel Speed, Gear, Intake Cam Angles
        data->vehicle_speed_kph = be_u16(d, 0) * 0.1f;   // 0.1 km/h / unit
        data->gear              = (int8_t)(be_u16(d, 2) & 0xFF);
        data->intake_cam_angle_1 = be_u16(d, 4) * 0.1f;  // 0.1 ° / unit
        data->intake_cam_angle_2 = be_u16(d, 6) * 0.1f;

    } else if (id == base_id + 18) {
        // 0x372 @ 10Hz — Battery, Target Boost, Barometric (bytes 2-3 unused)
        data->battery_v       = be_u16(d, 0) * 0.1f;      // 0.1 V / unit
        data->target_boost_kpa = be_u16(d, 4) * 0.1f;    // 0.1 kPa / unit
        data->baro_kpa        = be_u16(d, 6) * 0.1f;

    } else if (id == base_id + 19) {
        // 0x373 @ 10Hz — EGT 1-4 (0.1 K/unit → °C)
        for (int i = 0; i < 4; i++)
            data->egt_c[i] = k_to_c(be_u16(d, i * 2));

    } else if (id == base_id + 20) {
        // 0x374 @ 10Hz — EGT 5-8
        for (int i = 0; i < 4; i++)
            data->egt_c[4 + i] = k_to_c(be_u16(d, i * 2));

    } else if (id == base_id + 21) {
        // 0x375 @ 10Hz — EGT 9-12
        for (int i = 0; i < 4; i++)
            data->egt_c[8 + i] = k_to_c(be_u16(d, i * 2));

    } else if (id == base_id + 0x80) {
        // 0x3E0 @ 5Hz — Coolant, Air, Fuel, Oil Temps (0.1 K/unit → °C)
        data->coolant_temp_c = k_to_c(be_u16(d, 0));
        data->iat_c          = k_to_c(be_u16(d, 2));
        data->fuel_temp_c    = k_to_c(be_u16(d, 4));
        data->oil_temp_c     = k_to_c(be_u16(d, 6));

    } else if (id == base_id + 0x82) {
        // 0x3E2 @ 5Hz — Fuel Consumption, Avg Economy (bytes 0-1 unused)
        data->fuel_consumption_lhr = be_u16(d, 2) * 0.01f;  // 0.01 L/hr / unit
        data->avg_fuel_economy_l   = be_u16(d, 4) * 0.1f;   // 0.1 L / unit
    }
}
