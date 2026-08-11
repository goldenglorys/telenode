/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ota_update, LOG_LEVEL_DBG);

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/data/json.h>
#include <zephyr/sys/reboot.h>
#include <net/fota_download.h>
#include <dfu/dfu_target.h>

#ifdef CONFIG_MODEM_KEY_MGMT
#include <modem/modem_key_mgmt.h>
#endif

#include "ota_update.h"
#include "ota_update_certs.h"
#include "uplink_mqtt.h"

#define MAX_URL_LEN  200
#define MAX_HOST_LEN 128

struct ota_pointer {
	const char *version;
	const char *url;
};

static const struct json_obj_descr ota_pointer_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct ota_pointer, version, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct ota_pointer, url, JSON_TOK_STRING),
};

/* Mutable copy of the "url" field, split in place by split_url(): host_buf
 * gets "host[:port]" copied out (NUL-terminated), file_out (returned by
 * split_url) points at the '/'-prefixed path still inside url_buf. */
static char url_buf[MAX_URL_LEN];
static char host_buf[MAX_HOST_LEN];

static int provision_credentials(void)
{
#if defined(CONFIG_MODEM_KEY_MGMT)
	int err = modem_key_mgmt_write(CONFIG_FOTA_UPDATE_SEC_TAG,
					MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, ota_update_ca_certificate,
					strlen(ota_update_ca_certificate));
	if (err) {
		LOG_ERR("Failed to provision FOTA CA certificate: %d", err);
	}
	return err;
#else
	LOG_WRN("CONFIG_MODEM_KEY_MGMT disabled; skipping FOTA CA provisioning");
	return 0;
#endif
}

static bool split_url(bool *use_tls, const char **file_out)
{
	char *rest;
	char *slash;

	if (strncmp(url_buf, "https://", 8) == 0) {
		*use_tls = true;
		rest = url_buf + 8;
	} else if (strncmp(url_buf, "http://", 7) == 0) {
		*use_tls = false;
		rest = url_buf + 7;
	} else {
		LOG_ERR("OTA url missing http(s):// scheme");
		return false;
	}

	slash = strchr(rest, '/');
	if (!slash) {
		LOG_ERR("OTA url has no path component");
		return false;
	}

	if ((size_t)(slash - rest) >= sizeof(host_buf)) {
		LOG_ERR("OTA url host too long");
		return false;
	}

	memcpy(host_buf, rest, slash - rest);
	host_buf[slash - rest] = '\0';

	*file_out = slash;
	return true;
}

static void fota_download_event_handler(const struct fota_download_evt *evt)
{
	switch (evt->id) {
	case FOTA_DOWNLOAD_EVT_PROGRESS:
		LOG_DBG("FOTA progress: %d%%", evt->progress);
		break;

	case FOTA_DOWNLOAD_EVT_FINISHED: {
		static const char status_payload[] = "{\"state\":\"ota_ready_reboot\"}";

		LOG_INF("FOTA download finished; image pending MCUboot swap-test. "
			"Rebooting to apply.");
		/* Best-effort — don't block the reboot on this succeeding. */
		uplink_mqtt_publish_status(status_payload, strlen(status_payload));
		k_msleep(500);
		sys_reboot(SYS_REBOOT_WARM);
		break;
	}

	case FOTA_DOWNLOAD_EVT_ERROR:
		LOG_ERR("FOTA download failed, cause: %d", evt->cause);
		break;

	case FOTA_DOWNLOAD_EVT_ERASE_PENDING:
	case FOTA_DOWNLOAD_EVT_ERASE_TIMEOUT:
	case FOTA_DOWNLOAD_EVT_ERASE_DONE:
	case FOTA_DOWNLOAD_EVT_CANCELLED:
	default:
		break;
	}
}

static void on_ota_message(const uint8_t *payload, size_t len)
{
	struct ota_pointer parsed = { 0 };
	char json_buf[256];
	const char *file;
	bool use_tls;
	int ret;
	int err;

	if (len >= sizeof(json_buf)) {
		LOG_ERR("OTA message too large (%zu bytes)", len);
		return;
	}
	memcpy(json_buf, payload, len);
	json_buf[len] = '\0';

	ret = json_obj_parse(json_buf, len, ota_pointer_descr, ARRAY_SIZE(ota_pointer_descr),
			      &parsed);
	if (ret < 0 || !(ret & BIT(0)) || !(ret & BIT(1))) {
		LOG_ERR("Malformed OTA message (missing version/url): %d", ret);
		return;
	}

	LOG_INF("OTA available: version=%s url=%s", parsed.version, parsed.url);

	if (strlen(parsed.url) >= sizeof(url_buf)) {
		LOG_ERR("OTA url too long");
		return;
	}
	strncpy(url_buf, parsed.url, sizeof(url_buf) - 1);
	url_buf[sizeof(url_buf) - 1] = '\0';

	if (!split_url(&use_tls, &file)) {
		return;
	}

	if (strlen(CONFIG_FOTA_UPDATE_SERVER_HOSTNAME) > 0) {
		if (strcmp(host_buf, CONFIG_FOTA_UPDATE_SERVER_HOSTNAME) != 0) {
			LOG_ERR("OTA url host '%s' != configured FOTA_UPDATE_SERVER_HOSTNAME "
				"'%s'; refusing",
				host_buf, CONFIG_FOTA_UPDATE_SERVER_HOSTNAME);
			return;
		}
	} else {
		LOG_WRN("FOTA_UPDATE_SERVER_HOSTNAME not configured; accepting OTA url host "
			"'%s' without validation",
			host_buf);
	}

	err = fota_download_start_with_image_type(host_buf, file,
						   use_tls ? CONFIG_FOTA_UPDATE_SEC_TAG : -1, 0, 0,
						   DFU_TARGET_IMAGE_TYPE_MCUBOOT);
	if (err) {
		LOG_ERR("fota_download_start_with_image_type failed: %d", err);
	}
}

int ota_update_init(void)
{
	int err;

	err = provision_credentials();
	if (err) {
		return err;
	}

	err = fota_download_init(fota_download_event_handler);
	if (err) {
		LOG_ERR("fota_download_init failed: %d", err);
		return err;
	}

	uplink_mqtt_set_ota_handler(on_ota_message);
	return 0;
}
