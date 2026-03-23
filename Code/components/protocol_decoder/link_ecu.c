#include <string.h>
#include "link_ecu.h"

// Read little-endian signed 16-bit word from CAN data at byte offset
static inline int16_t le_s16(const uint8_t *d, int ofs)
{
    return (int16_t)((uint16_t)d[ofs] | ((uint16_t)d[ofs + 1] << 8));
}

// Read little-endian unsigned 16-bit word from CAN data at byte offset
static inline uint16_t le_u16(const uint8_t *d, int ofs)
{
    return (uint16_t)d[ofs] | ((uint16_t)d[ofs + 1] << 8);
}

void link_ecu_decode(const can_frame_t *frame, uint32_t base_id, gauge_data_t *data)
{
    if (frame->dlc < 8) return;

    uint32_t id = frame->id;
    const uint8_t *d = frame->data;

    if (id == base_id + 0) {
        // ECT[s16,0.1°C], IAT[s16,0.1°C], TPS[u16,0.1%], RPM[u16,1rpm]
        data->coolant_temp_c  = le_s16(d, 0) * 0.1f;
        data->iat_c           = le_s16(d, 2) * 0.1f;
        data->tps_pct         = le_u16(d, 4) * 0.1f;
        data->rpm             = le_u16(d, 6);
        data->valid           = true;

    } else if (id == base_id + 1) {
        // MAP[u16,0.1kPa], Lambda1[u16,0.001λ], Lambda2[u16,0.001λ], Battery[u16,0.01V]
        data->map_kpa    = le_u16(d, 0) * 0.1f;
        data->lambda1    = le_u16(d, 2) * 0.001f;
        // lambda2 at d[4] — not currently exposed in gauge_data_t
        data->battery_v  = le_u16(d, 6) * 0.01f;

    } else if (id == base_id + 2) {
        // OilTemp[s16,0.1°C], OilPress[u16,0.1kPa], FuelPress[u16,0.1kPa], Gear[u16]
        data->oil_temp_c        = le_s16(d, 0) * 0.1f;
        data->oil_pressure_bar  = le_u16(d, 2) * 0.1f / 100.0f; // kPa → bar
        data->fuel_pressure_bar = le_u16(d, 4) * 0.1f / 100.0f;
        data->gear              = (int8_t)le_u16(d, 6);

    } else if (id == base_id + 3) {
        // Speed[u16,0.1kph], IgnAdv[s16,0.1°], FuelTrim[s16,0.1%], BoostTarget[u16,0.1kPa]
        data->vehicle_speed_kph = le_u16(d, 0) * 0.1f;
        // ign advance and boost target available but not in current gauge_data_t
    }
}
