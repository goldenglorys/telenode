/*
 * Copyright (c) 2024 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Local config state (loop/GPS/vehicle-speed delays, fake-GPS controls),
 * read via the getters below. TODO(replace-with-mqtt): app_settings_register()
 * was Golioth Settings Service registration, letting the cloud change these
 * at runtime — see app_settings.c for what's still missing.
 */

#ifndef __APP_SETTINGS_H__
#define __APP_SETTINGS_H__

#include <stdint.h>

int32_t get_loop_delay_s(void);
void app_settings_register(void);
int32_t get_gps_delay_s(void);
bool get_fake_gps_enabled_s(void);
float get_fake_gps_latitude_s(void);
float get_fake_gps_longitude_s(void);
int32_t get_vehicle_speed_delay_s(void);

#endif /* __APP_SETTINGS_H__ */
