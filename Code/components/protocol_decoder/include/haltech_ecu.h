#pragma once

#include "can_manager.h"
#include "protocol_decoder.h"

/**
 * Haltech Elite / Nexus CAN broadcast stream decoder.
 * Protocol version 2.35.0 — big-endian, 11-bit IDs, 1 Mbit/s.
 *
 * Default base ID: 0x360 (configurable in NSP/Haltech software)
 *
 * Frame layout (all fields big-endian):
 *
 *  base+0x00 (0x360) @ 50Hz
 *      b0-1  RPM              1 RPM / unit
 *      b2-3  MAP              0.1 kPa / unit
 *      b4-5  TPS              0.1 % / unit
 *
 *  base+0x01 (0x361) @ 50Hz
 *      b0-1  Fuel Pressure    0.1 kPa / unit  (÷100 for bar)
 *      b2-3  Oil Pressure     0.1 kPa / unit  (÷100 for bar)
 *
 *  base+0x02 (0x362) @ 50Hz
 *      b0-1  Primary Injector Duty     0.1 % / unit
 *      b2-3  Secondary Injector Duty   0.1 % / unit
 *      b4-5  Ignition Angle Leading    0.1 ° / unit (s16)
 *      b6-7  Ignition Angle Trailing   0.1 ° / unit (s16)
 *
 *  base+0x08 (0x368) @ 20Hz
 *      b0-1  Lambda 1         0.001 λ / unit
 *      b2-3  Lambda 2         0.001 λ / unit
 *
 *  base+0x09 (0x369) @ 20Hz
 *      b0-1  Miss Counter
 *      b2-3  Trigger Counter
 *      b4-5  Home Counter
 *      b6-7  Triggers since last home
 *
 *  base+0x10 (0x370) @ 20Hz
 *      b0-1  Wheel Speed      0.1 km/h / unit
 *      b2-3  Gear
 *      b4-5  Intake Cam Angle 1  0.1 ° / unit
 *      b6-7  Intake Cam Angle 2  0.1 ° / unit
 *
 *  base+0x12 (0x372) @ 10Hz
 *      b0-1  Battery Voltage  0.1 V / unit
 *      b2-3  (unused)
 *      b4-5  Target Boost     0.1 kPa / unit
 *      b6-7  Barometric       0.1 kPa / unit
 *
 *  base+0x13 (0x373) @ 10Hz  — EGT 1-4  (0.1 K/unit, each b0-1, b2-3, b4-5, b6-7)
 *  base+0x14 (0x374) @ 10Hz  — EGT 5-8
 *  base+0x15 (0x375) @ 10Hz  — EGT 9-12
 *
 *  base+0x80 (0x3E0) @ 5Hz
 *      b0-1  Coolant Temp     0.1 K / unit  → °C = (raw × 0.1) − 273.15
 *      b2-3  Air (IAT) Temp   0.1 K / unit
 *      b4-5  Fuel Temp        0.1 K / unit
 *      b6-7  Oil Temp         0.1 K / unit
 *
 *  base+0x82 (0x3E2) @ 5Hz
 *      b0-1  (unused)
 *      b2-3  Fuel Consumption  0.01 L/hr / unit
 *      b4-5  Avg Fuel Economy  0.1 L / unit
 */
void haltech_decode(const can_frame_t *frame, uint32_t base_id, gauge_data_t *data);
