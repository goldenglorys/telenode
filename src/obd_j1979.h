/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * SAE J1979 Mode 01 (current data) PID request/response state machine,
 * built on top of the same CAN device/init/send/receive plumbing
 * app_sensors.c already established for the single-PID vehicle-speed poll
 * it used to do on its own — this module takes over and generalizes that
 * (own can_start(), own CAN RX filter/msgq, own polling thread) rather than
 * running a second, competing filter on the same response ID.
 *
 * Covers PIDs: vehicle speed (0x0D), engine RPM (0x0C), fuel level (0x2F),
 * coolant temperature (0x05), control module voltage (0x42). Also drives a
 * Mode 03 DTC poll each cycle via dtc_decode.c (read-only — see that
 * module's header for the read-only scope statement).
 */

#ifndef __OBD_J1979_H__
#define __OBD_J1979_H__

#include <stdbool.h>

#include "dtc_decode.h"

struct obd_j1979_data {
	int vehicle_speed_kph;
	bool vehicle_speed_valid;
	int engine_rpm;
	bool engine_rpm_valid;
	/* float, not int: A*100/255 has real fractional precision (e.g.
	 * 47.5%), unlike vehicle_speed_kph/coolant_temp_c which are whole
	 * units straight off a single CAN data byte. */
	float fuel_level_pct;
	bool fuel_level_valid;
	int coolant_temp_c;
	bool coolant_temp_valid;
	/* PID 0x42, ((A*256)+B)/1000 volts — vehicle electrical system
	 * voltage, not this tracker's own supply. */
	float supply_voltage_v;
	bool supply_voltage_valid;
};

/**
 * @return true if any PID in data succeeded this cycle — i.e. the OBD bus
 * responded at all. Used as the standard commercial-OBD-dongle proxy for
 * "ignition/accessory power is on" (there's no dedicated PID for that).
 */
static inline bool obd_j1979_bus_responded(const struct obd_j1979_data *data)
{
	return data->vehicle_speed_valid || data->engine_rpm_valid || data->fuel_level_valid ||
	       data->coolant_temp_valid || data->supply_voltage_valid;
}

/** Starts the CAN controller, RX filter, and polling thread. */
void obd_j1979_init(void);

/** Copies the most recently decoded PID values under lock. */
void obd_j1979_get_latest(struct obd_j1979_data *out);

/** Copies the most recently decoded DTC list under lock. */
void obd_j1979_get_latest_dtcs(struct dtc_list *out);

#endif /* __OBD_J1979_H__ */
