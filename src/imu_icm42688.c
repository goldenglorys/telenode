/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(imu_icm42688, LOG_LEVEL_DBG);

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include "imu_icm42688.h"
#include "harsh_driving.h"
#include "uplink_mqtt.h"
#include "app_sensors.h"

#define IMU_POLL_INTERVAL_MS 100

#define IMU_THREAD_STACK_SIZE 2048
#define IMU_THREAD_PRIORITY   3

static const struct device *const imu_dev = DEVICE_DT_GET_ANY(invensense_icm42688);

static K_THREAD_STACK_DEFINE(imu_thread_stack, IMU_THREAD_STACK_SIZE);
static struct k_thread imu_thread_data;

static const struct harsh_driving_thresholds thresholds = HARSH_DRIVING_DEFAULT_THRESHOLDS;

/* Same device_id/timestamp/event_type/detail/buffered envelope as the
 * "dtc_detected" event in app_sensors.c, per the PRD schema's event
 * convention — "timestamp" isn't explicitly specified for harsh-driving
 * events, but reuses app_sensors_get_last_known_time() for consistency
 * with that convention rather than omitting it. */
/* clang-format off */
#define HARSH_DRIVING_EVENT_JSON_FMT \
"{" \
	"\"device_id\":\"%s\"," \
	"\"timestamp\":\"%s\"," \
	"\"event_type\":\"harsh_driving_detected\"," \
	"\"detail\":{\"type\":\"%s\",\"accel_x_mps2\":%.2f,\"accel_y_mps2\":%.2f}," \
	"\"buffered\":%s" \
"}"
/* clang-format on */

static void imu_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		struct sensor_value accel[3];
		float accel_x, accel_y;
		enum harsh_driving_event event;
		int err;

		err = sensor_sample_fetch(imu_dev);
		if (err) {
			LOG_ERR("sensor_sample_fetch failed: %d", err);
			k_msleep(IMU_POLL_INTERVAL_MS);
			continue;
		}

		err = sensor_channel_get(imu_dev, SENSOR_CHAN_ACCEL_XYZ, accel);
		if (err) {
			LOG_ERR("sensor_channel_get failed: %d", err);
			k_msleep(IMU_POLL_INTERVAL_MS);
			continue;
		}

		accel_x = sensor_value_to_float(&accel[0]);
		accel_y = sensor_value_to_float(&accel[1]);

		event = harsh_driving_evaluate(accel_x, accel_y, &thresholds);

		if (event != HARSH_DRIVING_NONE) {
			char event_json_buf[192];
			bool connected = uplink_mqtt_is_connected();

			LOG_WRN("Harsh driving event: %s (x=%.2f y=%.2f m/s^2)",
				harsh_driving_event_name(event), (double)accel_x,
				(double)accel_y);

			snprintk(event_json_buf, sizeof(event_json_buf),
				 HARSH_DRIVING_EVENT_JSON_FMT, uplink_mqtt_get_device_id(),
				 app_sensors_get_last_known_time(),
				 harsh_driving_event_name(event), (double)accel_x,
				 (double)accel_y, connected ? "false" : "true");

			uplink_mqtt_publish_event(event_json_buf, strlen(event_json_buf));
		}

		k_msleep(IMU_POLL_INTERVAL_MS);
	}
}

void imu_icm42688_init(void)
{
	if (!device_is_ready(imu_dev)) {
		LOG_ERR("ICM-42688-P device not ready; harsh-driving detection disabled");
		return;
	}

	k_thread_create(&imu_thread_data, imu_thread_stack, K_THREAD_STACK_SIZEOF(imu_thread_stack),
			imu_thread_fn, NULL, NULL, NULL, IMU_THREAD_PRIORITY, 0, K_NO_WAIT);
}
