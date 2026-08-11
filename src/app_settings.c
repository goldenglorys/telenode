/*
 * Copyright (c) 2024 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_settings, LOG_LEVEL_DBG);

#include "main.h"
#include "app_settings.h"

/* How long to wait between uploading to Golioth */
static int32_t _loop_delay_s = 5;
#define LOOP_DELAY_S_MAX 43200
#define LOOP_DELAY_S_MIN 1

/* How long to wait between GPS readings */
static int32_t _gps_delay_s = 3;
#define GPS_DELAY_S_MAX 43200
#define GPS_DELAY_S_MIN 0

/* Fake GPS control */
static bool _fake_gps_enabled_s;

/* Fake GPS latitude value */
static float _fake_gps_latitude_s = 37.789980;
#define FAKE_GPS_LATITUDE_S_MAX 90.0
#define FAKE_GPS_LATITUDE_S_MIN -90.0

/* Fake GPS longitude value */
static float _fake_gps_longitude_s = -122.400860;
#define FAKE_GPS_LONGITUDE_S_MAX 180.0
#define FAKE_GPS_LONGITUDE_S_MIN -180.0

/* How long to wait between vehicle speed readings */
static int32_t _vehicle_speed_delay_s = 1;
#define VEHICLE_SPEED_DELAY_S_MAX 43200
#define VEHICLE_SPEED_DELAY_S_MIN 0

int32_t get_loop_delay_s(void)
{
	return _loop_delay_s;
}

int32_t get_gps_delay_s(void)
{
	return _gps_delay_s;
}

bool get_fake_gps_enabled_s(void)
{
	return _fake_gps_enabled_s;
}

float get_fake_gps_latitude_s(void)
{
	return _fake_gps_latitude_s;
}

float get_fake_gps_longitude_s(void)
{
	return _fake_gps_longitude_s;
}

int32_t get_vehicle_speed_delay_s(void)
{
	return _vehicle_speed_delay_s;
}

/* TODO(replace-with-mqtt): the six on_setting callbacks and their
 * golioth_settings_register calls are removed — each callback's return type
 * (enum golioth_settings_status) is the Golioth Settings Service API
 * contract itself, so there's no non-Golioth-typed body to keep. The
 * getters above (the actual config state fed into app_sensors.c/main.c) are
 * untouched and still return their compiled-in defaults; there is currently
 * no runtime way to change the loop delay, GPS delay, fake-GPS controls, or
 * vehicle-speed delay until an MQTT-based remote-config path replaces this
 * file's registration call. wake_system_thread() (previously called from
 * the loop-delay callback so a changed value took effect immediately) is
 * unreferenced here now for the same reason. */
void app_settings_register(void)
{
}
