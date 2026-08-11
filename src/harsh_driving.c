/*
 * SPDX-License-Identifier: Apache-2.0
 */

/* Deliberately zero Zephyr/driver includes — see harsh_driving.h. */
#include <math.h>
#include <stddef.h>

#include "harsh_driving.h"

enum harsh_driving_event harsh_driving_evaluate(float accel_x_mps2, float accel_y_mps2,
						 const struct harsh_driving_thresholds *thresholds)
{
	if (accel_x_mps2 >= thresholds->harsh_accel_mps2) {
		return HARSH_DRIVING_ACCELERATION;
	}
	if (accel_x_mps2 <= -thresholds->harsh_brake_mps2) {
		return HARSH_DRIVING_BRAKING;
	}
	if (fabsf(accel_y_mps2) >= thresholds->harsh_corner_mps2) {
		return HARSH_DRIVING_CORNERING;
	}
	return HARSH_DRIVING_NONE;
}

const char *harsh_driving_event_name(enum harsh_driving_event event)
{
	switch (event) {
	case HARSH_DRIVING_ACCELERATION:
		return "acceleration";
	case HARSH_DRIVING_BRAKING:
		return "braking";
	case HARSH_DRIVING_CORNERING:
		return "cornering";
	case HARSH_DRIVING_NONE:
	default:
		return "none";
	}
}
