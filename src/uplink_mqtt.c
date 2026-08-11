/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(uplink_mqtt, LOG_LEVEL_DBG);

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/tls_credentials.h>

#ifdef CONFIG_MODEM_KEY_MGMT
#include <modem/modem_key_mgmt.h>
#endif
#include <modem/modem_info.h>

#include "buffer_fifo.h"
#include "uplink_mqtt_certs.h"
#include "uplink_mqtt.h"

#define RX_BUF_SIZE  512
#define TX_BUF_SIZE  512
#define TOPIC_MAX_LEN	  64
#define DEVICE_ID_MAX_LEN 32

#define RECONNECT_BACKOFF_BASE_MS 1000
#define RECONNECT_BACKOFF_MAX_MS  60000
#define CONNACK_WAIT_TIMEOUT_MS	  5000
#define SOCKET_POLL_TIMEOUT_MS	  1000

#define UPLINK_THREAD_STACK_SIZE 4096
#define UPLINK_THREAD_PRIORITY	 5

static struct mqtt_client client;
static struct sockaddr_storage broker_addr;
static uint8_t rx_buf[RX_BUF_SIZE];
static uint8_t tx_buf[TX_BUF_SIZE];

static char device_id[DEVICE_ID_MAX_LEN];
static char topic_telemetry[TOPIC_MAX_LEN];
static char topic_events[TOPIC_MAX_LEN];
static char topic_status[TOPIC_MAX_LEN];
static char topic_ota[TOPIC_MAX_LEN];

static uplink_mqtt_ota_handler_t ota_handler;
/* Payload of an incoming PUBLISH is read into this buffer inside the MQTT
 * event callback (see mqtt_event_handler's MQTT_EVT_PUBLISH case) — small
 * and fixed since the OTA message is documented as "just a JSON pointer". */
#define OTA_PAYLOAD_MAX_LEN 256
static uint8_t ota_payload_buf[OTA_PAYLOAD_MAX_LEN];

static struct mqtt_topic will_topic;
static struct mqtt_utf8 will_message;
static const char will_payload[] = "{\"state\":\"offline\"}";

static sec_tag_t sec_tags[] = { CONFIG_UPLINK_MQTT_SEC_TAG };

static struct zsock_pollfd fds[1];
static volatile bool mqtt_connack_ok;
static volatile bool uplink_connected;
static uint16_t next_message_id = 1;

K_MUTEX_DEFINE(publish_lock);
K_SEM_DEFINE(network_up_sem, 0, 1);

static K_THREAD_STACK_DEFINE(uplink_thread_stack, UPLINK_THREAD_STACK_SIZE);
static struct k_thread uplink_thread_data;
static bool thread_started;

static int provision_credentials(void)
{
#if defined(CONFIG_MODEM_KEY_MGMT)
	int err;

	err = modem_key_mgmt_write(CONFIG_UPLINK_MQTT_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				    uplink_mqtt_ca_certificate, strlen(uplink_mqtt_ca_certificate));
	if (err) {
		LOG_ERR("Failed to provision CA certificate: %d", err);
		return err;
	}

	err = modem_key_mgmt_write(CONFIG_UPLINK_MQTT_SEC_TAG,
				    MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT,
				    uplink_mqtt_client_certificate,
				    strlen(uplink_mqtt_client_certificate));
	if (err) {
		LOG_ERR("Failed to provision client certificate: %d", err);
		return err;
	}

	err = modem_key_mgmt_write(CONFIG_UPLINK_MQTT_SEC_TAG,
				    MODEM_KEY_MGMT_CRED_TYPE_PRIVATE_CERT, uplink_mqtt_private_key,
				    strlen(uplink_mqtt_private_key));
	if (err) {
		LOG_ERR("Failed to provision private key: %d", err);
		return err;
	}

	return 0;
#else
	LOG_WRN("CONFIG_MODEM_KEY_MGMT disabled; skipping TLS credential provisioning");
	return 0;
#endif
}

static void build_device_id_and_topics(void)
{
	if (strlen(CONFIG_UPLINK_MQTT_DEVICE_ID) > 0) {
		strncpy(device_id, CONFIG_UPLINK_MQTT_DEVICE_ID, sizeof(device_id) - 1);
	} else {
		int err = modem_info_init();

		if (err) {
			LOG_WRN("modem_info_init returned %d (continuing)", err);
		}
		if (modem_info_string_get(MODEM_INFO_IMEI, device_id, sizeof(device_id)) < 0) {
			LOG_WRN("Failed to read IMEI for device id; using placeholder");
			strncpy(device_id, "unknown-device", sizeof(device_id) - 1);
		}
	}
	device_id[sizeof(device_id) - 1] = '\0';

	snprintk(topic_telemetry, sizeof(topic_telemetry), "trackers/%s/telemetry", device_id);
	snprintk(topic_events, sizeof(topic_events), "trackers/%s/events", device_id);
	snprintk(topic_status, sizeof(topic_status), "trackers/%s/status", device_id);
	snprintk(topic_ota, sizeof(topic_ota), "trackers/%s/ota", device_id);

	/* Retained "offline" LWT on the status topic. Pointed-to storage must
	 * outlive the connection, hence file-scope statics rather than locals. */
	will_topic.topic.utf8 = (const uint8_t *)topic_status;
	will_topic.topic.size = strlen(topic_status);
	will_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;

	will_message.utf8 = (const uint8_t *)will_payload;
	will_message.size = strlen(will_payload);
}

static void mqtt_event_handler(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
	ARG_UNUSED(c);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT CONNACK error: %d", evt->result);
			mqtt_connack_ok = false;
			break;
		}
		mqtt_connack_ok = true;
		break;

	case MQTT_EVT_DISCONNECT:
		LOG_INF("MQTT disconnected (result %d)", evt->result);
		uplink_connected = false;
		break;

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBACK error: %d", evt->result);
		}
		break;

	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *pub = &evt->param.publish;
		size_t payload_len = pub->message.payload.len;
		size_t topic_len = pub->message.topic.topic.size;
		int rc;

		if (payload_len >= sizeof(ota_payload_buf)) {
			LOG_ERR("OTA message too large (%zu bytes); discarding", payload_len);
			rc = mqtt_read_publish_payload_blocking(&client, ota_payload_buf,
								 sizeof(ota_payload_buf));
			ARG_UNUSED(rc);
			payload_len = 0;
		} else {
			rc = mqtt_read_publish_payload_blocking(&client, ota_payload_buf,
								 payload_len);
			if (rc != (int)payload_len) {
				LOG_ERR("Failed to read PUBLISH payload: %d", rc);
				payload_len = 0;
			}
		}

		if (topic_len == strlen(topic_ota) &&
		    memcmp(pub->message.topic.topic.utf8, topic_ota, topic_len) == 0) {
			if (payload_len > 0 && ota_handler) {
				ota_handler(ota_payload_buf, payload_len);
			}
		} else {
			LOG_WRN("Unexpected PUBLISH on an unsubscribed topic; ignoring");
		}

		if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param puback = { .message_id = pub->message_id };

			(void)mqtt_publish_qos1_ack(&client, &puback);
		}
		break;
	}

	case MQTT_EVT_PINGRESP:
		break;

	default:
		break;
	}
}

static void prepare_fds(void)
{
	if (client.transport.type == MQTT_TRANSPORT_SECURE) {
		fds[0].fd = client.transport.tls.sock;
	} else {
		fds[0].fd = client.transport.tcp.sock;
	}
	fds[0].events = ZSOCK_POLLIN;
}

static int poll_socket(int timeout_ms)
{
	prepare_fds();
	return zsock_poll(fds, 1, timeout_ms);
}

static int mqtt_broker_resolve(void)
{
	struct zsock_addrinfo *result;
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	char port_str[8];
	struct sockaddr_in *broker4;
	int rc;

	/* Bare getaddrinfo()/struct addrinfo are only aliased when
	 * CONFIG_POSIX_API is set (see socket.h) — this project doesn't
	 * enable that, so use the always-available zsock_ names directly. */
	snprintk(port_str, sizeof(port_str), "%d", CONFIG_UPLINK_MQTT_BROKER_PORT);

	rc = zsock_getaddrinfo(CONFIG_UPLINK_MQTT_BROKER_HOSTNAME, port_str, &hints, &result);
	if (rc != 0) {
		LOG_ERR("Failed to resolve broker hostname '%s': %d",
			CONFIG_UPLINK_MQTT_BROKER_HOSTNAME, rc);
		return -EIO;
	}
	if (result == NULL) {
		return -ENOENT;
	}

	broker4 = (struct sockaddr_in *)&broker_addr;
	memset(broker4, 0, sizeof(*broker4));
	broker4->sin_family = AF_INET;
	broker4->sin_port = ((struct sockaddr_in *)result->ai_addr)->sin_port;
	broker4->sin_addr.s_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;

	zsock_freeaddrinfo(result);
	return 0;
}

static int mqtt_client_setup(void)
{
	int err;

	err = mqtt_broker_resolve();
	if (err) {
		return err;
	}

	mqtt_client_init(&client);

	client.broker = &broker_addr;
	client.evt_cb = mqtt_event_handler;
	client.client_id.utf8 = (const uint8_t *)device_id;
	client.client_id.size = strlen(device_id);
	client.password = NULL;
	client.user_name = NULL;
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);

	/* Must be (re)set before every mqtt_connect() — parameter changes
	 * after a connection is established have no effect (see mqtt.h). */
	client.will_topic = &will_topic;
	client.will_message = &will_message;
	client.will_retain = 1;

#if defined(CONFIG_MQTT_LIB_TLS)
	client.transport.type = MQTT_TRANSPORT_SECURE;
	client.transport.tls.config.peer_verify = TLS_PEER_VERIFY_REQUIRED;
	client.transport.tls.config.cipher_list = NULL;
	client.transport.tls.config.sec_tag_list = sec_tags;
	client.transport.tls.config.sec_tag_count = ARRAY_SIZE(sec_tags);
	/* TLS_HOSTNAME -> NRF_SO_SEC_HOSTNAME (see nrf9x_sockets.c) is passed
	 * straight through to the modem's own TLS stack alongside
	 * TLS_PEER_VERIFY_REQUIRED -> NRF_SO_SEC_PEER_VERIFY. The modem
	 * firmware itself (closed-source, not in this checked-out tree) is
	 * what actually performs chain + hostname/SAN verification against
	 * this value — confirmed only that both socket options reach the
	 * modem; the verification algorithm itself isn't something readable
	 * from source here. Nordic's documented behavior is that this
	 * combination performs full hostname verification, but confirm this
	 * against your modem firmware's release notes, and make sure this
	 * hostname exactly matches the CN/SAN on the broker's server
	 * certificate — a mismatch fails the handshake with no more detail
	 * than a generic TLS error over the cellular link.
	 */
	client.transport.tls.config.hostname = CONFIG_UPLINK_MQTT_BROKER_HOSTNAME;
#else
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif

	return 0;
}

static int try_connect_once(void)
{
	int64_t deadline;
	int err;

	err = mqtt_client_setup();
	if (err) {
		return err;
	}

	mqtt_connack_ok = false;

	err = mqtt_connect(&client);
	if (err) {
		LOG_ERR("mqtt_connect failed: %d", err);
		return err;
	}

	deadline = k_uptime_get() + CONNACK_WAIT_TIMEOUT_MS;

	while (k_uptime_get() < deadline) {
		int rc = poll_socket(100);

		if (rc > 0) {
			if (fds[0].revents & ZSOCK_POLLIN) {
				mqtt_input(&client);
				if (mqtt_connack_ok) {
					return 0;
				}
			}
			if (fds[0].revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
				break;
			}
		}
	}

	LOG_ERR("Timed out waiting for MQTT CONNACK");
	mqtt_abort(&client);
	return -ETIMEDOUT;
}

static int publish_raw(const char *topic, const void *payload, size_t len, enum mqtt_qos qos,
			bool retain)
{
	struct mqtt_publish_param param = { 0 };

	param.message.topic.topic.utf8 = (const uint8_t *)topic;
	param.message.topic.topic.size = strlen(topic);
	param.message.topic.qos = qos;
	param.message.payload.data = (uint8_t *)payload;
	param.message.payload.len = len;
	param.message_id = (qos == MQTT_QOS_0_AT_MOST_ONCE) ? 0 : next_message_id++;
	param.dup_flag = 0;
	param.retain_flag = retain ? 1 : 0;

	return mqtt_publish(&client, &param);
}

static int publish_status_locked(const void *payload, size_t len)
{
	if (!uplink_connected) {
		return -ENOTCONN;
	}
	return publish_raw(topic_status, payload, len, MQTT_QOS_1_AT_LEAST_ONCE, true);
}

/*
 * buffer_fifo's telemetry/events channels are already topic-specific (see
 * buffer_fifo.h), so buffered records store raw payload bytes only — no
 * envelope/tag needed, unlike a single shared queue would require.
 */
static int publish_or_buffer(enum buffer_fifo_channel channel, const char *topic,
			      enum mqtt_qos qos, const void *payload, size_t len)
{
	if (len > BUFFER_FIFO_MAX_RECORD_LEN) {
		LOG_ERR("Record too large to publish/buffer (%zu bytes)", len);
		return -EMSGSIZE;
	}

	k_mutex_lock(&publish_lock, K_FOREVER);
	if (uplink_connected) {
		int err = publish_raw(topic, payload, len, qos, false);

		k_mutex_unlock(&publish_lock);
		if (!err) {
			return 0;
		}
		LOG_WRN("Publish to '%s' failed (%d); buffering instead", topic, err);
	} else {
		k_mutex_unlock(&publish_lock);
	}

	return buffer_fifo_push(channel, payload, len);
}

static void drain_channel(enum buffer_fifo_channel channel, const char *topic, enum mqtt_qos qos)
{
	uint8_t buf[BUFFER_FIFO_MAX_RECORD_LEN];
	size_t out_len;

	while (buffer_fifo_peek(channel, buf, sizeof(buf), &out_len) == 0) {
		int err;

		k_mutex_lock(&publish_lock, K_FOREVER);
		err = publish_raw(topic, buf, out_len, qos, false);
		k_mutex_unlock(&publish_lock);

		if (err) {
			LOG_WRN("Failed to replay buffered '%s' record (%d); retrying next reconnect",
				topic, err);
			break;
		}

		buffer_fifo_pop_ack(channel);
	}
}

static void drain_buffered_records(void)
{
	/* Events first — lower volume, higher stakes (DTC/harsh-driving
	 * detections) than the disposable telemetry stream. */
	drain_channel(BUFFER_FIFO_CHANNEL_EVENTS, topic_events, MQTT_QOS_1_AT_LEAST_ONCE);
	drain_channel(BUFFER_FIFO_CHANNEL_TELEMETRY, topic_telemetry, MQTT_QOS_0_AT_MOST_ONCE);
}

static void subscribe_ota_topic(void)
{
	struct mqtt_topic sub_topic = {
		.topic = {
			.utf8 = (const uint8_t *)topic_ota,
			.size = strlen(topic_ota),
		},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
	};
	struct mqtt_subscription_list sub_list = {
		.list = &sub_topic,
		.list_count = 1,
		.message_id = next_message_id++,
	};
	int err = mqtt_subscribe(&client, &sub_list);

	if (err) {
		LOG_ERR("Failed to subscribe to '%s': %d", topic_ota, err);
	}
}

static void uplink_thread_fn(void *p1, void *p2, void *p3)
{
	uint32_t backoff_ms = RECONNECT_BACKOFF_BASE_MS;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sem_take(&network_up_sem, K_FOREVER);

	while (1) {
		int err = try_connect_once();

		if (err) {
			LOG_WRN("MQTT connect failed (%d); retrying in %u ms", err, backoff_ms);
			k_msleep(backoff_ms);
			backoff_ms = MIN(backoff_ms * 2, RECONNECT_BACKOFF_MAX_MS);
			continue;
		}

		backoff_ms = RECONNECT_BACKOFF_BASE_MS;
		uplink_connected = true;
		LOG_INF("MQTT connected as '%s'", device_id);

		static const char online_payload[] = "{\"state\":\"online\"}";

		k_mutex_lock(&publish_lock, K_FOREVER);
		publish_status_locked(online_payload, strlen(online_payload));
		k_mutex_unlock(&publish_lock);

		subscribe_ota_topic();
		drain_buffered_records();

		while (uplink_connected) {
			int timeout = mqtt_keepalive_time_left(&client);

			if (timeout <= 0 || timeout > SOCKET_POLL_TIMEOUT_MS) {
				timeout = SOCKET_POLL_TIMEOUT_MS;
			}

			int rc = poll_socket(timeout);

			if (rc > 0) {
				if (fds[0].revents & ZSOCK_POLLIN) {
					if (mqtt_input(&client) != 0) {
						uplink_connected = false;
						break;
					}
				}
				if (fds[0].revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
					uplink_connected = false;
					break;
				}
			} else if (rc == 0) {
				if (mqtt_live(&client) != 0) {
					uplink_connected = false;
					break;
				}
			}
		}

		mqtt_abort(&client);
		LOG_WRN("MQTT disconnected; will reconnect");
	}
}

int uplink_mqtt_init(void)
{
	int err;

	err = buffer_fifo_init();
	if (err) {
		LOG_ERR("buffer_fifo_init failed: %d (offline buffering unavailable)", err);
		/* Not fatal — connectivity itself doesn't depend on it. */
	}

	build_device_id_and_topics();

	return provision_credentials();
}

void uplink_mqtt_start(void)
{
	if (thread_started) {
		return;
	}
	thread_started = true;

	k_thread_create(&uplink_thread_data, uplink_thread_stack,
			K_THREAD_STACK_SIZEOF(uplink_thread_stack), uplink_thread_fn, NULL, NULL,
			NULL, UPLINK_THREAD_PRIORITY, 0, K_NO_WAIT);

	k_sem_give(&network_up_sem);
}

int uplink_mqtt_publish_telemetry(const void *payload, size_t len)
{
	return publish_or_buffer(BUFFER_FIFO_CHANNEL_TELEMETRY, topic_telemetry,
				  MQTT_QOS_0_AT_MOST_ONCE, payload, len);
}

int uplink_mqtt_publish_event(const void *payload, size_t len)
{
	return publish_or_buffer(BUFFER_FIFO_CHANNEL_EVENTS, topic_events,
				  MQTT_QOS_1_AT_LEAST_ONCE, payload, len);
}

int uplink_mqtt_publish_status(const void *payload, size_t len)
{
	int err;

	k_mutex_lock(&publish_lock, K_FOREVER);
	err = publish_status_locked(payload, len);
	k_mutex_unlock(&publish_lock);

	return err;
}

bool uplink_mqtt_is_connected(void)
{
	return uplink_connected;
}

const char *uplink_mqtt_get_device_id(void)
{
	return device_id;
}

void uplink_mqtt_set_ota_handler(uplink_mqtt_ota_handler_t handler)
{
	ota_handler = handler;
}
