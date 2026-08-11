/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * ICM-42688-P sensor I/O: polls the IMU via Zephyr's generic Sensor API
 * and feeds readings into harsh_driving.c's pure threshold logic,
 * publishing a "harsh_driving_detected" event via uplink_mqtt whenever a
 * harsh event is classified. Deliberately kept separate from
 * harsh_driving.c's pure threshold math (see that header) so the math can
 * be unit-tested without this driver I/O involved.
 */

#ifndef __IMU_ICM42688_H__
#define __IMU_ICM42688_H__

/**
 * Starts the IMU polling thread. Logs an error and returns without
 * starting the thread if the sensor device isn't ready (e.g. not present
 * or not wired) — not fatal to the rest of the app.
 */
void imu_icm42688_init(void);

#endif /* __IMU_ICM42688_H__ */
