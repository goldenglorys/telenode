/*
 * Copyright (c) 2024 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_rpc, LOG_LEVEL_DBG);

#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

#include "app_rpc.h"

/* Kept: plain Zephyr reboot mechanism, no Golioth types involved. Currently
 * unreachable since its only caller (the RPC "reboot" handler below) was
 * removed; wire it up again once an MQTT command topic exists. */
static void reboot_work_handler(struct k_work *work)
{
	for (int8_t i = 5; i >= 0; i--) {
		if (i) {
			LOG_INF("Rebooting in %d seconds...", i);
		}
		k_sleep(K_SECONDS(1));
	}

	/* Sync logs before reboot */
	LOG_PANIC();

	sys_reboot(SYS_REBOOT_COLD);
}
K_WORK_DEFINE(reboot_work, reboot_work_handler);

/* TODO(replace-with-mqtt): this file's three RPC handlers
 * (get_network_info/set_log_level/reboot) and their registration are removed
 * — their signatures (zcbor_state_t* params, enum golioth_rpc_status return)
 * are the Golioth RPC transport's own API contract, not generic code with a
 * thin Golioth wrapper, so there's no non-Golioth-typed shell to preserve
 * (see AUDIT.md's zephyr-network-info / golioth-battery-monitor split for
 * the same distinction applied to those two libraries). The reboot work
 * queue above and network_info_log() (in the zephyr-network-info module)
 * are the reusable pieces; re-wire them to an MQTT command topic when one
 * exists. */
void app_rpc_register(void)
{
}
