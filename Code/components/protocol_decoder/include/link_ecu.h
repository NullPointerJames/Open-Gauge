#pragma once

#include "can_manager.h"
#include "protocol_decoder.h"

/**
 * Link ECU G4+/G4X default CAN stream decoder.
 *
 * Default base ID: 0x518 (configurable)
 *
 * Frame layout (all values little-endian, signed unless noted):
 *  ID+0  (0x518): ECT[s16,0.1°C], IAT[s16,0.1°C], TPS[u16,0.1%], RPM[u16,1rpm]
 *  ID+1  (0x519): MAP[u16,0.1kPa], Lambda1[u16,0.001λ], Lambda2[u16,0.001λ], Battery[u16,0.01V]
 *  ID+2  (0x51A): OilTemp[s16,0.1°C], OilPress[u16,0.1kPa], FuelPress[u16,0.1kPa], Gear[u16]
 *  ID+3  (0x51B): Speed[u16,0.1kph], IgnAdv[s16,0.1°], FuelTrim[s16,0.1%], BoostTarget[u16,0.1kPa]
 */
void link_ecu_decode(const can_frame_t *frame, uint32_t base_id, gauge_data_t *data);
