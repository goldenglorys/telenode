/*
 * Copyright (c) 2022-2023 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(golioth_can_asset_tracker, LOG_LEVEL_DBG);

#include <app_version.h>
#include "app_rpc.h"
#include "app_settings.h"
#include "app_sensors.h"
#include "uplink_mqtt.h"
#include "obd_j1979.h"
#include "imu_icm42688.h"
#include "ota_update.h"
#include <zephyr/dfu/mcuboot.h>
/* TODO(replace-with-mqtt): app_state.c was a Golioth LightDB State ("digital
 * twin") demo with no real vehicle data flowing through it (see AUDIT.md) —
 * dropped rather than ported. */
#include <zephyr/kernel.h>

#ifdef CONFIG_SOC_SERIES_NRF91X
#include <modem/lte_lc.h>
#endif
#ifdef CONFIG_ALUDEL_BATTERY_MONITOR
#include <battery_monitor.h>
#endif

#include <zephyr/drivers/gpio.h>

#ifdef CONFIG_MODEM_INFO
#include <modem/modem_info.h>
#endif

/* Current firmware version; update in VERSION */
static const char *_current_version =
	STRINGIFY(APP_VERSION_MAJOR) "." STRINGIFY(APP_VERSION_MINOR) "." STRINGIFY(APP_PATCHLEVEL);

static k_tid_t _system_thread = 0;

#if DT_NODE_EXISTS(DT_ALIAS(golioth_led))
static const struct gpio_dt_spec golioth_led = GPIO_DT_SPEC_GET(DT_ALIAS(golioth_led), gpios);
#endif /* DT_NODE_EXISTS(DT_ALIAS(golioth_led)) */
static const struct gpio_dt_spec user_btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static struct gpio_callback button_cb_data;

/* forward declarations */
void golioth_connection_led_set(uint8_t state);

void wake_system_thread(void)
{
	k_wakeup(_system_thread);
}

/* TODO(replace-with-mqtt): this whole Golioth client bootstrap (client
 * create, connect-event callback, fw_update init, and the four service
 * registration calls it fanned out to) is replaced by uplink_mqtt's
 * connect/reconnect handling in Phase 3. Nothing here fed sensor data in —
 * it was purely the cloud transport bootstrap — so it's dropped rather than
 * stubbed. app_sensors_set_client()/app_settings_register()/app_rpc_register()
 * calls that used to happen here are removed at their own definitions. */

#ifdef CONFIG_SOC_SERIES_NRF91X

static void lte_handler(const struct lte_lc_evt *const evt)
{
	if (evt->type == LTE_LC_EVT_NW_REG_STATUS) {

		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		    (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {

			uplink_mqtt_start();
		}
	}
}

#endif /* CONFIG_SOC_SERIES_NRF91X */

#ifdef CONFIG_MODEM_INFO
static void log_modem_firmware_version(void)
{
	char sbuf[128];

	/* Initialize modem info */
	int err = modem_info_init();

	if (err) {
		LOG_ERR("Failed to initialize modem info: %d", err);
	}

	/* Log modem firmware version */
	modem_info_string_get(MODEM_INFO_FW_VERSION, sbuf, sizeof(sbuf));
	LOG_INF("Modem firmware version: %s", sbuf);
}
#endif

void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	LOG_DBG("Button pressed at %d", k_cycle_get_32());
	/* This function is an Interrupt Service Routine. Do not call functions that
	 * use other threads, or perform long-running operations here
	 */
	k_wakeup(_system_thread);
}

/* Set (unset) LED indicators for active Golioth connection */
void golioth_connection_led_set(uint8_t state)
{
	uint8_t pin_state = state ? 1 : 0;
	ARG_UNUSED(pin_state); /* silence warning if no LED present */

	/* Turn on Golioth logo LED once connected */
	IF_ENABLED(DT_NODE_EXISTS(DT_ALIAS(golioth_led)),
		(gpio_pin_set_dt(&golioth_led, pin_state);));
}

int main(void)
{
	int err;

	/* Confirm this image so MCUboot doesn't revert it at the next reset.
	 * Every fota_download-driven update is a "test" swap (dfu_target
	 * schedules BOOT_UPGRADE_TEST, never PERMANENT — see AUDIT.md's Phase
	 * 6 section) until this runs; safe/idempotent to call on every boot,
	 * including ones that didn't follow an update. Called unconditionally
	 * here, matching NCS's own http_update sample — this is a known,
	 * intentionally-flagged limitation, not a considered safety design:
	 * it confirms immediately, before any health check, so a genuinely
	 * broken new image gets permanently confirmed rather than caught and
	 * rolled back automatically. A stricter design would defer this call
	 * until after confirming the new image can actually bring up
	 * LTE/MQTT, letting MCUboot's own revert-on-next-reset behavior catch
	 * a bad build — not implemented here, see ISSUES.md. */
	boot_write_img_confirmed();

	/* Initialize sensors */
	app_sensors_init();
	obd_j1979_init();
	imu_icm42688_init();

	LOG_DBG("Started CAN Asset Tracker app");

	LOG_INF("Firmware version: %s", _current_version);
	IF_ENABLED(CONFIG_MODEM_INFO, (log_modem_firmware_version();));

	/* Get system thread id so loop delay change event can wake main */
	_system_thread = k_current_get();

#if DT_NODE_EXISTS(DT_ALIAS(golioth_led))
	/* Initialize Golioth logo LED */
	err = gpio_pin_configure_dt(&golioth_led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Unable to configure LED for Golioth Logo");
	}
#endif /* DT_NODE_EXISTS(DT_ALIAS(golioth_led)) */

#ifdef CONFIG_SOC_SERIES_NRF91X
	/* Provision TLS credentials and mount the offline-telemetry buffer
	 * before LTE comes up — modem_key_mgmt_write() (used by
	 * uplink_mqtt_init()) fails with -EPERM once the link is active.
	 * uplink_mqtt_start() (called from lte_handler() once LTE registers)
	 * only starts the connect/reconnect thread, no credential writes. */
	err = uplink_mqtt_init();
	if (err) {
		LOG_ERR("uplink_mqtt_init failed: %d", err);
	}

	err = ota_update_init();
	if (err) {
		LOG_ERR("ota_update_init failed: %d", err);
	}

	/* Start LTE asynchronously; uplink_mqtt_start() is triggered from
	 * lte_handler() once LTE registers. */
	LOG_INF("Connecting to LTE, this may take some time...");
	lte_lc_connect_async(lte_handler);

#else
	/* TODO(replace-with-mqtt): non-nRF91x path (WiFi/DHCP bring-up +
	 * blocking connect) depended on golioth-firmware-sdk's samples/common
	 * library (net_connect(), sample_credentials), which no longer exists
	 * in this workspace. Target hardware for this project is nRF9160-DK
	 * only (see AUDIT.md), so this branch is dead code for us; left as a
	 * stub rather than deleted in case a non-cellular board is added later. */
#endif /* CONFIG_SOC_SERIES_NRF91X */

	/* Set up user button */
	err = gpio_pin_configure_dt(&user_btn, GPIO_INPUT);
	if (err) {
		LOG_ERR("Error %d: failed to configure %s pin %d", err, user_btn.port->name,
			user_btn.pin);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&user_btn, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("Error %d: failed to configure interrupt on %s pin %d", err,
			user_btn.port->name, user_btn.pin);
		return err;
	}

	gpio_init_callback(&button_cb_data, button_pressed, BIT(user_btn.pin));
	gpio_add_callback(user_btn.port, &button_cb_data);

	while (true) {
		app_sensors_read_and_stream();

		k_sleep(K_SECONDS(get_loop_delay_s()));
	}
}
