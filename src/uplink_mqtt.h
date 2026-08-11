/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * MQTT/TLS uplink to a self-hosted broker, replacing the removed Golioth
 * Stream/RPC/Settings/LightDBState transport (see AUDIT.md). TLS uses
 * mutual auth via the nRF9160 modem's own credential storage
 * (modem_key_mgmt_write()) — see uplink_mqtt_certs.h for where to put your
 * real certificates.
 *
 * Topics (see Kconfig UPLINK_MQTT_DEVICE_ID for how {device_id} is chosen):
 *   trackers/{device_id}/telemetry — QoS 0, buffered+replayed on reconnect
 *   trackers/{device_id}/events    — QoS 1, buffered+replayed on reconnect
 *   trackers/{device_id}/status    — QoS 1, retained, also the connection's
 *                                    Last-Will-and-Testament ("offline")
 *   trackers/{device_id}/ota       — QoS 1, Cloud -> Device, subscribed;
 *                                    see ota_update.c for the handler
 */

#ifndef __UPLINK_MQTT_H__
#define __UPLINK_MQTT_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * Provision TLS credentials and build topic strings. Must be called
 * exactly once, and must complete before LTE comes up —
 * modem_key_mgmt_write() fails with -EPERM while the link is active.
 * Does not open a network connection.
 *
 * @retval 0 Success
 * @retval <0 errno code (credential provisioning or modem_info failure)
 */
int uplink_mqtt_init(void);

/**
 * Start the internal connect/reconnect/process thread. Call once the
 * network (LTE) is up. Safe to call more than once (subsequent calls are a
 * no-op).
 */
void uplink_mqtt_start(void);

/**
 * Publish to trackers/{device_id}/telemetry, QoS 0. If currently
 * disconnected, the payload is pushed onto the flash-backed buffer_fifo
 * instead and replayed in order once reconnected.
 *
 * @retval 0 Published (or successfully buffered)
 * @retval <0 errno code
 */
int uplink_mqtt_publish_telemetry(const void *payload, size_t len);

/**
 * Publish to trackers/{device_id}/events, QoS 1. Same buffering behavior
 * as uplink_mqtt_publish_telemetry() when disconnected.
 *
 * @retval 0 Published (or successfully buffered)
 * @retval <0 errno code
 */
int uplink_mqtt_publish_event(const void *payload, size_t len);

/**
 * Publish to trackers/{device_id}/status, QoS 1, retained. Not buffered —
 * only the most recent status matters, and one publishes automatically on
 * every successful (re)connect. Returns -ENOTCONN if currently
 * disconnected rather than buffering (a stale queued status would be
 * misleading once finally sent).
 *
 * @retval 0 Published
 * @retval -ENOTCONN Not currently connected
 * @retval <0 other errno code
 */
int uplink_mqtt_publish_status(const void *payload, size_t len);

/** @retval true if currently connected to the broker. */
bool uplink_mqtt_is_connected(void);

/**
 * @return This device's id (either CONFIG_UPLINK_MQTT_DEVICE_ID, or the
 * modem IMEI fallback) as built by uplink_mqtt_init(). Valid only after
 * uplink_mqtt_init() has been called.
 */
const char *uplink_mqtt_get_device_id(void);

/**
 * Called with the raw payload bytes of any message received on
 * trackers/{device_id}/ota. Not parsed here — ota_update.c owns the
 * version/url JSON schema.
 */
typedef void (*uplink_mqtt_ota_handler_t)(const uint8_t *payload, size_t len);

/**
 * Register the callback invoked for messages on trackers/{device_id}/ota.
 * Call before uplink_mqtt_start() (the subscribe happens on every connect,
 * including the first one). NULL to unregister.
 */
void uplink_mqtt_set_ota_handler(uplink_mqtt_ota_handler_t handler);

#endif /* __UPLINK_MQTT_H__ */
