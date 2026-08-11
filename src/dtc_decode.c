/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "dtc_decode.h"

#define OBD2_SERVICE_READ_STORED_DTCS	    0x03
#define OBD2_SERVICE_READ_STORED_DTCS_RESP (OBD2_SERVICE_READ_STORED_DTCS + 0x40)

/* DTC first-byte top 2 bits select the category letter. */
static const char dtc_category_letters[4] = { 'P', 'C', 'B', 'U' };

void dtc_decode_build_request(uint8_t buf[8])
{
	/* [data_length, service, pad...] — no PID for Mode 03, matching the
	 * same ISO 15765-2 0xCC padding convention app_sensors.c's Mode 01
	 * requests already use. */
	buf[0] = 1;
	buf[1] = OBD2_SERVICE_READ_STORED_DTCS;
	for (int i = 2; i < 8; i++) {
		buf[i] = 0xCC;
	}
}

int dtc_decode_parse_response(const uint8_t *data, size_t len, struct dtc_list *out)
{
	if (len < 2 || data[1] != OBD2_SERVICE_READ_STORED_DTCS_RESP) {
		return -EINVAL;
	}

	out->count = 0;

	for (size_t i = 2; (i + 1) < len && out->count < DTC_MAX_CODES_PER_FRAME; i += 2) {
		uint8_t b0 = data[i];
		uint8_t b1 = data[i + 1];

		if (b0 == 0 && b1 == 0) {
			/* Unused DTC slot in this response. */
			continue;
		}

		char category = dtc_category_letters[(b0 >> 6) & 0x03];
		uint8_t first_digit = (b0 >> 4) & 0x03;
		uint8_t second_digit = b0 & 0x0F;
		uint8_t third_digit = (b1 >> 4) & 0x0F;
		uint8_t fourth_digit = b1 & 0x0F;

		snprintk(out->codes[out->count], DTC_CODE_STRLEN, "%c%1u%1X%1X%1X", category,
			 first_digit, second_digit, third_digit, fourth_digit);
		out->count++;
	}

	return 0;
}
