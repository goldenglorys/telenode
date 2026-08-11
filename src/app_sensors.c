/*
 * Copyright (c) 2024 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_sensors, LOG_LEVEL_DBG);

#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/thread_stack.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/byteorder.h>

#include <app_version.h>
#include "app_sensors.h"
#include "app_settings.h"
#include "uplink_mqtt.h"
#include "obd_j1979.h"
#include "lib/minmea/minmea.h"

static const char *fw_version_str =
	STRINGIFY(APP_VERSION_MAJOR) "." STRINGIFY(APP_VERSION_MINOR) "." STRINGIFY(APP_PATCHLEVEL);

#ifdef CONFIG_ALUDEL_BATTERY_MONITOR
#include <battery_monitor.h>
#endif

#define NMEA_SIZE 128
#define UART_DEVICE_NODE DT_ALIAS(click_uart)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

#define UART_SEL DT_ALIAS(gnss7_sel)
static const struct gpio_dt_spec gnss7_sel = GPIO_DT_SPEC_GET(UART_SEL, gpios);

/* Vehicle data captured alongside each GPS fix — sourced from obd_j1979.c's
 * latest PID readings at the time of the fix, not read fresh from the CAN
 * bus here (that plumbing moved to obd_j1979.c; see its header for why). */
struct can_asset_tracker_data {
	struct minmea_sentence_rmc rmc_frame;
	struct obd_j1979_data obd;
	/* Latest GGA fix_quality (0=invalid,1=GPS,2=DGPS,...), captured from
	 * g_fix_quality below — RMC itself carries no fix-quality field, only
	 * GGA does, so that sentence type is parsed in process_reading() too. */
	int fix_quality;
};

/* Updated directly from process_reading() (UART ISR context) whenever a
 * GGA sentence parses; read into each RMC-triggered cat_frame record. No
 * mutex, matching this file's existing risk tolerance for small
 * ISR-shared scalars (see _last_gps in process_reading()). */
static int g_fix_quality = -1;

K_MSGQ_DEFINE(cat_msgq, sizeof(struct can_asset_tracker_data), 64, 4);
K_MSGQ_DEFINE(rmc_msgq, sizeof(struct minmea_sentence_rmc), 2, 4);

#define PROCESS_RMC_FRAMES_THREAD_STACK_SIZE 2048
#define PROCESS_RMC_FRAMES_THREAD_PRIORITY   2
static k_tid_t process_rmc_frames_tid;
struct k_thread process_rmc_frames_thread_data;
K_THREAD_STACK_DEFINE(process_rmc_frames_thread_stack, PROCESS_RMC_FRAMES_THREAD_STACK_SIZE);

/* clang-format off */
/* args: device_id, fw_version, timestamp, lat, lng, speed_kmh, heading_deg,
 * fix_quality, location.fake, obd.vehicle_speed_kmh, engine_rpm,
 * fuel_level_pct, coolant_temp_c, ignition_on, supply_voltage_v, buffered */
#define JSON_FMT \
"{" \
	"\"device_id\":\"%s\"," \
	"\"fw_version\":\"%s\"," \
	"\"timestamp\":\"%s\"," \
	"\"location\":" \
	"{" \
		"\"lat\":%s," \
		"\"lng\":%s," \
		"\"speed_kmh\":%.1f," \
		"\"heading_deg\":%.1f," \
		"\"fix_quality\":%d," \
		"\"fake\":%s" \
	"}," \
	"\"obd\":" \
	"{" \
		"\"vehicle_speed_kmh\":%.1f," \
		"\"engine_rpm\":%d," \
		"\"fuel_level_pct\":%.1f," \
		"\"coolant_temp_c\":%d" \
	"}," \
	"\"power\":" \
	"{" \
		"\"ignition_on\":%s," \
		"\"supply_voltage_v\":%.1f" \
	"}," \
	"\"buffered\":%s" \
"}"
/* Same as JSON_FMT but without "timestamp" — no real UTC time exists for a
 * synthetic fake-GPS fix. args: device_id, fw_version, lat, lng, speed_kmh,
 * heading_deg, fix_quality, location.fake, obd.*, ignition_on,
 * supply_voltage_v, buffered */
#define JSON_FMT_FAKE_GPS \
"{" \
	"\"device_id\":\"%s\"," \
	"\"fw_version\":\"%s\"," \
	"\"location\":" \
	"{" \
		"\"lat\":%s," \
		"\"lng\":%s," \
		"\"speed_kmh\":%.1f," \
		"\"heading_deg\":%.1f," \
		"\"fix_quality\":%d," \
		"\"fake\":%s" \
	"}," \
	"\"obd\":" \
	"{" \
		"\"vehicle_speed_kmh\":%.1f," \
		"\"engine_rpm\":%d," \
		"\"fuel_level_pct\":%.1f," \
		"\"coolant_temp_c\":%d" \
	"}," \
	"\"power\":" \
	"{" \
		"\"ignition_on\":%s," \
		"\"supply_voltage_v\":%.1f" \
	"}," \
	"\"buffered\":%s" \
"}"
/* {device_id}/{timestamp}/{code}/{buffered ? "true" : "false"} */
#define DTC_EVENT_JSON_FMT \
"{" \
	"\"device_id\":\"%s\"," \
	"\"timestamp\":\"%s\"," \
	"\"event_type\":\"dtc_detected\"," \
	"\"detail\":{\"code\":\"%s\",\"first_seen\":true}," \
	"\"buffered\":%s" \
"}"
/* clang-format on */

/* Most recent authoritative (non-fake) UTC timestamp from a GPS fix, used
 * for DTC events (which aren't tied to any particular GPS fix the way
 * telemetry records are). Sentinel value until the first valid fix arrives
 * — there's no RTC hardware wired up in this design to fall back to. */
static char last_known_utc_time[32] = "1970-01-01T00:00:00Z";

/* DTC codes already published as a "dtc_detected" event this session —
 * checked so a code is reported exactly once, not re-fired on every poll
 * it's still active, and not re-fired if it clears and later reappears. */
#define MAX_TRACKED_DTC_CODES 16
static char reported_dtc_codes[MAX_TRACKED_DTC_CODES][DTC_CODE_STRLEN];
static size_t reported_dtc_count;

static bool dtc_already_reported(const char *code)
{
	for (size_t i = 0; i < reported_dtc_count; i++) {
		if (strcmp(reported_dtc_codes[i], code) == 0) {
			return true;
		}
	}
	return false;
}

static void dtc_mark_reported(const char *code)
{
	if (reported_dtc_count >= MAX_TRACKED_DTC_CODES) {
		LOG_WRN("Reported-DTC tracking table full; new codes won't be deduped further");
		return;
	}
	strncpy(reported_dtc_codes[reported_dtc_count], code, DTC_CODE_STRLEN - 1);
	reported_dtc_codes[reported_dtc_count][DTC_CODE_STRLEN - 1] = '\0';
	reported_dtc_count++;
}

/**
 * Convert a floating point coordinate value to a minmea_float coordinate.
 *
 * NMEA latitude is represented as [+-]DDMM.MMMM... [-90.0, 90.0]
 * NMEA longitude is represented as [+-]DDDMM.MMMM... [-180.0, 180.0]
 *
 * For example, a float -123.456789 will be be converted to:
 *   degrees = -123
 *   minutes = -0.456789 * 60 = -27.40734
 *   NMEA = (degrees * 100) + minutes = -12327.40734
 *
 * minmea_float stores the .value as a int_least32_t with a .scale factor:
 *   .value = (int_least32_t)(NMEA * .scale)
 *
 * So, NMEA -12327.40734 will be converted to:
 *   .value = -1232740734
 *   .scale = 100000
 *
 * 100000 scaling factor provides 5 digits of precision (±2cm LSB at the
 * equator) for the minutes value.
 *
 * Note: 5 digits of precision is the max we can use. If we were to try to use
 * 6 digits of precision (a scaling factor of 1000000), NMEA -12327.40734 would
 * be converted to .value = -12327407340, which would overflow int32_t:
 *   INT32_MIN =  -2147483648
 *   .value    = -12327407340 <- overflow!
 */
static inline int coord_to_minmea(struct minmea_float *f, float coord)
{
	int32_t degrees = (int32_t)coord;
	float minutes = (coord - degrees) * 60;

	/* Convert degrees to NMEA [+-]DDDMM.MMMMM format */
	degrees *= 100;

	/**
	 * Use 100000 as the scaling factor so that we can store up to 5 decimal
	 * places of minutes in minmea_float.value
	 */
	degrees *= 100000;
	minutes *= 100000;

	/* Make sure we don't overflow int32_t */
	if (minutes < INT32_MIN || minutes > (float)(INT32_MAX - 1)) {
		return -ERANGE;
	}

	/* minmea_float uses int_least32_t internally */
	f->value = (int_least32_t)(degrees + minutes);
	f->scale = 100000;

	return 0;
}

void process_rmc_frames_thread(void *arg1, void *arg2, void *arg3)
{
	int err;
	struct minmea_sentence_rmc rmc_frame;
	struct can_asset_tracker_data cat_frame;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (k_msgq_get(&rmc_msgq, &rmc_frame, K_FOREVER) == 0) {
		cat_frame.rmc_frame = rmc_frame;
		cat_frame.fix_quality = g_fix_quality;

		/* Use the latest OBD PID readings alongside this GPS fix. */
		obd_j1979_get_latest(&cat_frame.obd);

		err = k_msgq_put(&cat_msgq, &cat_frame, K_NO_WAIT);
		if (err) {
			LOG_ERR("Unable to add cat_frame to cat_msgq: %d", err);
		}

		LOG_DBG("GPS Position%s: %f, %f", cat_frame.rmc_frame.valid ? "" : " (fake)",
			(double)minmea_tocoord(&rmc_frame.latitude),
			(double)minmea_tocoord(&rmc_frame.longitude));

	}
}

/* This is called from the UART irq callback to try to get out fast */
static void process_reading(char *raw_nmea)
{
	/* _last_gps timestamp records when the previous GPS value was stored */
	static uint64_t _last_gps;
	enum minmea_sentence_id sid;
	sid = minmea_sentence_id(raw_nmea, false);
	if (sid == MINMEA_SENTENCE_RMC) {
		struct minmea_sentence_rmc rmc_frame;
		bool success = minmea_parse_rmc(&rmc_frame, raw_nmea);
		if (success) {
			uint64_t wait_for = _last_gps;
			if (k_uptime_delta(&wait_for) >= ((uint64_t)get_gps_delay_s() * 1000)) {
				if (rmc_frame.valid == true) {
					/* if queue is full, message is silently dropped */
					k_msgq_put(&rmc_msgq, &rmc_frame, K_NO_WAIT);

					/*
					 * wait_for now contains the current timestamp. Store this
					 * for the next reading.
					 */
					_last_gps = wait_for;
				} else {
					if (get_fake_gps_enabled_s() == true) {
						/* use fake GPS coordinates from LightDB state */
						coord_to_minmea(&rmc_frame.latitude,
								get_fake_gps_latitude_s());
						coord_to_minmea(&rmc_frame.longitude,
								get_fake_gps_longitude_s());
						k_msgq_put(&rmc_msgq, &rmc_frame, K_NO_WAIT);

						/*
						 * wait_for now contains the current timestamp.
						 * Store this for the next reading.
						 */
						_last_gps = wait_for;
					}
				}
			} else {
				/* LOG_DBG("Ignoring reading due to gps_delay_s window"); */
			}
		}
	} else if (sid == MINMEA_SENTENCE_GGA) {
		struct minmea_sentence_gga gga_frame;

		if (minmea_parse_gga(&gga_frame, raw_nmea)) {
			g_fix_quality = gga_frame.fix_quality;
		}
	}
}

/* UART callback */
void serial_cb(const struct device *dev, void *user_data)
{
	uint8_t c;
	static char rx_buf[NMEA_SIZE];
	static int rx_buf_pos;

	if (!uart_irq_update(uart_dev)) {
		return;
	}

	while (uart_irq_rx_ready(uart_dev)) {

		uart_fifo_read(uart_dev, &c, 1);

		if ((c == '\n') && rx_buf_pos > 0) {
			/* terminate string */
			if (rx_buf_pos == (NMEA_SIZE - 1)) {
				rx_buf[rx_buf_pos] = '\0';
			} else {
				rx_buf[rx_buf_pos] = '\n';
				rx_buf[rx_buf_pos + 1] = '\0';
			}

			process_reading(rx_buf);
			/* reset the buffer (it was copied to the msgq) */
			rx_buf_pos = 0;
		} else if (rx_buf_pos < (sizeof(rx_buf) - 1)) {
			rx_buf[rx_buf_pos++] = c;
		}
		/* else: characters beyond buffer size are dropped */
	}
}

void app_sensors_init(void)
{
	int err;

	LOG_DBG("Initializing GNSS receiver");

	err = gpio_pin_configure_dt(&gnss7_sel, GPIO_OUTPUT_ACTIVE);
	if (err < 0) {
		LOG_ERR("Unable to configure GNSS SEL Pin: %d", err);
	}

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device %s not ready", uart_dev->name);
	}

	/* Configure UART interrupt and callback to receive data */
	uart_irq_callback_user_data_set(uart_dev, serial_cb, NULL);
	uart_irq_rx_enable(uart_dev);

	/* Spawn a thread to process RMC frames */
	process_rmc_frames_tid = k_thread_create(
		&process_rmc_frames_thread_data, process_rmc_frames_thread_stack,
		K_THREAD_STACK_SIZEOF(process_rmc_frames_thread_stack), process_rmc_frames_thread,
		NULL, NULL, NULL, PROCESS_RMC_FRAMES_THREAD_PRIORITY, 0, K_NO_WAIT);
	if (!process_rmc_frames_tid) {
		LOG_ERR("Error spawning RMC frame processing thread");
	}
}

/* This will be called by the main() loop */
/* Do all of your work here! */
void app_sensors_read_and_stream(void)
{
	struct can_asset_tracker_data cached_data;
	char json_buf[512];
	char ts_str[32];
	char lat_str[12];
	char lon_str[12];

	/* read_and_report_battery(client)'s read/format/log steps, called
	 * directly here since its own signature is golioth_client-typed (see
	 * AUDIT.md's golioth-battery-monitor function split); its final
	 * stream_battery_data() step is replaced by the uplink_mqtt publish
	 * below, using the same JSON shape golioth-battery-monitor's own
	 * stream_battery_data() built. */
	IF_ENABLED(CONFIG_ALUDEL_BATTERY_MONITOR, (
		struct battery_data batt_data;
		int batt_err = read_battery_data(&batt_data);

		if (batt_err) {
			LOG_ERR("Error reading battery data");
		} else {
			char batt_json_buf[40];

			log_battery_data();
			snprintk(batt_json_buf, sizeof(batt_json_buf),
				 "{\"batt_v\":%d.%03d,\"batt_pct\":%d.%02d}",
				 batt_data.battery_voltage_mv / 1000,
				 batt_data.battery_voltage_mv % 1000,
				 batt_data.battery_level_pptt / 100,
				 batt_data.battery_level_pptt % 100);
			uplink_mqtt_publish_telemetry(batt_json_buf, strlen(batt_json_buf));
		}
	));

	while (k_msgq_get(&cat_msgq, &cached_data, K_NO_WAIT) == 0) {
		double obd_speed = cached_data.obd.vehicle_speed_valid
					    ? (double)cached_data.obd.vehicle_speed_kph
					    : -1.0;
		int rpm = cached_data.obd.engine_rpm_valid ? cached_data.obd.engine_rpm : -1;
		double fuel_level = cached_data.obd.fuel_level_valid
					     ? (double)cached_data.obd.fuel_level_pct
					     : -1.0;
		int coolant_temp =
			cached_data.obd.coolant_temp_valid ? cached_data.obd.coolant_temp_c : -999;
		double supply_voltage = cached_data.obd.supply_voltage_valid
						 ? (double)cached_data.obd.supply_voltage_v
						 : -1.0;
		/* Standard commercial-OBD-dongle proxy: bus responded at all this
		 * cycle => ignition/accessory power is on. */
		const char *ignition_on = obd_j1979_bus_responded(&cached_data.obd) ? "true" : "false";
		/* Best-effort snapshot — see the same caveat on the DTC event's
		 * "buffered" field below; the actual live-vs-buffer decision is
		 * made inside uplink_mqtt_publish_telemetry(), after this string
		 * is already built. */
		const char *buffered = uplink_mqtt_is_connected() ? "false" : "true";
		/* GPS-derived speed/heading, distinct from (and a cross-check
		 * against) obd_speed above — minmea's speed is in knots. */
		double location_speed_kmh =
			(double)minmea_tofloat(&cached_data.rmc_frame.speed) * 1.852;
		double heading_deg = (double)minmea_tofloat(&cached_data.rmc_frame.course);

		snprintk(lat_str, sizeof(lat_str), "%f",
			 (double) minmea_tocoord(&cached_data.rmc_frame.latitude));
		snprintk(lon_str, sizeof(lon_str), "%f",
			 (double) minmea_tocoord(&cached_data.rmc_frame.longitude));
		snprintk(ts_str, sizeof(ts_str), "20%02d-%02d-%02dT%02d:%02d:%02d.%03dZ",
			 cached_data.rmc_frame.date.year, cached_data.rmc_frame.date.month,
			 cached_data.rmc_frame.date.day, cached_data.rmc_frame.time.hours,
			 cached_data.rmc_frame.time.minutes, cached_data.rmc_frame.time.seconds,
			 cached_data.rmc_frame.time.microseconds);

		if (cached_data.rmc_frame.valid == true) {
			snprintk(json_buf, sizeof(json_buf), JSON_FMT, uplink_mqtt_get_device_id(),
				 fw_version_str, ts_str, lat_str, lon_str, location_speed_kmh,
				 heading_deg, cached_data.fix_quality, "false", obd_speed, rpm,
				 fuel_level, coolant_temp, ignition_on, supply_voltage, buffered);

			/* Authoritative UTC time for DTC/harsh-driving events below,
			 * which aren't tied to any one GPS fix the way this
			 * telemetry record is. */
			strncpy(last_known_utc_time, ts_str, sizeof(last_known_utc_time) - 1);
			last_known_utc_time[sizeof(last_known_utc_time) - 1] = '\0';
		} else { /* Fake GPS data has no real UTC time, so no "timestamp" field */
			snprintk(json_buf, sizeof(json_buf), JSON_FMT_FAKE_GPS,
				 uplink_mqtt_get_device_id(), fw_version_str, lat_str, lon_str,
				 location_speed_kmh, heading_deg, cached_data.fix_quality, "true",
				 obd_speed, rpm, fuel_level, coolant_temp, ignition_on,
				 supply_voltage, buffered);
		}

		uplink_mqtt_publish_telemetry(json_buf, strlen(json_buf));
	}

	/* Fire exactly one "dtc_detected" event per newly-seen code — not one
	 * batched event for the whole current set — and never re-fire for a
	 * code already reported (this session), even if it clears and later
	 * reappears. */
	struct dtc_list dtcs;

	obd_j1979_get_latest_dtcs(&dtcs);
	for (size_t i = 0; i < dtcs.count; i++) {
		if (dtc_already_reported(dtcs.codes[i])) {
			continue;
		}

		char event_json_buf[160];
		/* Best-effort snapshot: uplink_mqtt_publish_event() itself decides
		 * live-publish vs. buffer based on this same connection state, but
		 * that decision happens after this payload is already built, so
		 * there's a small race window between this check and the actual
		 * publish (e.g. a disconnect right in between) that this field
		 * won't reflect. */
		bool connected = uplink_mqtt_is_connected();

		snprintk(event_json_buf, sizeof(event_json_buf), DTC_EVENT_JSON_FMT,
			 uplink_mqtt_get_device_id(), last_known_utc_time, dtcs.codes[i],
			 connected ? "false" : "true");

		uplink_mqtt_publish_event(event_json_buf, strlen(event_json_buf));
		dtc_mark_reported(dtcs.codes[i]);
	}
}

const char *app_sensors_get_last_known_time(void)
{
	return last_known_utc_time;
}
