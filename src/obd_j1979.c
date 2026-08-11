/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(obd_j1979, LOG_LEVEL_DBG);

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>

#include "obd_j1979.h"
#include "dtc_decode.h"
#include "app_settings.h"

#define OBD2_PID_REQUEST_ID		     0x7DF
#define OBD2_PID_REQUEST_DATA_LENGTH	     2
#define OBD2_PID_RESPONSE_ID		     0x7E8
#define OBD2_SERVICE_SHOW_CURRENT_DATA	     0x01
#define OBD2_SERVICE_SHOW_CURRENT_DATA_RESP (OBD2_SERVICE_SHOW_CURRENT_DATA + 0x40)

#define PID_VEHICLE_SPEED	   0x0D
#define PID_ENGINE_RPM		   0x0C
#define PID_FUEL_LEVEL		   0x2F
#define PID_COOLANT_TEMP	   0x05
#define PID_CONTROL_MODULE_VOLTAGE 0x42

#define RESPONSE_WAIT_TIMEOUT_MS 500
#define REQUEST_SEND_TIMEOUT_MS	 100

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

CAN_MSGQ_DEFINE(obd_can_msgq, 4);

#define OBD_J1979_THREAD_STACK_SIZE 2048
#define OBD_J1979_THREAD_PRIORITY   2
static k_tid_t obd_j1979_tid;
static struct k_thread obd_j1979_thread_data;
K_THREAD_STACK_DEFINE(obd_j1979_thread_stack, OBD_J1979_THREAD_STACK_SIZE);

#define DATA_MUTEX_TIMEOUT 1000
K_MUTEX_DEFINE(data_mutex);
static struct obd_j1979_data latest_data;
static struct dtc_list latest_dtcs;

/*
 * Send a Mode 01 PID request and wait up to RESPONSE_WAIT_TIMEOUT_MS for a
 * matching response (same PID echoed back in data[2]), discarding any
 * non-matching frames in between (e.g. a straggler reply to a previous
 * request still in the queue).
 *
 * @retval 0 Success — out_data/out_len hold the response payload
 * @retval <0 No matching response arrived in time, or the send failed
 */
static int request_pid(uint8_t pid, uint8_t *out_data, size_t *out_len)
{
	struct can_frame request = {
		.flags = 0,
		.id = OBD2_PID_REQUEST_ID,
		.dlc = 8,
		.data = { OBD2_PID_REQUEST_DATA_LENGTH, OBD2_SERVICE_SHOW_CURRENT_DATA, pid, 0xCC,
			  0xCC, 0xCC, 0xCC, 0xCC },
	};
	struct can_frame response;
	int64_t deadline;
	int err;

	err = can_send(can_dev, &request, K_MSEC(REQUEST_SEND_TIMEOUT_MS), NULL, NULL);
	if (err) {
		LOG_ERR("Error sending PID 0x%02X request: %d", pid, err);
		return err;
	}

	deadline = k_uptime_get() + RESPONSE_WAIT_TIMEOUT_MS;

	while (1) {
		int64_t remaining = deadline - k_uptime_get();
		size_t len;

		if (remaining <= 0) {
			break;
		}
		if (k_msgq_get(&obd_can_msgq, &response, K_MSEC(remaining)) != 0) {
			break;
		}

		len = can_dlc_to_bytes(response.dlc);
		if (len < 3) {
			continue;
		}
		if ((response.data[1] == OBD2_SERVICE_SHOW_CURRENT_DATA_RESP) &&
		    (response.data[2] == pid)) {
			memcpy(out_data, response.data, len);
			*out_len = len;
			return 0;
		}
	}

	return -ETIMEDOUT;
}

static int request_dtcs(struct dtc_list *out)
{
	struct can_frame request = {
		.flags = 0,
		.id = OBD2_PID_REQUEST_ID,
		.dlc = 8,
	};
	struct can_frame response;
	int64_t deadline;
	int err;

	dtc_decode_build_request(request.data);

	err = can_send(can_dev, &request, K_MSEC(REQUEST_SEND_TIMEOUT_MS), NULL, NULL);
	if (err) {
		LOG_ERR("Error sending DTC request: %d", err);
		return err;
	}

	deadline = k_uptime_get() + RESPONSE_WAIT_TIMEOUT_MS;

	while (1) {
		int64_t remaining = deadline - k_uptime_get();
		size_t len;

		if (remaining <= 0) {
			break;
		}
		if (k_msgq_get(&obd_can_msgq, &response, K_MSEC(remaining)) != 0) {
			break;
		}

		len = can_dlc_to_bytes(response.dlc);
		if (dtc_decode_parse_response(response.data, len, out) == 0) {
			return 0;
		}
		/* Not a Mode 03 response (e.g. a straggler Mode 01 reply) — discard. */
	}

	return -ETIMEDOUT;
}

static void obd_j1979_thread_fn(void *arg1, void *arg2, void *arg3)
{
	uint8_t resp[8];
	size_t resp_len;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		struct obd_j1979_data data = { 0 };
		struct dtc_list dtcs = { 0 };

		if (request_pid(PID_VEHICLE_SPEED, resp, &resp_len) == 0 && resp_len >= 4) {
			data.vehicle_speed_kph = resp[3];
			data.vehicle_speed_valid = true;
		}

		if (request_pid(PID_ENGINE_RPM, resp, &resp_len) == 0 && resp_len >= 5) {
			data.engine_rpm = ((resp[3] * 256) + resp[4]) / 4;
			data.engine_rpm_valid = true;
		}

		if (request_pid(PID_FUEL_LEVEL, resp, &resp_len) == 0 && resp_len >= 4) {
			data.fuel_level_pct = (resp[3] * 100.0f) / 255.0f;
			data.fuel_level_valid = true;
		}

		if (request_pid(PID_COOLANT_TEMP, resp, &resp_len) == 0 && resp_len >= 4) {
			data.coolant_temp_c = (int)resp[3] - 40;
			data.coolant_temp_valid = true;
		}

		if (request_pid(PID_CONTROL_MODULE_VOLTAGE, resp, &resp_len) == 0 && resp_len >= 5) {
			data.supply_voltage_v = ((resp[3] * 256) + resp[4]) / 1000.0f;
			data.supply_voltage_valid = true;
		}

		if (request_dtcs(&dtcs) != 0) {
			dtcs.count = 0;
		}

		k_mutex_lock(&data_mutex, K_MSEC(DATA_MUTEX_TIMEOUT));
		latest_data = data;
		latest_dtcs = dtcs;
		k_mutex_unlock(&data_mutex);

		LOG_DBG("OBD: speed=%d rpm=%d fuel=%.1f%% coolant=%dC voltage=%.1fV dtcs=%u",
			data.vehicle_speed_valid ? data.vehicle_speed_kph : -1,
			data.engine_rpm_valid ? data.engine_rpm : -1,
			(double)(data.fuel_level_valid ? data.fuel_level_pct : -1.0f),
			data.coolant_temp_valid ? data.coolant_temp_c : -999,
			(double)(data.supply_voltage_valid ? data.supply_voltage_v : -1.0f),
			(unsigned int)dtcs.count);

		k_sleep(K_SECONDS(get_vehicle_speed_delay_s()));
	}
}

void obd_j1979_init(void)
{
	const struct can_filter filter = {
		.flags = 0U, .id = OBD2_PID_RESPONSE_ID, .mask = CAN_STD_ID_MASK
	};
	int filter_id;
	int err;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device %s not ready", can_dev->name);
		return;
	}

	err = can_start(can_dev);
	if (err == -EALREADY) {
		LOG_DBG("CAN controller already started");
	} else if (err != 0) {
		LOG_ERR("Error starting CAN controller: %d", err);
	}

	filter_id = can_add_rx_filter_msgq(can_dev, &obd_can_msgq, &filter);
	if (filter_id < 0) {
		LOG_ERR("Error adding CAN RX filter: %d", filter_id);
		return;
	}

	obd_j1979_tid = k_thread_create(&obd_j1979_thread_data, obd_j1979_thread_stack,
					K_THREAD_STACK_SIZEOF(obd_j1979_thread_stack),
					obd_j1979_thread_fn, NULL, NULL, NULL,
					OBD_J1979_THREAD_PRIORITY, 0, K_NO_WAIT);
	if (!obd_j1979_tid) {
		LOG_ERR("Error spawning OBD J1979 polling thread");
	}
}

void obd_j1979_get_latest(struct obd_j1979_data *out)
{
	k_mutex_lock(&data_mutex, K_FOREVER);
	*out = latest_data;
	k_mutex_unlock(&data_mutex);
}

void obd_j1979_get_latest_dtcs(struct dtc_list *out)
{
	k_mutex_lock(&data_mutex, K_FOREVER);
	*out = latest_dtcs;
	k_mutex_unlock(&data_mutex);
}
