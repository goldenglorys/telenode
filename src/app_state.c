/*
 * Copyright (c) 2024 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_state, LOG_LEVEL_DBG);

#include "app_state.h"

/* TODO(replace-with-mqtt): this file was a Golioth LightDB State ("digital
 * twin") demo (example_int0/example_int1, observe "desired" + write
 * "state"). Per AUDIT.md: "Entirely a Golioth Cloud demo feature; no real
 * vehicle data flows through it" — no data-production logic to preserve, so
 * it's dropped rather than ported. Left as a no-op stub (see app_state.h)
 * rather than deleted, matching the rest of Phase 2's approach of leaving
 * TODO markers instead of removing files outright. */
int app_state_observe(void)
{
	return 0;
}
