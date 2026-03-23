#pragma once

#include "can_manager.h"
#include "protocol_decoder.h"

// OBD2 standard CAN IDs
#define OBD2_REQUEST_ID   0x7DF  // Functional broadcast to all ECUs
#define OBD2_RESPONSE_ID  0x7E8  // Engine ECU response

// Mode 01 (Current Data) PIDs
#define OBD2_PID_ENGINE_LOAD  0x04
#define OBD2_PID_COOLANT_TEMP 0x05
#define OBD2_PID_RPM          0x0C
#define OBD2_PID_SPEED        0x0D
#define OBD2_PID_IAT          0x0F
#define OBD2_PID_MAF          0x10
#define OBD2_PID_TPS          0x11
#define OBD2_PID_RUNTIME      0x1F
#define OBD2_PID_OIL_TEMP     0x5C

/**
 * @brief Decode an OBD2 response frame into gauge_data.
 *        Call from the CAN RX callback.
 */
void obd2_decode_response(const can_frame_t *frame, gauge_data_t *data);

/**
 * @brief FreeRTOS task: polls OBD2 PIDs in a round-robin.
 *        Started by protocol_decoder_init() when OBD2 is selected.
 */
void obd2_poll_task(void *arg);
