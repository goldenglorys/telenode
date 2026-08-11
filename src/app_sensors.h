/*
 * Copyright (c) 2024 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __APP_SENSORS_H__
#define __APP_SENSORS_H__

/** The `app_sensors.c` file performs the important work of this application
 * which is to read CAN/GPS sensor values. TODO(replace-with-mqtt):
 * app_sensors_read_and_stream() builds the JSON payload but no longer
 * transmits it — see the splice-point TODO in app_sensors.c.
 */

void app_sensors_read_and_stream(void);
void app_sensors_init(void);

/**
 * @return The most recent authoritative (non-fake) UTC GPS timestamp,
 * "YYYY-MM-DDTHH:MM:SS.mmmZ", for event payloads not tied to any one GPS
 * fix (e.g. harsh-driving events). Sentinel "1970-01-01T00:00:00Z" until
 * the first valid fix arrives — there's no RTC hardware wired up here to
 * fall back to.
 */
const char *app_sensors_get_last_known_time(void);

#endif /* __APP_SENSORS_H__ */
