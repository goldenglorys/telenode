/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * SAE J1979 Mode 03 (read stored DTCs) request/response codec.
 *
 * Read-only by design: this module only builds Mode 03 *request* frames and
 * parses Mode 03 *response* frames into human-readable codes (e.g.
 * "P0301"). It has no CAN device access of its own (obd_j1979.c owns the
 * bus and calls into this module for the encode/decode step) and
 * deliberately does not implement Mode 04 (clear DTCs) or any other
 * CAN-write/actuation capability — out of scope per project policy.
 *
 * Scope limit: this only parses a single-frame Mode 03 response (up to
 * DTC_MAX_CODES_PER_FRAME codes in one 8-byte CAN frame), not the
 * ISO 15765-2 multi-frame ("first frame" + "consecutive frame" + flow
 * control) reassembly a vehicle with more stored DTCs than fit in one frame
 * would require. Most vehicles with only a handful of active faults fit in
 * a single frame; a vehicle reporting more DTCs than that will have some
 * silently truncated here rather than reassembled.
 */

#ifndef __DTC_DECODE_H__
#define __DTC_DECODE_H__

#include <stddef.h>
#include <stdint.h>

/** "P0301" + null terminator. */
#define DTC_CODE_STRLEN 6

/** Max DTCs decodable from a single (non-multi-frame) Mode 03 response. */
#define DTC_MAX_CODES_PER_FRAME 3

struct dtc_list {
	char codes[DTC_MAX_CODES_PER_FRAME][DTC_CODE_STRLEN];
	size_t count;
};

/**
 * Build the 8-byte Mode 03 request payload (service 0x03, no PID, padded
 * per ISO 15765-2 convention) into buf.
 *
 * @param buf Destination, must be at least 8 bytes
 */
void dtc_decode_build_request(uint8_t buf[8]);

/**
 * Parse a Mode 03 response frame's data bytes into human-readable DTC
 * codes.
 *
 * @param data Raw CAN frame data bytes
 * @param len Number of valid bytes in data (from can_dlc_to_bytes())
 * @param out Populated with 0..DTC_MAX_CODES_PER_FRAME decoded codes
 *
 * @retval 0 Success (out->count may be 0 if no DTCs are stored)
 * @retval -EINVAL data/len isn't a valid Mode 03 response (wrong service
 *                 byte or too short)
 */
int dtc_decode_parse_response(const uint8_t *data, size_t len, struct dtc_list *out);

#endif /* __DTC_DECODE_H__ */
