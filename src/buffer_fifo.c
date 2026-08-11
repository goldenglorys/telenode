/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(buffer_fifo, LOG_LEVEL_DBG);

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>

#include <pm_config.h>

#include "buffer_fifo.h"

/*
 * Two independent rings share one NVS mount, each with its own {head,tail}
 * metadata entry and its own range of slot ids — see buffer_fifo.h for why
 * telemetry and events are not one shared queue. NVS ids are a flat 16-bit
 * namespace, so ranges are just carved out back-to-back:
 *   telemetry: meta id 1,  slots 2..25   (TELEMETRY_CAPACITY = 24)
 *   events:    meta id 26, slots 27..74  (EVENTS_CAPACITY = 48)
 */
#define TELEMETRY_META_ID    1
#define TELEMETRY_SLOT_BASE  2
#define TELEMETRY_CAPACITY   24

#define EVENTS_META_ID	     (TELEMETRY_SLOT_BASE + TELEMETRY_CAPACITY)
#define EVENTS_SLOT_BASE     (EVENTS_META_ID + 1)
#define EVENTS_CAPACITY	     48

struct fifo_meta {
	uint32_t head; /* next write cursor */
	uint32_t tail; /* next read (unacked) cursor */
};

struct channel_config {
	uint16_t meta_id;
	uint16_t slot_base;
	uint32_t capacity;
	bool drop_oldest; /* false => reject new pushes instead of evicting */
	const char *name;
};

static const struct channel_config channel_configs[] = {
	[BUFFER_FIFO_CHANNEL_TELEMETRY] = { .meta_id = TELEMETRY_META_ID,
					     .slot_base = TELEMETRY_SLOT_BASE,
					     .capacity = TELEMETRY_CAPACITY,
					     .drop_oldest = true,
					     .name = "telemetry" },
	[BUFFER_FIFO_CHANNEL_EVENTS] = { .meta_id = EVENTS_META_ID,
					  .slot_base = EVENTS_SLOT_BASE,
					  .capacity = EVENTS_CAPACITY,
					  .drop_oldest = false,
					  .name = "events" },
};

static struct nvs_fs fs;
static struct fifo_meta channel_meta[ARRAY_SIZE(channel_configs)];
static bool mounted;

K_MUTEX_DEFINE(fifo_lock);

static int meta_persist(enum buffer_fifo_channel channel)
{
	const struct channel_config *cfg = &channel_configs[channel];
	ssize_t rc = nvs_write(&fs, cfg->meta_id, &channel_meta[channel],
			       sizeof(channel_meta[channel]));

	if (rc < 0) {
		LOG_ERR("Failed to persist %s FIFO metadata: %d", cfg->name, (int)rc);
		return (int)rc;
	}
	return 0;
}

int buffer_fifo_init(void)
{
	const struct flash_area *fa;
	struct flash_pages_info page_info;
	int rc;

	rc = flash_area_open(PM_TELEMETRY_FIFO_ID, &fa);
	if (rc) {
		LOG_ERR("flash_area_open failed: %d", rc);
		return rc;
	}

	fs.flash_device = fa->fa_dev;
	fs.offset = fa->fa_off;

	if (!device_is_ready(fs.flash_device)) {
		LOG_ERR("Flash device for telemetry_fifo partition not ready");
		return -ENODEV;
	}

	rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &page_info);
	if (rc) {
		LOG_ERR("Failed to get flash page info: %d", rc);
		return rc;
	}

	fs.sector_size = page_info.size;
	fs.sector_count = fa->fa_size / page_info.size;

	rc = nvs_mount(&fs);
	if (rc) {
		LOG_ERR("nvs_mount failed: %d", rc);
		return rc;
	}

	for (size_t i = 0; i < ARRAY_SIZE(channel_configs); i++) {
		const struct channel_config *cfg = &channel_configs[i];

		rc = (int)nvs_read(&fs, cfg->meta_id, &channel_meta[i], sizeof(channel_meta[i]));
		if (rc != sizeof(channel_meta[i])) {
			/* No metadata entry yet (fresh flash) — start empty. */
			channel_meta[i].head = 0;
			channel_meta[i].tail = 0;
		}
		LOG_INF("%s FIFO mounted: %u queued record(s)", cfg->name,
			channel_meta[i].head - channel_meta[i].tail);
	}

	mounted = true;
	return 0;
}

int buffer_fifo_push(enum buffer_fifo_channel channel, const void *data, size_t len)
{
	const struct channel_config *cfg = &channel_configs[channel];
	struct fifo_meta *meta = &channel_meta[channel];
	int rc;

	if (!mounted) {
		return -EIO;
	}
	if (len == 0 || len > BUFFER_FIFO_MAX_RECORD_LEN) {
		return -EINVAL;
	}

	k_mutex_lock(&fifo_lock, K_FOREVER);

	if ((meta->head - meta->tail) >= cfg->capacity) {
		if (!cfg->drop_oldest) {
			LOG_ERR("%s FIFO full (%u queued); rejecting new record", cfg->name,
				cfg->capacity);
			k_mutex_unlock(&fifo_lock);
			return -ENOSPC;
		}
		/* Disposable channel: drop the oldest record to make room. */
		LOG_WRN("%s FIFO full, dropping oldest queued record", cfg->name);
		meta->tail++;
	}

	rc = (int)nvs_write(&fs, cfg->slot_base + (meta->head % cfg->capacity), data, len);
	if (rc < 0) {
		LOG_ERR("Failed to write %s FIFO slot: %d", cfg->name, rc);
		k_mutex_unlock(&fifo_lock);
		return rc;
	}

	meta->head++;
	rc = meta_persist(channel);

	k_mutex_unlock(&fifo_lock);
	return rc;
}

int buffer_fifo_peek(enum buffer_fifo_channel channel, void *buf, size_t buf_size,
		      size_t *out_len)
{
	const struct channel_config *cfg = &channel_configs[channel];
	struct fifo_meta *meta = &channel_meta[channel];
	ssize_t rc;

	if (!mounted) {
		return -EIO;
	}

	k_mutex_lock(&fifo_lock, K_FOREVER);

	if (meta->head == meta->tail) {
		k_mutex_unlock(&fifo_lock);
		return -ENOENT;
	}

	rc = nvs_read(&fs, cfg->slot_base + (meta->tail % cfg->capacity), buf, buf_size);

	k_mutex_unlock(&fifo_lock);

	if (rc < 0) {
		LOG_ERR("Failed to read %s FIFO slot: %d", cfg->name, (int)rc);
		return (int)rc;
	}
	if ((size_t)rc > buf_size) {
		/* Truncated: caller's buffer was smaller than the stored record. */
		return -ENOMEM;
	}

	*out_len = (size_t)rc;
	return 0;
}

int buffer_fifo_pop_ack(enum buffer_fifo_channel channel)
{
	struct fifo_meta *meta = &channel_meta[channel];
	int rc;

	if (!mounted) {
		return -EIO;
	}

	k_mutex_lock(&fifo_lock, K_FOREVER);

	if (meta->head == meta->tail) {
		k_mutex_unlock(&fifo_lock);
		return -ENOENT;
	}

	meta->tail++;
	rc = meta_persist(channel);

	k_mutex_unlock(&fifo_lock);
	return rc;
}

bool buffer_fifo_is_empty(enum buffer_fifo_channel channel)
{
	struct fifo_meta *meta = &channel_meta[channel];
	bool empty;

	k_mutex_lock(&fifo_lock, K_FOREVER);
	empty = (meta->head == meta->tail);
	k_mutex_unlock(&fifo_lock);

	return empty;
}

uint32_t buffer_fifo_count(enum buffer_fifo_channel channel)
{
	struct fifo_meta *meta = &channel_meta[channel];
	uint32_t count;

	k_mutex_lock(&fifo_lock, K_FOREVER);
	count = meta->head - meta->tail;
	k_mutex_unlock(&fifo_lock);

	return count;
}
