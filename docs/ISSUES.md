# Modules built without native_sim runtime verification

Tracking list, per your request — one place to check off against once the
native_sim/vcan0 loop is actually working (see `TESTING.md` for why it isn't
yet: this host is macOS, and Zephyr's POSIX architecture — which
`native_sim` and the `zephyr,native-linux-can` driver both require — only
builds on Linux).

Everything below is **compile/link-verified on the nrf9160dk target only**
(clean pristine builds, zero compiler diagnostics, confirmed at the time
each was written — see `AUDIT.md`'s per-phase sections for the specific
build logs). None of it has been exercised against real or simulated CAN
traffic, a real broker, or real IMU data. Logic correctness (PID decode
math, DTC bit-encoding, MQTT protocol sequencing, threshold math) has been
reasoned through carefully and grounded in checked-out Zephyr/NCS source
where APIs were involved, but "compiles clean" is not the same claim as
"behaves correctly," and this list exists so that gap doesn't get lost.

## Phase 3 — MQTT/TLS uplink

- `src/uplink_mqtt.c`/`.h` — connect/reconnect/backoff state machine, the
  three-topic publish logic, LWT/retained status. Never actually connected
  to a broker (real or Mosquitto-on-native_sim); the CONNACK-wait loop,
  reconnect backoff timing, and TLS handshake sequencing are all unverified
  in practice.
- `src/buffer_fifo.c`/`.h` — flash-backed FIFO (NVS-based), two channels
  (telemetry drop-oldest, events reject-on-full). NVS mount, push/peek/
  pop-ack cycle, and the overflow policies have not been exercised —
  correctness rests on reading the NVS API correctly, not on having run it.

## Phase 4 — OBD PID + DTC coverage

- `src/obd_j1979.c`/`.h` — Mode 01 PID request/response state machine
  (speed/RPM/fuel/coolant) and the polling thread that drives it. Zero CAN
  frames have ever actually round-tripped through this code — the request-
  building, response-matching-by-PID, and per-PID decode formulas are
  unverified against real or simulated ECU responses.
- `src/dtc_decode.c`/`.h` — Mode 03 request build + single-frame response
  parse, DTC bit-encoding (category/digit extraction). Same: never actually
  parsed a real Mode 03 response frame. The single-frame-only limitation
  (no ISO 15765-2 multi-frame reassembly) is stated in the header as a
  design scope limit, not something tested against a multi-DTC vehicle.
- `src/app_sensors.c`'s DTC-event dedup logic (`dtc_already_reported()`/
  `dtc_mark_reported()`, the per-code "fire once" semantics) — logic reviewed
  carefully against the spec you gave, but never run against an actual
  sequence of DTC polls.

## Phase 5 — IMU harsh-driving detection

Part: **ICM-42688-P** (confirmed choice), Zephyr mainline driver
(`drivers/sensor/tdk/icm42688`, compatible `invensense,icm42688`), SPI-only
(no I2C backend in this driver), added as a second device on the same
`&arduino_spi` bus the MCP2515 already uses (second `cs-gpios` entry, D9).

- `src/harsh_driving.c`/`.h` — threshold-math functions. Genuinely
  IMU-agnostic and Zephyr-agnostic: **zero Zephyr/driver includes at all**
  (just `<math.h>`), so this specifically could be unit-tested via
  `west twister --platform unit_testing` or even a bare host compile,
  without needing native_sim, the CAN bridge, or any Zephyr build at all —
  but "could be" and "has been" aren't the same thing; not run yet. The
  default thresholds (`HARSH_DRIVING_DEFAULT_THRESHOLDS`, ±3.5-4.0 m/s²)
  are placeholder starting values, not derived from any vehicle spec or
  real driving data — need tuning once real/replayed accelerometer traces
  are available.
- `src/imu_icm42688.c`/`.h` — sensor I/O (polls via
  `sensor_sample_fetch()`/`sensor_channel_get()` at 100ms, no
  interrupt/trigger mode) and the event-publish glue calling into
  `harsh_driving_evaluate()`. Compile/link-verified only (confirmed the
  ICM-42688-P devicetree node correctly resolves onto
  `spi@b000/icm42688@1` in the final build's `zephyr.dts`) — zero actual
  sensor reads have happened; the SPI transaction sequence, the
  `sensor_value`→float conversion, and the polling cadence are all
  unverified against real or simulated hardware.
- Axis convention (X=longitudinal, Y=lateral) assumes a fixed, known
  mounting orientation with no runtime calibration — stated as a design
  limitation in `harsh_driving.h`, not a tested/validated assumption.

## Phase 6 — OTA update

**This one matters more than the others** (your own framing, and correct):
"does the device still boot after an update" isn't something a compile-clean
build guarantees at all, unlike CAN/MQTT/IMU logic where a clean build at
least proves the code paths are internally consistent.

- `src/uplink_mqtt.c`'s new `MQTT_EVT_PUBLISH` handling, the
  `trackers/{device_id}/ota` subscribe, and `mqtt_publish_qos1_ack()` — no
  message has ever actually been received and parsed through this path.
- `src/ota_update.c`'s URL-splitting (`split_url()`), the OTA-pointer JSON
  parse, the `FOTA_UPDATE_SERVER_HOSTNAME` host-match validation, and the
  `fota_download_start_with_image_type()` call itself — none of these have
  run against a real HTTP(S) download. `fota_download`'s internal
  download→flash-write→schedule-update pipeline (confirmed from source,
  not run) is NCS's own well-tested library, but the glue code around it
  here is not.
- **The one thing in this whole project that most needs a real hardware
  test before you trust it**: the actual fetch → stage → reboot → swap →
  `boot_write_img_confirmed()` cycle. Recommend, in order, before relying
  on this for anything real:
  1. Flash the current build once via `west flash` (establishes a known-
     good baseline image in the primary slot).
  2. Bump `VERSION`, do a pristine build, host the resulting
     `zephyr.signed.bin` (see AUDIT.md's Phase 6 section for exactly which
     file) somewhere reachable, publish a real
     `trackers/{device_id}/ota` MQTT message pointing at it.
  3. Confirm on the device console: FOTA progress logs, `FOTA_DOWNLOAD_EVT_FINISHED`,
     the reboot, and — critically — that the device actually comes back up
     running the new version (check `fw_version` in its next telemetry
     payload) rather than MCUboot reverting a bad swap.
  4. Only after that succeeds once for real should this be considered
     "working," regardless of how clean the build log looks.

## What "verification" would actually look like, once unblocked

1. `native_sim` + `vcan0`: confirm `obd_j1979.c`/`dtc_decode.c` correctly
   request/decode PIDs and DTCs against synthetic CAN traffic
   (`cangen`/`canplayer`/a small Python OBD-II ECU simulator).
2. A local Mosquitto broker (TLS optional for this step): confirm
   `uplink_mqtt.c` actually connects, publishes to all three topics, and
   that `buffer_fifo.c` replays correctly across a simulated disconnect.
3. Once real IMU hardware or a data-replay harness exists: confirm
   `harsh_driving.c`'s thresholds fire (and don't false-positive) against
   real or recorded accelerometer traces.
4. Real hardware, last: nRF9160-DK + actual MCP2515/GNSS/IMU, since
   native_sim/Mosquitto verification is explicitly scoped as logic/protocol
   correctness only, not radio- or SPI-timing correctness (see `TESTING.md`).
