# OBD-II / CAN Vehicle Tracker

[![Build firmware](https://github.com/goldenglorys/telenode/actions/workflows/test.yml/badge.svg)](https://github.com/goldenglorys/telenode/actions/workflows/test.yml)

> Note this badge reflects a **compile/build check only** — there is no
> automated unit/integration test suite in this project yet; see
> "Testing" below.

A self-hosted, DIY vehicle tracker built for Nordic nRF9160-DK + u-blox
NEO-M9N GNSS + MCP2515/SN65HVD230 CAN, targeting functional parity with
commercial trackers like the Jimi IoT JM-VL04: live GPS location, live
OBD-II PIDs (speed, RPM, fuel, coolant, supply voltage), stored DTC
(check-engine) codes, IMU-based harsh-driving detection, offline
buffering across connectivity gaps, and MQTT/TLS upload — all to a
broker and backend you run yourself, not a vendor cloud.

## Documentation

This README covers setup, build, and deployment. For everything else —
especially if you're an AI agent picking this project up and don't want
to skim the whole codebase — start with **[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)**,
the single source of truth for what this project is, its module map, and
pointers to everything else:

| Doc | Purpose |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | High-level overview, architecture diagram, module map, goals, hard constraints — **read this first** |
| [`docs/AUDIT.md`](docs/AUDIT.md) | Full phase-by-phase implementation log and decision record |
| [`docs/ISSUES.md`](docs/ISSUES.md) | Running list of modules built without runtime verification |
| [`docs/TESTING.md`](docs/TESTING.md) | Per-module compile/native_sim/hardware verification table, native_sim setup steps |

## Attribution

This project is a fork of Golioth's open-source
[`reference-design-can-asset-tracker`](https://github.com/golioth/reference-design-can-asset-tracker)
(Apache-2.0), which supplied the original CAN bus / OBD-II task
scaffolding, GPS/NMEA parsing, and Zephyr/nRF Connect SDK project
structure. The Golioth Cloud transport (LightDB Stream/State, RPC,
Settings Service, package/cohort/deployment OTA) has been fully replaced
with a self-hosted MQTT/TLS uplink and a from-scratch OTA flow built on
NCS's own `fota_download`/`dfu_target` libraries — see `docs/AUDIT.md` for the
full phase-by-phase record of what was kept vs. replaced and why.

## Hardware

- Nordic nRF9160-DK
- u-blox NEO-M9N GNSS module (NMEA over UART, via `arduino_serial`)
- Microchip MCP2515 CAN controller + Texas Instruments SN65HVD230 CAN
  transceiver (SPI, `arduino_spi`)
- InvenSense ICM-42688-P IMU (SPI, same `arduino_spi` bus as the
  MCP2515, second chip-select) — harsh-driving (accel/brake/corner)
  detection

## Architecture

- `src/main.c` — boot sequence (MCUboot image confirm, sensor/OBD/IMU
  init, LTE bring-up, main sensor-read loop).
- `src/app_sensors.c`/`.h` — GPS (NMEA RMC + GGA) parsing, ties together
  OBD data, builds and publishes the telemetry JSON envelope
  (`device_id`, `fw_version`, `timestamp`, `location`, `obd`, `power`,
  `buffered`), and DTC-detected event dedup/publish logic.
- `src/obd_j1979.c`/`.h` — SAE J1979 Mode 01 PID request/response state
  machine (vehicle speed, engine RPM, fuel level, coolant temp, control
  module voltage) over the CAN bus set up via `zephyr,canbus`.
- `src/dtc_decode.c`/`.h` — Mode 03 (read stored DTCs) request build and
  single-frame response decode (P/C/B/U category + 4-digit code).
  Read-only: no Mode 04 (clear DTCs) or any CAN-write/actuation
  capability is implemented, by design.
- `src/harsh_driving.c`/`.h` — pure threshold-math evaluation of
  longitudinal/lateral acceleration into harsh-acceleration/braking/
  cornering events. Zero Zephyr dependencies (unit-testable standalone).
- `src/imu_icm42688.c`/`.h` — polls the ICM-42688-P over SPI, feeds
  readings to `harsh_driving.c`, publishes `harsh_driving_detected`
  events.
- `src/uplink_mqtt.c`/`.h` — MQTT/TLS uplink to the self-hosted broker.
  Publishes to `trackers/{device_id}/telemetry` (QoS 0),
  `trackers/{device_id}/events` (QoS 1, DTC/harsh-driving events),
  `trackers/{device_id}/status` (QoS 1, retained + Last-Will-and-
  Testament); subscribes to `trackers/{device_id}/ota` (QoS 1) for
  firmware-update pointers.
- `src/buffer_fifo.c`/`.h` — flash-backed (NVS) offline buffer, separate
  drop-oldest (telemetry) and reject-on-full (events) channels, replayed
  on reconnect.
- `src/ota_update.c`/`.h` + `src/ota_update_certs.h` — OTA update. MQTT
  delivers a small JSON pointer (`{"version": "...", "url": "..."}`);
  this module hands the URL to NCS's `fota_download` library, which
  downloads the signed image over HTTPS and stages it as an MCUboot
  test-swap. No custom signing/verification logic — MCUboot verifies the
  image signature before it's ever run, exactly as it does for a
  locally-flashed image.
- `src/app_settings.c`/`.h` — local runtime config (loop/GPS/vehicle-speed
  poll delays, fake-GPS override), still used as-is from the original
  design.
- `src/app_rpc.c`/`.h`, `src/app_state.c`/`.h` — no-op stubs left over
  from removed Golioth RPC / LightDB State demo code, kept per Phase 2's
  decision in case a future MQTT command topic re-wires them; currently
  inert and not called from anywhere.

## Setup

**Do not `git clone` this repo to set up a workspace.** Zephyr's `west`
meta-tool manages this as a multi-repo manifest workspace (this repo plus
the whole Zephyr/nRF Connect SDK dependency tree) — cloning directly
gives you this directory only, none of the dependencies, and none of the
manifest-driven fetching described below.

```shell
mkdir obd-tracker && cd obd-tracker
python3 -m venv .venv && source .venv/bin/activate
pip install wheel west ecdsa

west init -m https://github.com/goldenglorys/telenode.git .
west update              # large fetch: Zephyr + NCS + HALs + this app
west zephyr-export
pip install -r deps/zephyr/scripts/requirements.txt
west sdk install         # installs the Zephyr SDK toolchain (needs `wget`)
```

`west update` resolves this project's own manifest (`app/west.yml`) — you
do not need to separately fetch Zephyr, the nRF Connect SDK, or any HALs;
`west` does that from the manifest automatically.

## Building and flashing

```shell
west build -p -b nrf9160dk/nrf9160/ns --sysbuild app
west flash
```

Update the `VERSION` file and do a pristine (`-p`) build whenever you
want the firmware-reported version to change — required for OTA to have
something new to offer. A successful build produces, among other things,
`build/app/zephyr/zephyr.signed.bin` and `build/dfu_application.zip` —
see the OTA row in the configuration table below for which of these to
host for firmware updates.

## Testing

There is **no automated unit/integration test suite** in this project
yet — see `docs/TESTING.md` for the honest per-module verification
status (short version: everything is compile-verified only; nothing has
been run against real or simulated hardware). The GitHub Actions badge at
the top of this file reflects a **build/compile check**, not a test
suite passing.

What actually exists to run, once you have a Linux host (`native_sim` —
Zephyr's POSIX-architecture simulation target — requires Linux; it does
not build on macOS or Windows):

```shell
sudo modprobe vcan && sudo ip link add dev vcan0 type vcan
sudo ip link property add dev vcan0 altname zcan0 && sudo ip link set up vcan0

west build -p -b native_sim app --no-sysbuild
./build/zephyr/zephyr.exe
```

Then, in another terminal, drive CAN traffic against `vcan0` with
`cangen`/`canplayer`/`python-can` (or a small OBD-II ECU simulator) and
watch `zephyr.exe`'s log output to confirm `obd_j1979.c`/`dtc_decode.c`
actually parse the frames. Full detail, including why this is blocked on
non-Linux hosts and what it does/doesn't validate, is in
`docs/TESTING.md`.

`harsh_driving.c` has zero Zephyr dependencies and is the one module
that's genuinely unit-testable standalone (e.g. via `west twister
--platform unit_testing`) without any of the above — this hasn't been
wired up yet, but would be the cheapest real automated test to add.

## Configuration — required before deployment

Everything below is a placeholder in the current tree. This is the one
list to check before shipping a device:

| What | Where | Current state |
|---|---|---|
| MQTT broker hostname | `CONFIG_UPLINK_MQTT_BROKER_HOSTNAME` (`Kconfig`) | Empty string — set to your broker's hostname/IP |
| MQTT broker port | `CONFIG_UPLINK_MQTT_BROKER_PORT` (`Kconfig`) | Defaults to `8883` (MQTT-over-TLS); change only if your broker differs |
| MQTT TLS credentials | `src/uplink_mqtt_certs.h` | Three placeholder PEM blocks (broker CA cert, device client cert, device private key) — mutual TLS, replace all three |
| MQTT sec_tag | `CONFIG_UPLINK_MQTT_SEC_TAG` (`Kconfig`) | Defaults to `1234567` — change if this device already uses that modem sec_tag for something else |
| Device ID | `CONFIG_UPLINK_MQTT_DEVICE_ID` (`Kconfig`) | Empty — falls back to modem IMEI if left blank (fine for most cases) |
| Firmware update server hostname | `CONFIG_FOTA_UPDATE_SERVER_HOSTNAME` (`Kconfig`) | Empty string, confirmed still a placeholder (not renamed or set to a real value during Phase 6) — while empty, any OTA URL's host is accepted with a loud warning logged; set this once you have a real update-hosting hostname, since a non-empty value here makes `ota_update.c` reject any OTA message whose URL host doesn't match |
| Firmware update CA cert | `src/ota_update_certs.h` | One placeholder PEM (server-authenticated HTTPS only, not mutual TLS) |
| Firmware update sec_tag | `CONFIG_FOTA_UPDATE_SEC_TAG` (`Kconfig`) | Defaults to `1234568` — deliberately separate from the MQTT sec_tag; see `docs/AUDIT.md`'s Phase 6 section for why |

Every other `TODO`/"placeholder" occurrence in `src/` and `prj.conf` is a
historical `TODO(replace-with-mqtt)` marker documenting what Golioth
functionality used to live there and was already replaced — not an
outstanding action item. The table above is the complete list of what
still needs real values.

## Known limitations / status

Full detail in `docs/ISSUES.md` and `docs/TESTING.md`; summary:

- **Never run against real or simulated CAN/GNSS/IMU traffic, a real MQTT
  broker, or real OTA hardware cycle.** Everything is compile/link-clean
  on the nRF9160-DK target only — logic has been carefully reasoned
  through and grounded in checked-out Zephyr/NCS source, but "compiles
  clean" is not "behaves correctly." `native_sim` verification is blocked
  on this development host (macOS; Zephyr's POSIX architecture requires
  Linux) — see `docs/TESTING.md`.
- **DTC decoding is single-frame only** — no ISO 15765-2 multi-frame
  reassembly, so vehicles reporting more than 2-3 DTCs in one response
  won't have all of them decoded. Stated as a scope limit, not tested
  against a real multi-DTC vehicle.
- **Harsh-driving thresholds are placeholders** (±3.5-4.0 m/s²), not
  derived from any vehicle spec or real driving data — expect to need
  tuning once real/replayed accelerometer traces are available.
- **IMU axis convention is fixed** (X=longitudinal, Y=lateral) with no
  runtime calibration — assumes a known, fixed mounting orientation.
- **OTA fetch → stage → reboot → swap cycle has not been tested on real
  hardware, even once.** This is the single highest-priority item to
  verify before relying on it — see `docs/ISSUES.md`'s Phase 6 section for a
  concrete 4-step real-hardware test plan. Signature verification itself
  is enforced by MCUboot (unmodified, untouched by this project) before
  any image is ever run, independent of whether this test has been done.

## License

Apache-2.0 — see [`LICENSE`](LICENSE).
