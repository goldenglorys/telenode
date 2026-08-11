/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Flash-backed FIFO buffers for telemetry/event records produced while
 * uplink_mqtt reports disconnected. Backed by Zephyr NVS on the
 * `telemetry_fifo` Partition Manager partition (see pm_static.yml), so
 * queued records survive a reboot.
 *
 * Telemetry and events are two INDEPENDENT ring buffers (distinct NVS id
 * ranges within the same NVS mount), not one shared queue — a burst of
 * disposable QoS0 telemetry must never be able to evict an unacknowledged
 * QoS1 event before it's published, which a single shared drop-oldest ring
 * would allow. Their overflow policies differ accordingly:
 *   - BUFFER_FIFO_CHANNEL_TELEMETRY: drops the oldest record on overflow.
 *     Telemetry is disposable (recency matters more than completeness), so
 *     this keeps the buffer bounded without an unbounded backlog after a
 *     long outage.
 *   - BUFFER_FIFO_CHANNEL_EVENTS: rejects the new push on overflow (logs
 *     an error, returns -ENOSPC) rather than evicting a queued event.
 *     Sized generously (BUFFER_FIFO_EVENTS_CAPACITY) on the assumption
 *     that events are low-frequency (DTC/harsh-driving detections), so
 *     overflow should only happen after an unusually long outage — at
 *     which point rejecting-and-alerting is preferable to either silently
 *     dropping a fault code or blocking the calling thread indefinitely.
 *
 * Usage: push() while offline; on reconnect, repeatedly peek() + publish +
 * pop_ack() in order until empty or a publish fails (leaving the unacked
 * record in place to retry next time).
 */

#ifndef __BUFFER_FIFO_H__
#define __BUFFER_FIFO_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum buffer_fifo_channel {
	BUFFER_FIFO_CHANNEL_TELEMETRY = 0,
	BUFFER_FIFO_CHANNEL_EVENTS = 1,
};

/** Max size of a single buffered record, in bytes. */
#define BUFFER_FIFO_MAX_RECORD_LEN 256

/**
 * Mount the flash-backed FIFOs. Must be called once before any other
 * buffer_fifo_* call.
 *
 * @retval 0 Success
 * @retval <0 errno code on failure (e.g. from nvs_mount)
 */
int buffer_fifo_init(void);

/**
 * Push a record onto the tail of the given channel's FIFO for later replay.
 *
 * @param channel Which ring buffer (see overflow policy differences above)
 * @param data Record bytes (e.g. a JSON payload)
 * @param len Length of data; must be <= BUFFER_FIFO_MAX_RECORD_LEN
 *
 * @retval 0 Success
 * @retval -EINVAL len is 0 or exceeds BUFFER_FIFO_MAX_RECORD_LEN
 * @retval -ENOSPC BUFFER_FIFO_CHANNEL_EVENTS is full (record rejected, not
 *                 dropped — nothing already queued was evicted)
 * @retval <0 other errno code from the underlying NVS write
 */
int buffer_fifo_push(enum buffer_fifo_channel channel, const void *data, size_t len);

/**
 * Look at the oldest not-yet-acknowledged record on the given channel
 * without removing it.
 *
 * @param buf Destination buffer
 * @param buf_size Size of buf
 * @param out_len Set to the actual record length on success
 *
 * @retval 0 Success
 * @retval -ENOENT Channel is empty
 * @retval -ENOMEM buf_size is too small for the stored record
 * @retval <0 other errno code from the underlying NVS read
 */
int buffer_fifo_peek(enum buffer_fifo_channel channel, void *buf, size_t buf_size,
		      size_t *out_len);

/**
 * Acknowledge and remove the record last returned by buffer_fifo_peek() for
 * this channel. Call this only after that record has been published
 * successfully.
 *
 * @retval 0 Success
 * @retval -ENOENT Channel is already empty
 * @retval <0 other errno code from the underlying NVS write
 */
int buffer_fifo_pop_ack(enum buffer_fifo_channel channel);

/** @retval true if there are no unacknowledged records queued on this channel. */
bool buffer_fifo_is_empty(enum buffer_fifo_channel channel);

/** @return Number of unacknowledged records currently queued on this channel. */
uint32_t buffer_fifo_count(enum buffer_fifo_channel channel);

#endif /* __BUFFER_FIFO_H__ */
