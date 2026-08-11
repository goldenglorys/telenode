/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Pure threshold-math logic for harsh-driving detection — deliberately has
 * NO Zephyr/driver includes, so it can be unit-tested (e.g. via
 * `west twister --platform unit_testing`, or even a bare host compile)
 * without needing the IMU driver, native_sim, or the CAN bridge working
 * (see ISSUES.md — none of that loop is available on this host yet).
 * imu_icm42688.c owns the actual sensor I/O and calls into this.
 *
 * Axis convention assumed (fixed vehicle mounting orientation, no runtime
 * calibration): X = longitudinal (+ = forward acceleration), Y = lateral
 * (+/- = right/left). Z (vertical) isn't used for these three thresholds.
 * A real deployment would need to either enforce a known mounting
 * orientation or add a calibration step — out of scope here.
 */

#ifndef __HARSH_DRIVING_H__
#define __HARSH_DRIVING_H__

enum harsh_driving_event {
	HARSH_DRIVING_NONE = 0,
	HARSH_DRIVING_ACCELERATION,
	HARSH_DRIVING_BRAKING,
	HARSH_DRIVING_CORNERING,
};

struct harsh_driving_thresholds {
	float harsh_accel_mps2;  /* longitudinal, +X */
	float harsh_brake_mps2;  /* longitudinal, -X (magnitude) */
	float harsh_corner_mps2; /* lateral, |Y| (magnitude) */
};

/* Reasonable starting defaults, not derived from any specific vehicle or
 * regulatory spec — tune against real driving data once hardware/native_sim
 * verification is available (see ISSUES.md). */
#define HARSH_DRIVING_DEFAULT_THRESHOLDS                                                         \
	{                                                                                          \
		.harsh_accel_mps2 = 3.5f,                                                         \
		.harsh_brake_mps2 = 4.0f,                                                         \
		.harsh_corner_mps2 = 4.0f,                                                        \
	}

/**
 * Classify one accelerometer sample against the given thresholds.
 * Pure function: no I/O, no global state, safe to call from a host unit
 * test with no Zephyr present.
 *
 * @param accel_x_mps2 Longitudinal acceleration, m/s^2
 * @param accel_y_mps2 Lateral acceleration, m/s^2
 * @param thresholds Threshold configuration to evaluate against
 *
 * @return The single most significant event this sample crosses, or
 * HARSH_DRIVING_NONE. Longitudinal (accel/brake) takes priority over
 * lateral if both are exceeded simultaneously in one sample.
 */
enum harsh_driving_event harsh_driving_evaluate(float accel_x_mps2, float accel_y_mps2,
						 const struct harsh_driving_thresholds *thresholds);

/** @return A short label for an event ("acceleration"/"braking"/"cornering"/"none"), never NULL. */
const char *harsh_driving_event_name(enum harsh_driving_event event);

#endif /* __HARSH_DRIVING_H__ */
