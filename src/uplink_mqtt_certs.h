/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * TLS credentials for the self-hosted MQTT broker (mutual TLS).
 *
 * TODO: replace all three placeholders below with your actual PEM-encoded
 * CA certificate, device client certificate, and device private key before
 * flashing to hardware — as written, uplink_mqtt_init()'s
 * modem_key_mgmt_write() calls will succeed (the modem accepts any
 * well-formed PEM blob), but the TLS handshake against your real broker
 * will fail with an invalid/placeholder identity.
 *
 * Each device needs its own client certificate + private key issued by
 * whatever CA your self-hosted broker trusts; the CA certificate below is
 * shared across your fleet. Null-terminated PEM strings (including the
 * trailing newline before the closing quote) are what modem_key_mgmt_write()
 * expects — do not strip the "-----BEGIN/END-----" markers.
 */

#ifndef __UPLINK_MQTT_CERTS_H__
#define __UPLINK_MQTT_CERTS_H__

static const char uplink_mqtt_ca_certificate[] =
	"-----BEGIN CERTIFICATE-----\n"
	"TODO: paste your broker's CA certificate PEM here.\n"
	"-----END CERTIFICATE-----\n";

static const char uplink_mqtt_client_certificate[] =
	"-----BEGIN CERTIFICATE-----\n"
	"TODO: paste this device's client certificate PEM here.\n"
	"-----END CERTIFICATE-----\n";

static const char uplink_mqtt_private_key[] =
	"-----BEGIN PRIVATE KEY-----\n"
	"TODO: paste this device's private key PEM here.\n"
	"-----END PRIVATE KEY-----\n";

#endif /* __UPLINK_MQTT_CERTS_H__ */
