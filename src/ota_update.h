/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * OTA firmware update, replacing Golioth's removed package/cohort/
 * deployment system. Two networks doing two different jobs (per the
 * original design): MQTT tells the device "a new version is available and
 * where" (small JSON pointer on trackers/{device_id}/ota — see
 * uplink_mqtt.h), then this module hands the URL to NCS's fota_download
 * library to actually pull the (larger) signed image over HTTP(S).
 *
 * fota_download already wraps the whole download -> flash-write ->
 * MCUboot-pending-swap pipeline internally (confirmed from source — see
 * AUDIT.md's Phase 6 section) — this module doesn't touch dfu_target or
 * MCUboot's image-manager APIs directly, and does NOT implement any new
 * signing/verification logic: MCUboot verifies the image signature before
 * marking it bootable exactly as it already does for a locally-flashed
 * image, using the same key `west build`'s existing signing step already
 * produces images for. See AUDIT.md for what still needs to be uploaded
 * to the update server (the exact signed artifact West already builds,
 * not a re-signed one).
 */

#ifndef __OTA_UPDATE_H__
#define __OTA_UPDATE_H__

/**
 * Provisions the FOTA CA certificate (CONFIG_FOTA_UPDATE_SEC_TAG — a
 * separate sec_tag from uplink_mqtt's, see AUDIT.md for why) and
 * registers this module as uplink_mqtt's OTA handler. Must be called
 * once, before LTE comes up — same modem_key_mgmt_write() constraint as
 * uplink_mqtt_init(). Does not start a download by itself.
 *
 * @retval 0 Success
 * @retval <0 errno code (credential provisioning or fota_download_init failure)
 */
int ota_update_init(void);

#endif /* __OTA_UPDATE_H__ */
