/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * CA certificate for the self-hosted firmware update server (HTTPS,
 * server-authenticated only — no client cert, unlike uplink_mqtt.c's
 * mutual TLS). Provisioned under CONFIG_FOTA_UPDATE_SEC_TAG, a separate
 * sec_tag from CONFIG_UPLINK_MQTT_SEC_TAG — see AUDIT.md's Phase 6 section
 * for why they're kept separate.
 *
 * TODO: replace with your actual update server's CA certificate PEM
 * before flashing to hardware. If you serve signed images from a host
 * with a publicly-trusted CA (e.g. Let's Encrypt via a standard CDN/cloud
 * host), this would be that CA's root cert, not something you generate
 * yourself — only your MQTT broker's CA (uplink_mqtt_certs.h) is
 * necessarily self-signed/private.
 */

#ifndef __OTA_UPDATE_CERTS_H__
#define __OTA_UPDATE_CERTS_H__

static const char ota_update_ca_certificate[] =
	"-----BEGIN CERTIFICATE-----\n"
	"TODO: paste your firmware update server's CA certificate PEM here.\n"
	"-----END CERTIFICATE-----\n";

#endif /* __OTA_UPDATE_CERTS_H__ */
