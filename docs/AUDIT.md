# Phase 1 Audit — golioth/reference-design-can-asset-tracker

Read-only audit. No source files have been modified. Workspace was set up via
`west` (not `git clone`) per the upstream README:

```
mkdir ~/obd-tracker && cd ~/obd-tracker
python3 -m venv .venv && source .venv/bin/activate
pip install wheel west ecdsa
west init -m https://github.com/golioth/reference-design-can-asset-tracker.git .
west update      # <- large fetch (Zephyr + NCS + HALs), in progress at time of writing
west zephyr-export
pip install -r deps/zephyr/scripts/requirements.txt
```

Manifest self-path is `app` (i.e. this repo's contents live under
`~/obd-tracker/app/`, dependencies under `~/obd-tracker/deps/`).

Baseline build status: **PASSED — clean, zero warnings/errors.**

```
$ west build -p -b nrf9160dk/nrf9160/ns --sysbuild app
...
Memory region         Used Size  Region Size  %age Used
           FLASH:      357760 B       416 KB     83.98%
             RAM:       84668 B     211608 B     40.01%
        IDT_LIST:          0 GB        32 KB      0.00%
...
[23/23] Generating ../merged.hex
```

Verified artifacts exist: `build/app/zephyr/zephyr.bin`,
`build/app/zephyr/zephyr.signed.bin`, `build/merged.hex`,
`build/dfu_application.zip`. Full build log scanned for `error`/`warning` —
zero matches outside the "0 errors, 0 warnings" linker summary lines. This is
the baseline every later phase's diff will be checked against.

**Toolchain notes for reproducing this environment** (not upstream issues,
just this host's setup — worth keeping in mind since you may hit the same
things):
- Homebrew's current `cmake` (4.4.2) is incompatible with this Zephyr/NCS
  version's `FindZephyr-sdk.cmake` — it hits a real `if()` argument-parsing
  bug when `ZEPHYR_TOOLCHAIN_VARIANT` is undefined (undefined-variable
  expansion inside `STREQUAL` swallows a token). Fixed by `pip install
  "cmake<4"` inside the venv (installs 3.31.10), which takes PATH priority
  over the Homebrew one once the venv is active. No firmware code involved —
  pure host tooling version skew.
- `west sdk install` additionally needed `wget` (`brew install wget`) for the
  SDK's own `setup.sh -t arm-zephyr-eabi -c` step.

---

## 1. Files under `src/` — one line each

| File | Purpose |
|---|---|
| `src/main.c` | Entry point. Boots sensors, connects LTE (nRF91x) or Wi-Fi, calls `start_golioth_client()`, wires button/LED GPIO, drives the Ostentus e-paper slideshow, and runs the main loop that calls `app_sensors_read_and_stream()` every `LOOP_DELAY_S` seconds. |
| `src/main.h` | Single declaration: `wake_system_thread()`, used by settings callbacks to interrupt the main loop's sleep when `LOOP_DELAY_S` changes. |
| `src/app_sensors.c` | The actual data-production logic: CAN bus OBD-II PID 0x0D (vehicle speed) request/response thread, UART/NMEA GPS parsing thread, shared-state combination of the two, and the Golioth Stream Client publish call. This is the file with the CAN driver plumbing worth keeping. |
| `src/app_sensors.h` | Declares `app_sensors_init/read_and_stream/set_client`; defines Ostentus slide-key enum and label strings. |
| `src/app_rpc.c` | Registers three Golioth RPC (Remote Procedure Call) handlers: `get_network_info`, `reboot`, `set_log_level`. Pure cloud-control-plane code, no sensor data production. |
| `src/app_rpc.h` | Declares `app_rpc_register(struct golioth_client *)`. |
| `src/app_settings.c` | Registers six Golioth Settings Service callbacks (`LOOP_DELAY_S`, `GPS_DELAY_S`, `FAKE_GPS_ENABLED`, `FAKE_GPS_LATITUDE`, `FAKE_GPS_LONGITUDE`, `VEHICLE_SPEED_DELAY_S`) and exposes getters used elsewhere in `src/`. The getters themselves are generic config state, not Golioth-specific. |
| `src/app_settings.h` | Declares the getters and `app_settings_register(struct golioth_client *)`. |
| `src/app_state.c` | Implements a toy "digital twin" demo (`example_int0`/`example_int1`) against Golioth's LightDB State service — observes a `desired` path, validates, writes an `actual` path back. Entirely a Golioth Cloud demo feature; no real vehicle data flows through it. |
| `src/app_state.h` | Declares `app_state_observe/update_actual` and the `desired`/`state` endpoint path strings. |
| `src/json_helper.h` | `zephyr/data/json.h` descriptor (`app_state_descr`) for parsing the `example_int0`/`example_int1` JSON used only by `app_state.c`. |
| `src/lib/minmea/` (west-managed, not app-owned) | Vendored NMEA-0183 parser library (`minmea.c/h`), pulled in via `west.yml` as its own project rooted at `app/src/lib/minmea`. Fully generic, not Golioth-specific — used by `app_sensors.c` to decode GPS RMC sentences. |

Not under `src/` but relevant: `boards/*.overlay`, `boards/*.conf`, `socs/nrf9160_ns.conf`,
`pm_static.yml`, `pipelines/json-to-lightdb.yml`, `prj.conf`, `Kconfig`,
`sysbuild.conf`, `CMakeLists.txt`, `west.yml`, `sample.yaml` — covered in
sections 3/4 below.

---

## 2. Golioth Firmware SDK call sites (file + line)

Every `golioth_*` function call and `#include <golioth/...>` header, as of
this checkout:

**`src/main.c`**
- L15–16: `#include <golioth/client.h>`, `#include <golioth/fw_update.h>`
- L43: `static struct golioth_client *client;`
- L62–72: `on_client_event()` — Golioth client connect/disconnect event callback
- L74–101: `start_golioth_client()` — `golioth_client_create`, `golioth_client_register_event_callback`, `golioth_fw_update_init`, plus calls into `app_state_observe`, `app_sensors_set_client`, `app_settings_register`, `app_rpc_register` (all Golioth-service registration)
- L117: `start_golioth_client()` invoked from the LTE connect handler
- L225, L228: `start_golioth_client()` invoked in the non-nRF91x path; blocks on `connected` semaphore that is only ever given inside the Golioth client event callback

**`src/app_sensors.c`**
- L10–11: `#include <golioth/client.h>`, `#include <golioth/stream.h>`
- L43: `static struct golioth_client *client;`
- L476–481: **the actual transport call** — `golioth_client_is_connected(client)` guard, then `golioth_stream_set_sync(client, "tracker", GOLIOTH_CONTENT_TYPE_JSON, json_buf, strlen(json_buf), GOLIOTH_STREAM_TIMEOUT_S)`. This is the single splice point for the new MQTT publish (see section 5).
- L485–488: `app_sensors_set_client()` — stores the client pointer handed in from `main.c`

**`src/app_rpc.c`**
- L10–11: `#include <golioth/client.h>`, `#include <golioth/rpc.h>`
- L37–44: `on_get_network_info()` — returns `GOLIOTH_RPC_OK` / `GOLIOTH_RPC_UNIMPLEMENTED`
- L91–98: `on_reboot()` — returns `GOLIOTH_RPC_OK`
- L100–105: `rpc_log_if_register_failure()` — generic wrapper, no direct Golioth call, but only exists to serve registration below
- L107–121: `app_rpc_register()` — `golioth_rpc_init`, 3× `golioth_rpc_register`

**`src/app_rpc.h`**
- L17: `#include <golioth/client.h>`
- L19: `void app_rpc_register(struct golioth_client *client);`

**`src/app_settings.c`**
- L10–11: `#include <golioth/client.h>`, `#include <golioth/settings.h>`
- L73–124: six `on_*_setting()` callbacks, each returning `enum golioth_settings_status` (`GOLIOTH_SETTINGS_SUCCESS` / `GOLIOTH_SETTINGS_VALUE_OUTSIDE_RANGE`)
- L126–168: `app_settings_register()` — `golioth_settings_init`, 2× `golioth_settings_register_int_with_range`, `golioth_settings_register_bool`, 2× `golioth_settings_register_float`

**`src/app_settings.h`**
- L18: `#include <golioth/client.h>`
- L21: `void app_settings_register(struct golioth_client *client);`

**`src/app_state.c`**
- L10–11: `#include <golioth/client.h>`, `#include <golioth/lightdb_state.h>`
- L24: `static struct golioth_client *client;`
- L26–38: `async_handler()` — checks `enum golioth_status`
- L49–59: `app_state_reset_desired()` — `golioth_lightdb_set_async(...)`
- L62–83: `app_state_update_actual()` — `golioth_lightdb_set_async(...)`
- L85–165: `app_state_desired_handler()` — checks `enum golioth_status`, calls back into `app_state_update_actual`/`app_state_reset_desired`
- L167–190: `app_state_observe()` — `golioth_lightdb_observe_async(...)`, then calls `app_state_update_actual()`

**`src/app_state.h`**
- L29: `#include <golioth/client.h>`
- L34–35: functions take/return no client type directly here, but declare the observe/update pair above

No backend-logging call sites appear directly in `src/` — `CONFIG_LOG_BACKEND_GOLIOTH=y` in `prj.conf` hooks Golioth's log backend in at the Kconfig/logging-subsystem level, not via an explicit function call in this app's code. Removing the Kconfig option is sufficient there.

---

## 3. `west.yml` — full contents, flagged

```yaml
manifest:
  version: 1.0
  projects:
    - name: golioth                        # GOLIOTH-SPECIFIC — Golioth Firmware SDK itself.
      path: modules/lib/golioth-firmware-sdk # Also the vehicle for importing the entire
      revision: v0.21.0                      # Zephyr + NCS + HAL tree (see `import:` below) —
      url: .../golioth-firmware-sdk.git      # removing this project removes the SDK but you
      west-commands: scripts/west-commands.yml # must re-import zephyr/nrf/HALs directly in
      submodules: true                       # west.yml once this is gone (west update will
      import:                                # not resolve zephyr/nrf/etc. without it).
        file: west-ncs.yml
        path-prefix: deps
        name-allowlist:                      # GENERIC — these are the actual Zephyr/NCS/HAL
          - nrf                              # projects Golioth's SDK happens to pull in.
          - zephyr                           # Must be preserved (re-imported directly) once
          - cmsis_6                          # the golioth-firmware-sdk project is removed:
          - hal_nordic                        # zephyr, nrf, hal_nordic, mbedtls, mcuboot,
          - mbedtls                           # nrfxlib, tinycrypt, trusted-firmware-m,
          - mbedtls-nrf                        # zcbor/qcbor, cmsis_6, segger, oberon-psa-crypto,
          - mcuboot                            # net-tools. None of these are Golioth-owned.
          - net-tools
          - nrfxlib
          - oberon-psa-crypto
          - qcbor
          - segger
          - qcbor                              # (duplicate entry in upstream file, harmless)
          - tfm-mcuboot
          - tinycrypt
          - trusted-firmware-m
          - zcbor

    - name: golioth-zephyr-boards           # GOLIOTH-SPECIFIC — Aludel-Mini/Elixir board
      path: deps/modules/lib/golioth-boards  # defs. Not used by the nRF9160-DK target.
      revision: v2.1.1                       # Safe to remove per upstream README itself.
      url: .../golioth-zephyr-boards

    - name: libostentus                     # GOLIOTH-SPECIFIC — Ostentus ePaper faceplate
      path: deps/modules/lib/libostentus     # driver library. Only referenced under
      revision: v2.0.0                       # `#ifdef CONFIG_LIB_OSTENTUS` guards in src/.
      url: .../libostentus

    - name: zephyr-network-info              # GOLIOTH-AUTHORED, but functionally generic —
      path: deps/modules/lib/network-info     # queries/formats network info; only consumed
      revision: v1.2.0                        # today via the Golioth RPC handler
      url: .../zephyr-network-info             # (`on_get_network_info` in app_rpc.c). Can be
                                                # kept and repurposed for an MQTT `status` topic,
                                                # or dropped — not a hard cloud dependency.

    - name: golioth-battery-monitor          # GOLIOTH-AUTHORED, functionally generic — reads
      path: deps/modules/lib/battery-monitor  # battery V/%, gated behind
      revision: v1.1.0                        # CONFIG_ALUDEL_BATTERY_MONITOR (Aludel-only in
      url: .../battery-monitor                 # practice, since the DK doesn't wire a battery
                                                # ADC here). Not cloud-specific; safe to keep
                                                # dormant or drop with the Aludel boards.

    - name: minmea                           # GENERIC — vendored NMEA parser, not Golioth's.
      path: app/src/lib/minmea                # Must stay.
      revision: 85439b97dd4984c5efb84ce954b85088e781dae8
      url: https://github.com/kosma/minmea.git

  self:
    path: app
```

**Verdict:** remove the `golioth` project (SDK) and re-import
`zephyr`/`nrf`/HAL projects directly (they currently only exist in the
manifest as a transitive `import:` of the Golioth SDK project — this is the
part of Phase 2 step 1 that needs care, since a naive deletion of the
`golioth` entry with no replacement `import:` will break `west update`
entirely, not just remove Golioth). Remove `golioth-zephyr-boards` and
`libostentus` outright (step 5). `zephyr-network-info` and
`golioth-battery-monitor` are not hard blockers — flagging for your call
rather than assuming.

---

## 4. `prj.conf` / `Kconfig` — every `CONFIG_GOLIOTH_*` option

**`prj.conf`** (Golioth-specific lines flagged):

```
CONFIG_GOLIOTH_FIRMWARE_SDK=y          # GOLIOTH — master enable switch
CONFIG_GOLIOTH_FW_UPDATE=y             # GOLIOTH — OTA via Golioth package/cohort/deployment
CONFIG_GOLIOTH_LIGHTDB_STATE=y         # GOLIOTH — LightDB State client (app_state.c)
CONFIG_LOG_BACKEND_GOLIOTH=y           # GOLIOTH — backend logging to Golioth Cloud
CONFIG_GOLIOTH_RPC=y                   # GOLIOTH — RPC service (app_rpc.c)
CONFIG_GOLIOTH_SETTINGS=y              # GOLIOTH — Settings service (app_settings.c)
CONFIG_GOLIOTH_STREAM=y                # GOLIOTH — Stream client (app_sensors.c publish call)
CONFIG_GOLIOTH_SAMPLE_COMMON=y         # GOLIOTH — sample-common helper lib (net_connect, creds)
CONFIG_GOLIOTH_SAMPLE_SETTINGS_AUTOLOAD=y   # GOLIOTH — auto-loads psk-id/psk from settings
CONFIG_GOLIOTH_SAMPLE_SETTINGS_SHELL=y      # GOLIOTH — `settings set golioth/psk...` shell cmds
CONFIG_GOLIOTH_RPC_MAX_RESPONSE_LEN=512     # GOLIOTH — RPC response buffer size
```

Everything else in `prj.conf` (`CONFIG_ZVFS_*`, `CONFIG_MBEDTLS_*`,
`CONFIG_NETWORKING`/`CONFIG_NET_IPV4`, `CONFIG_COAP_EXTENDED_OPTIONS_LEN*`,
`CONFIG_MAIN_STACK_SIZE`, `CONFIG_NET_LOG`, `CONFIG_NET_SHELL`,
`CONFIG_REBOOT`, `CONFIG_FLASH*`, `CONFIG_NVS`, `CONFIG_STREAM_FLASH`,
`CONFIG_IMG_MANAGER`, `CONFIG_IMG_ERASE_PROGRESSIVELY`, `CONFIG_SETTINGS*`,
`CONFIG_JSON_LIBRARY`, `CONFIG_I2C`, `CONFIG_GPIO`, `CONFIG_SERIAL`,
`CONFIG_UART_INTERRUPT_DRIVEN`, `CONFIG_CAN`) is generic Zephyr — kept as-is.
Note `CONFIG_COAP_EXTENDED_OPTIONS_LEN*` exists only because Golioth's
transport is CoAP-based; once removed, these two lines become dead config
(harmless to leave, but candidates for cleanup in Phase 2/3 since MQTT/TLS
doesn't use CoAP). Flash/NVS/IMG_MANAGER/MCUboot options are **not**
Golioth-specific — they back the generic Zephyr/MCUboot OTA mechanism and
must be preserved (relevant to Phase 6).

**`Kconfig`** (app-level, top of tree): zero `CONFIG_GOLIOTH_*` references.
Only defines `DNS_SERVER_IP_ADDRESSES`/`DNS_SERVER1` defaults and sources
`Kconfig.zephyr`. Fully generic — no changes needed here.

**`socs/nrf9160_ns.conf`**: no `CONFIG_GOLIOTH_*` lines, but the comment on
L43 ("Configure Golioth SDK dependencies (for NCS)") over the
`CONFIG_MBEDTLS_SSL_IN/OUT_CONTENT_LEN=2048` lines is misleading —  those
values were sized for Golioth's CoAP/DTLS stack. They'll need re-tuning once
MQTT/TLS is the transport in Phase 3 (different handshake/record sizes), but
that's a Phase 3 concern, not a Phase 2 deletion.

**`boards/aludel_elixir_ns.conf`**: `CONFIG_ALUDEL_BATTERY_MONITOR=y` +
`CONFIG_REGULATOR=y` — Aludel-board-specific, irrelevant to the nRF9160-DK
target already in use; this whole file goes away with the Aludel board
removal in Phase 2 step 5.

**`sysbuild.conf`**: `SB_CONFIG_BOOTLOADER_MCUBOOT=y` only — generic
Zephyr/MCUboot sysbuild flag, not Golioth's. Confirms Phase 6 step 1's
assumption: MCUboot enablement is a plain sysbuild option, not glued in
through Golioth.

---

## 5. Data path trace: CAN frame → `vehicle/speed` LightDB Stream path

Full trace, file by file, of exactly how a CAN response becomes a value at
the cloud-side `vehicle/speed` path — this is where the new MQTT publish call
gets spliced in later.

1. **`app_sensors_init()`** (`app_sensors.c:374-424`) starts the CAN
   controller (`can_start`) and spawns two long-running threads:
   `process_can_frames_thread` and `process_rmc_frames_thread`.

2. **`process_can_frames_thread()`** (`app_sensors.c:164-248`):
   - Registers a CAN RX filter for ID `0x7E8` (`OBD2_PID_RESPONSE_ID`) backed
     by a message queue (`can_add_rx_filter_msgq`), so matching frames land in
     `can_msgq` automatically via the CAN driver's ISR — no polling.
   - Loops forever: sends a Mode 01 PID `0x0D` (vehicle speed) request frame
     to ID `0x7DF` (`OBD2_PID_REQUEST_ID`) via `can_send()`, then drains
     `can_msgq` for up to 500ms.
   - For each response frame with `data[1] == 0x41` (Mode 01 + 0x40 response
     offset) and `data[2] == 0x0D`, takes `data[3]` as the raw speed byte
     (already km/h per J1979, no scaling needed for this PID).
   - Writes the result into the file-scope global `g_vehicle_speed` under
     `shared_data_mutex`.
   - Sleeps `get_vehicle_speed_delay_s()` seconds and repeats.

3. **GPS ingestion, in parallel:**
   - `serial_cb()` (`app_sensors.c:341-372`) is the UART IRQ handler; it
     accumulates characters from the NEO-M9N into `rx_buf` until a newline,
     then calls `process_reading()`.
   - `process_reading()` (`app_sensors.c:296-338`) uses the vendored
     `minmea` library to identify and parse `$..RMC` NMEA sentences. If the
     fix is valid (or fake-GPS is enabled and no real fix exists), and the
     `GPS_DELAY_S` interval has elapsed, it pushes the parsed
     `minmea_sentence_rmc` onto `rmc_msgq`.

4. **`process_rmc_frames_thread()`** (`app_sensors.c:250-293`) blocks on
   `rmc_msgq`. For each GPS fix that arrives, it locks `shared_data_mutex`,
   snapshots the *current* `g_vehicle_speed` value (whatever
   `process_can_frames_thread` last wrote), bundles
   `{rmc_frame, vehicle_speed}` into a `struct can_asset_tracker_data`, and
   pushes it onto `cat_msgq` (depth 64) — this is where GPS and CAN data are
   joined into one record, keyed to the GPS sample's timing rather than the
   CAN sample's.

5. **`main()`**'s top-level loop (`main.c:281-285`) calls
   `app_sensors_read_and_stream()` every `get_loop_delay_s()` seconds — this
   is the upload cadence, decoupled from both the GPS and CAN sampling rates.

6. **`app_sensors_read_and_stream()`** (`app_sensors.c:428-483`) drains
   `cat_msgq` completely (`while (k_msgq_get(..., K_NO_WAIT) == 0)`). For each
   queued record it:
   - Formats lat/lon as strings and an ISO-8601 timestamp from the RMC date/time
     fields (or omits `time` entirely for fake-GPS records — see
     `JSON_FMT_FAKE_GPS`).
   - Builds a JSON body via `snprintk` with the literal shape
     `{"time":"...","gps":{"lat":...,"lon":...,"fake":...},"vehicle":{"speed":N}}`.
   - **Splice point:** guards on `golioth_client_is_connected(client)`, then
     calls `golioth_stream_set_sync(client, "tracker", GOLIOTH_CONTENT_TYPE_JSON, json_buf, strlen(json_buf), GOLIOTH_STREAM_TIMEOUT_S)`
     (`app_sensors.c:476-481`). The string `"tracker"` is the LightDB Stream
     path prefix; combined with the JSON body's nesting, Golioth's cloud-side
     pipeline (`pipelines/json-to-lightdb.yml`, an `extract-timestamp` +
     `inject-path` transformer chain) is what actually maps
     `{"vehicle":{"speed":N}}` under the `"tracker"` path into the
     `vehicle/speed` LightDB Stream sub-path shown in the README example
     payload. **The path name is a cloud-side pipeline concern, not something
     hardcoded per-field in firmware** — the firmware only ever sends one flat
     JSON blob to the `"tracker"` path.

**Conclusion for Phase 3 wiring:** the one and only place a new
`uplink_mqtt_publish_telemetry(json_buf, len)`-style call needs to replace the
deleted `golioth_stream_set_sync(...)` call is `app_sensors.c:476-481`, inside
`app_sensors_read_and_stream()`. No other file touches the CAN-derived speed
value on its way out. The `json_buf` payload construction above it
(`app_sensors.c:452-475`) is pure data production and needs no change — it's
already exactly the JSON shape needed for the `telemetry` MQTT topic, modulo
whatever schema changes you want for the new PID/DTC/IMU fields being added in
Phases 4–5.

---

## Resolved — user review (2026-08-10)

Splice point (`app_sensors.c:476-481`) confirmed correct, no changes.
Both open items below resolved and addressed:

### 1. `west.yml` import fix — DONE, verified in isolation

Replaced golioth's transitive `import: {file: west-ncs.yml, ...}` with a
direct `nrf` project entry pinned at the exact same revision (`v3.1.1`) and
the same `name-allowlist` (see diff in `west.yml`). Verified byte-for-byte
identical resolution before/after:
- `west list` output diffed against a pre-change snapshot — **zero
  differences** (same paths, same revisions, same URLs for all 20 projects).
- `git rev-parse HEAD` for `deps/nrf` and `deps/zephyr` unchanged:
  `nrf` = `e2a97fe2578a1030f99f85d5e9e61160d261f0e3`,
  `zephyr` = `ff8f0c579eeb896876b6f36aca70c2bbfa756e19`.
- Incremental build after the change: clean, zero errors/warnings.

One non-obvious thing surfaced along the way, worth knowing for later
manifest edits: **`import.path-prefix` on a project applies to that
project's own resolved path, too — not just to the projects its import
pulls in.** First attempt set the new `nrf` project's `path: deps/nrf` *and*
`import.path-prefix: deps`, which doubled up to `deps/deps/nrf` (caught by
the `west list` diff). Same mechanism explained a mystery from the original
manifest: `golioth`'s own declared `path:` was `modules/lib/golioth-firmware-sdk`
(no prefix) in `west.yml`, yet it was physically checked out at
`deps/modules/lib/golioth-firmware-sdk` — because its own (now-removed)
`import.path-prefix: deps` was silently prepending to its own path too. Fixed
by giving `nrf` a bare `path: nrf` (letting path-prefix add `deps/`) and
giving `golioth` an explicit `path: deps/modules/lib/golioth-firmware-sdk`
(baking in what the prefix used to add implicitly), so removing golioth's
`import:` block doesn't also silently relocate its checkout.

### 2. `zephyr-network-info` / `golioth-battery-monitor` — function-level split

Both libraries mix genuinely generic local-sensor-reading logic with an API
surface that's inherently Golioth-RPC/Stream-typed (not just "wrapped" —
the function *signatures themselves* take Golioth/zcbor types). Keeping both
per your instruction; here's exactly which functions are which, so Phase 2
only touches the call sites, not the vendored libraries themselves (both are
separate west-managed git repos, pinned by revision — not part of this app's
own git history, so we don't edit them in place):

**`zephyr-network-info`** (`deps/modules/lib/network-info/`):

| Function | File | Category | Notes |
|---|---|---|---|
| `network_info_log(void)` | `network_info_modem_info.c` (nRF91x/modem), `network_info_nrf7002dk.c` (WiFi), `network_info_placeholder.c` (fallback) | **Generic — keep, callable as-is** | Zero Golioth types in signature or body. Just calls `modem_info_string_get()` / `cmd_wifi_status()` and `LOG_DBG`s the results. Safe to call directly from new code (e.g. to populate an MQTT `status` topic payload) once transport wiring is stripped from `app_rpc.c`. |
| `network_info_add_to_map(zcbor_state_t *response_detail_map)` | same three files | **Golioth-transport-specific — do not port** | Signature itself is the Golioth RPC response-encoding contract (`zcbor_state_t*` param, `enum golioth_rpc_status` return via `golioth/rpc.h`). Only caller is `app_rpc.c`'s `on_get_network_info()`, which goes away entirely with the rest of RPC removal. The underlying data (`modem_info_string_get`/`cmd_wifi_status` results) is already duplicated in `network_info_log()` above — no new code needed to preserve the local logic, since nothing here is unique to this function. |

**`golioth-battery-monitor`** (`deps/modules/lib/battery-monitor/`):

| Function | File | Category | Notes |
|---|---|---|---|
| `battery_measurement_setup(void)`, `battery_measure_enable(bool)`, `battery_read_voltage(struct battery_data*)`, `battery_level_pptt(...)` (static) | `bat_voltage_divider.c` (or `bat_max17262.c` depending on board) | **Generic — keep, callable as-is** | Pure ADC/GPIO/fuel-gauge hardware reads. Zero Golioth references confirmed by grep. `battery_measurement_api.h` is the clean hardware-backend interface (`battery_measurement_setup`, `battery_read_voltage`) both backends implement. |
| `read_battery_data(struct battery_data*)`, `get_batt_v_str(void)`, `get_batt_pct_str(void)`, `log_battery_data(void)` | `battery.c` | **Generic — keep, callable as-is** | Thin wrappers around the hardware backend plus string formatting/logging. No Golioth types anywhere in these four. |
| `stream_battery_data(struct golioth_client *client, struct battery_data*)` | `battery.c` | **Golioth-transport-specific — do not port** | Takes `golioth_client*`, builds JSON, calls `golioth_stream_set_async(...)`. This is the transport call, structurally identical to the splice point already found in `app_sensors.c`. |
| `read_and_report_battery(struct golioth_client *client)` | `battery.c` | **Mixed — call site needs to change, not the library** | Orchestrates: `read_battery_data()` → format strings → `log_battery_data()` → *if connected*, `stream_battery_data()`. Only the last step is transport-specific, but the function's own signature is golioth-typed, so we can't keep calling it from non-Golioth code. Currently the only caller is `app_sensors.c`'s `IF_ENABLED(CONFIG_ALUDEL_BATTERY_MONITOR, (read_and_report_battery(client); ...))` block. Fix at the call site: call `read_battery_data()`, `get_batt_v_str()`, `get_batt_pct_str()`, `log_battery_data()` directly (all generic), then hand the values to the new MQTT publish function instead of calling `read_and_report_battery()` — same pattern as the vehicle-speed splice point. Note this path is currently gated behind `CONFIG_ALUDEL_BATTERY_MONITOR`, which in practice only applies to the Aludel board being removed — so this only matters if you want battery reporting on the nRF9160-DK too (would need its own `CONFIG_ALUDEL_BATTERY_MONITOR`-independent Kconfig gate and devicetree `vbatt`/`zephyr_user` ADC channel, not currently wired for that board). |

Both libraries remain in `west.yml` unchanged. No vendored files touched.

---

## Phase 2 complete — summary (2026-08-10)

Final build: `west build -p -b nrf9160dk/nrf9160/ns --sysbuild app` — **clean,
zero errors, zero warnings.** Signed image shrank from 390,680 B to
227,502 B (all CoAP/DTLS/RPC/CBOR transport code gone). This went further
than the original plan anticipated: rather than stopping once Settings/RPC/
Stream/LightDBState/Logging calls were TODO-commented (which was expected to
leave dangling symbol references and fail to *link*, per the task's own
Phase 2 step 7), every call site was fully resolved, so the build succeeds
end-to-end today with the transport simply absent — there's nothing left
for Phase 3 to "unbreak," only new code to wire in.

`grep -ric golioth` across `west.yml`/`prj.conf`/`src/` returns only:
copyright headers, TODO-comment prose, the retained `golioth_led` devicetree
alias + `golioth_connection_led_set()` GPIO helper (untouched — not a
Golioth API call, just a local LED-toggle function; out of scope, no
functional dependency on Golioth), and the `zephyr-network-info`/
`golioth-battery-monitor` project names/URLs kept per your explicit
decision. Zero actual `golioth_*` SDK function calls remain anywhere.

**Files changed:**

| File | Change |
|---|---|
| `west.yml` | Removed `golioth`, `golioth-zephyr-boards`, `libostentus` projects. Added direct `nrf` import replacing golioth's transitive one (verified byte-identical resolution — see above). `zephyr-network-info`/`golioth-battery-monitor` kept per your decision. |
| `prj.conf` | Removed all `CONFIG_GOLIOTH_*` (8 options) and the Golioth-CoAP/DTLS-sized dependency block (`CONFIG_ZVFS_*`, `CONFIG_COAP_EXTENDED_OPTIONS_LEN*`) that broke once golioth's Kconfig tree disappeared. Removed `CONFIG_GOLIOTH_SAMPLE_SETTINGS_AUTOLOAD`/`_SHELL` (the psk-id/psk shell commands). |
| `socs/nrf9160_ns.conf` | Disabled `CONFIG_NETWORK_INFO` — its own Kconfig help text says its purpose is "Return network information to a Golioth RPC or Log function," and its `CMakeLists.txt` compiles Golioth-RPC-typed and generic code as one inseparable unit (see the "Update" note above). |
| `src/main.c` | Removed Golioth client bootstrap (`start_golioth_client`, `on_client_event`, semaphore-gated connect), Golioth includes, non-nRF91x dead branch (depended on deleted `samples/common`). Kept: LTE connect trigger point, button/GPIO setup, `golioth_led` LED helper (unrelated to the transport), main loop. |
| `src/app_sensors.c` / `.h` | Removed `struct golioth_client*`, `app_sensors_set_client()`, the stream-publish call (TODO'd at the exact splice point). Battery block rewritten to call `read_battery_data()`/`log_battery_data()` directly instead of the Golioth-typed `read_and_report_battery()`. CAN/GPS data-production logic untouched. Ostentus slide code removed (dead for this board anyway, but now also not referencing a deleted library). |
| `src/app_rpc.c` / `.h` | Golioth RPC handlers removed (irreducibly Golioth-typed signatures); reboot work-queue kept as reusable dead code for future MQTT command wiring. `app_rpc_register()` is now a no-op, no-arg stub. |
| `src/app_settings.c` / `.h` | Golioth Settings Service callbacks + registration removed; all 6 config getters (loop/GPS/vehicle-speed delays, fake-GPS) untouched and still return compiled-in defaults — no runtime reconfiguration until Phase 3. |
| `src/app_state.c` / `.h` | Entire LightDB State "digital twin" demo gutted to a no-op stub — audit confirmed zero real vehicle data flowed through it. |
| `boards/aludel_elixir_ns.conf`, `boards/aludel_elixir_ns.overlay` | Deleted — Aludel-Mini/Elixir board files, unusable once `golioth-zephyr-boards` was removed. |
| `pipelines/json-to-lightdb.yml` | Deleted with the whole `pipelines/` directory. |

**Known follow-up, out of Phase 2's scope:** `.github/workflows/test.yml`
and `release.yml` still reference the `aludel_elixir/nrf9160/ns` board
target, which will fail if that CI runs (the board no longer resolves). Not
touched since Phase 2 only covered `src/`/`boards/`/`west.yml`/`prj.conf`.

---

## Phase 3 complete — MQTT/TLS uplink (2026-08-11)

TLS approach: **mutual TLS (client certificate)**, per your explicit choice.

**Research before writing any code** (two research passes, both grounded in
this workspace's actual checked-out `deps/zephyr`/`deps/nrf` source, not
assumed API behavior — see citations below):

1. `zephyr/net/mqtt.h` / `tls_credentials.h` API surface, and — the load-
   bearing discovery — **nRF9160 offloads TLS to the modem**, so
   `tls_credential_add()`/`CONFIG_MBEDTLS`/`CONFIG_TLS_CREDENTIALS` (my
   original guess in the task's own Phase 3 step 1 wording) are the *wrong*
   mechanism for this hardware. The real path is
   `modem_key_mgmt_write()` (`deps/nrf/include/modem/modem_key_mgmt.h`,
   `CONFIG_MODEM_KEY_MGMT`), confirmed against
   `deps/nrf/samples/net/mqtt/src/modules/transport/credentials_provision/credentials_provision.c`
   and `deps/nrf/samples/net/mqtt/overlay-tls-nrf91.conf` (which sets no
   `CONFIG_MBEDTLS*` options at all). `mqtt_helper.h` (NCS's convenience
   MQTT wrapper) was ruled out — it has no Last-Will-and-Testament support,
   so `uplink_mqtt.c` uses `struct mqtt_client` from `zephyr/net/mqtt.h`
   directly. Neither in-tree sample implements exponential backoff (both
   use a fixed reconnect delay) — that part is new code, not adapted from a
   reference.
2. Partition Manager mechanics for the new `telemetry_fifo` flash region:
   confirmed `pm_static.yml` entries become `PM_<NAME>_*` macros (in
   generated `pm_config.h`) resolved via `flash_area_open()`, **not** a
   devicetree `fixed-partitions` node — verified by finding a real address
   collision between PM's `settings_storage` (`0xf8000`/`0x2000`) and an
   unrelated devicetree-only `storage_partition` node
   (`0xf8000`/`0x8000`) already present in this SoC's default `.dts`,
   proving the two mechanisms are unsynced in this NCS version. Real NVS
   mount pattern taken from `deps/zephyr/samples/subsys/nvs/src/main.c`.

**New files:**

| File | Purpose |
|---|---|
| `src/uplink_mqtt.h`/`.c` | MQTT/TLS client: connect/reconnect with exponential backoff (1s → 60s cap, reset on success), the three topics (`telemetry` QoS0, `events` QoS1, `status` QoS1 retained + LWT "offline"), auto-publishes `{"state":"online"}` on every (re)connect, drains `buffer_fifo` in order before resuming live publishing. |
| `src/uplink_mqtt_certs.h` | Placeholder CA/client-cert/private-key PEM strings — **must be replaced with real certificates before flashing**; clearly TODO-marked. |
| `src/buffer_fifo.h`/`.c` | Flash-backed FIFO on Zephyr NVS (`telemetry_fifo` partition, 6×4KB sectors), 32-record ring buffer, drops oldest on overflow, peek/pop-ack pattern so a failed publish mid-drain doesn't lose the record. |

**Files changed:** `app/Kconfig` gained a menu for broker hostname/port/
sec-tag/device-id (all deployment-specific, all empty/placeholder
defaults); `prj.conf` gained the MQTT/modem-key-mgmt Kconfig block;
`pm_static.yml`'s `EMPTY_1` renamed to `telemetry_fifo`; `CMakeLists.txt`
gained the two new source files; `main.c` calls `uplink_mqtt_init()` before
LTE comes up (required — `modem_key_mgmt_write()` fails with `-EPERM` once
the link is active) and `uplink_mqtt_start()` from `lte_handler()` once LTE
registers; `app_sensors.c`'s two splice points now call
`uplink_mqtt_publish_telemetry()` directly (both the main GPS/CAN record
and the battery block, which builds the same JSON shape
`golioth-battery-monitor`'s own `stream_battery_data()` used).

**Build: clean pristine build, zero errors/warnings**, full sysbuild +
signing (`dfu_application.zip`, `merged.hex`). `grep -ric golioth` across
`west.yml`/`prj.conf`/`src/` returns only comments/copyright headers and
the pre-existing, out-of-scope `golioth_led` GPIO alias +
`golioth_connection_led_set()` helper (a plain LED toggle, not an SDK call)
— zero actual `golioth_*` function calls remain anywhere.

**Known gaps, deliberately out of Phase 3's scope:**
- Certificates are placeholders (`uplink_mqtt_certs.h`) — real PEM content
  needed before this connects to anything.
- `CONFIG_UPLINK_MQTT_BROKER_HOSTNAME` defaults to empty — must be set
  (e.g. in a board/deployment conf overlay) before `uplink_mqtt_init()` can
  resolve a broker.
- No command/subscribe topic exists yet — `app_rpc.c`'s reboot/log-level/
  network-info logic is still just dead code behind a no-op
  `app_rpc_register()`, per the plan (Phase 3's topic list has no
  "commands" topic).

### Post-review fixes (2026-08-11)

Three issues raised in review, all fixed and rebuilt clean:

1. **`CONFIG_NET_SOCKETS_TLS_PRIORITY=35` in `socs/nrf9160_ns.conf`** — a
   leftover from Golioth's CoAP/DTLS stack that forces native/software TLS
   instead of modem-offloaded TLS. Per
   `deps/zephyr/subsys/net/lib/sockets/Kconfig`:
   `NET_SOCKETS_OFFLOAD_PRIORITY` defaults to 40, `NET_SOCKETS_TLS_PRIORITY`
   defaults to 45 (40 < 45 ⇒ offloaded/modem TLS wins). The override dropped
   it to 35 (40 > 35 ⇒ native TLS wins instead), which would have silently
   looked for credentials via `tls_credential_add()`'s volatile store —
   never populated, since `uplink_mqtt.c` provisions credentials into the
   *modem's* storage via `modem_key_mgmt_write()`. This would have produced
   a TLS handshake failure with no useful diagnostic. Removed; both
   priorities are back at their Kconfig defaults.
2. **Hostname verification on modem-offloaded TLS** — confirmed from source
   that `TLS_HOSTNAME` → `NRF_SO_SEC_HOSTNAME` and `TLS_PEER_VERIFY_REQUIRED`
   → `NRF_SO_SEC_PEER_VERIFY` (`deps/nrf/lib/nrf_modem_lib/nrf9x_sockets.c`)
   both reach the modem's own TLS socket implementation. That's the limit of
   what's verifiable from this checked-out source, though — the actual
   verification algorithm runs in closed-source modem firmware, not
   anywhere in this tree. Documented in `uplink_mqtt.c` at the TLS config
   site with a note to confirm against your modem firmware's release notes
   and to make sure `CONFIG_UPLINK_MQTT_BROKER_HOSTNAME` exactly matches the
   broker certificate's CN/SAN.
3. **Split `buffer_fifo` into two independent channels** — the original
   single shared ring meant a burst of disposable QoS0 telemetry could
   evict an unacknowledged QoS1 event (DTC/harsh-driving data) before it
   was ever published, defeating the point of QoS1. `buffer_fifo.h`/`.c`
   now expose `BUFFER_FIFO_CHANNEL_TELEMETRY` (24 slots, drops oldest on
   overflow — telemetry is disposable) and `BUFFER_FIFO_CHANNEL_EVENTS` (48
   slots, *rejects* new pushes on overflow rather than evicting — logs an
   error and returns `-ENOSPC` instead). Both channels share the same NVS
   mount on the `telemetry_fifo` partition via distinct id ranges (no
   partition resize needed). `uplink_mqtt.c` simplified alongside this: the
   old cross-channel envelope/tag (`struct queued_record` with a
   `queued_topic_kind` byte) is gone, since each channel is now already
   topic-specific — buffered records store raw payload bytes directly.
   `drain_buffered_records()` drains events before telemetry on reconnect
   (lower volume, higher stakes).

**Update (Phase 2 step 2 build):** `CONFIG_NETWORK_INFO` (set in
`socs/nrf9160_ns.conf`) had to be disabled — its `CMakeLists.txt` compiles
`network_info_modem_info.c`/`network_info_placeholder.c` unconditionally
together, and both hard-`#include <golioth/rpc.h>` for
`network_info_add_to_map()`'s return type, regardless of whether anything
calls that function. Once the Golioth SDK was removed (step 1), this broke
the build even though nothing in `src/` calls into the library anymore. Its
own Kconfig help text is explicit that the option's whole purpose is "Return
network information to a Golioth RPC or Log function" — so disabling it,
rather than patching the vendored library, is the correct minimal fix. No
equivalent problem exists for `golioth-battery-monitor`, since its two
translation units are already split by concern (`battery.c` has the
Golioth-typed `stream_battery_data`, but no `#include <golioth/...>` sits in
the same file as anything CONFIG-gated the way network-info's does).

---

## Phase 4 complete — full PID + DTC coverage (2026-08-11)

**Note:** Phase 3.5 (native_sim + vcan0 host-side testing) was attempted
first, per the plan's stated dependency ("every OBD/DTC change in Phase 4
... should be verified against this native_sim + vcan0 loop first"), but
hit a hard blocker — this host is macOS, and Zephyr's own build system
refuses outright: *"The POSIX architecture only works on Linux."* The
`zephyr,native-linux-can` driver Phase 3.5 needs wraps Linux SocketCAN
directly, which doesn't exist on macOS at all — no code-level workaround.
`boards/native_sim.overlay` and `TESTING.md` are written and ready for the
moment this runs on Linux, but unverified. Per your direction, Phase 4
proceeded on the nRF9160-DK build target instead (still compiles/links
clean; not yet runnable/testable without either real hardware or that
Linux host).

**New files:**

| File | Purpose |
|---|---|
| `src/obd_j1979.h`/`.c` | Mode 01 (current data) PID request/response state machine — vehicle speed (`0x0D`), engine RPM (`0x0C`), fuel level (`0x2F`), coolant temp (`0x05`). Took over CAN device init/RX filter/polling thread from `app_sensors.c`'s old single-PID-only thread (removed — see below) rather than running a second, competing filter on the same response ID. Also drives a Mode 03 DTC poll each cycle via `dtc_decode.c`. |
| `src/dtc_decode.h`/`.c` | SAE J1979 Mode 03 (read stored DTCs) codec — pure request-build/response-parse logic, no CAN device access of its own. **Read-only by design**: does not implement Mode 04 (clear DTCs) or any CAN-write/actuation capability, per project scope. **Scope limit, stated in the header, not silently overclaimed**: parses only a single-frame response (up to 3 DTCs); a vehicle with more stored faults than fit in one 8-byte frame would need ISO 15765-2 multi-frame reassembly, which isn't implemented. |

**Files changed:**
- `src/app_sensors.c` — removed `process_can_frames_thread()`, `g_vehicle_speed`, and the CAN device/filter/msgq it owned entirely (superseded by `obd_j1979.c`, not duplicated — running two filters against the same `0x7E8` response ID would have raced). `process_rmc_frames_thread()` now calls `obd_j1979_get_latest()` to pair each GPS fix with the latest PID readings, same "-1 = unknown" convention as before. `app_sensors_init()` no longer starts the CAN controller (moved to `obd_j1979_init()`, called separately from `main.c`). Telemetry JSON extended with `rpm`/`fuel_level`/`coolant_temp` alongside the existing `speed`. DTCs published as a new event payload — but only when the DTC set actually changes since the last publish, not every cycle, to avoid spamming the same fault codes.
- `src/main.c`, `CMakeLists.txt` — wire `obd_j1979_init()` in, register the two new source files.

**Build: clean pristine build, zero compiler diagnostics**, full sysbuild + signing.

**Open item — JSON schema not yet confirmed.** Per the task's own instruction, asked you to paste the exact PRD telemetry/event JSON schema rather than guess it; not yet received. Implemented with reasonable placeholder key names in the meantime (`"vehicle":{"speed","rpm","fuel_level","coolant_temp"}` for telemetry, `{"dtc_codes":["P0301",...]}` for DTC events — both marked `TODO(confirm-json-schema)` at their call sites in `app_sensors.c`) so Phase 4's protocol logic (the actual hard part — PID decode formulas, DTC bit-encoding) wasn't blocked waiting on it. Adjust the two `JSON_FMT`/`JSON_FMT_FAKE_GPS` macros and the DTC event's `snprintk` calls once the real schema is available.

---

## JSON schema confirmed and applied (2026-08-11)

Telemetry `"vehicle"` object renamed to `"obd"` with exact field names/types
from your schema: `vehicle_speed_kmh`/`fuel_level_pct` as floats (`%.1f`),
`engine_rpm`/`coolant_temp_c` as ints. `fuel_level_pct` changed from `int` to
`float` in `obd_j1979_data` (`(resp[3] * 100.0f) / 255.0f`) to actually carry
the fractional precision the schema's `47.5` example implies — the old
integer-division version would have always produced whole percentages.
`vehicle_speed_kmh` stays internally `int` (PID `0x0D` is a whole-byte
km/h value with no real fractional meaning) but is cast to `double` for the
`%.1f` format.

**Assumption, not explicitly stated in your schema fragment**: the `"obd"`
object replaces the old `"vehicle"` key with the top-level `"time"`/`"gps"`
siblings unchanged (you pasted only the `"obd"` sub-object) — flag if that's
wrong.

DTC event rebuilt to your exact envelope (`device_id`/`timestamp`/
`event_type`/`detail`/`buffered`) and, per your explicit instruction, changed
from one batched event per poll to **one event per newly-seen code**:
`app_sensors.c` now tracks a per-session "already reported" set
(`reported_dtc_codes[]`, up to 16 codes) and fires exactly once per code,
never re-firing a code that clears and later reappears. `device_id` comes
from a new `uplink_mqtt_get_device_id()` getter; `timestamp` reuses the most
recent valid GPS fix's UTC time (`last_known_utc_time`, now exposed via
`app_sensors_get_last_known_time()` since Phase 5's harsh-driving events use
the same value) — sentinel `"1970-01-01T00:00:00Z"` until the first fix
arrives, since there's no RTC hardware in this design to fall back to.
`buffered` is a best-effort snapshot of `uplink_mqtt_is_connected()` taken
right before publish — not perfectly atomic with the actual publish
decision inside `uplink_mqtt_publish_event()`, documented as a known race
window in the code comment.

Rebuilt clean, zero compiler diagnostics.

---

## Phase 5 complete — IMU harsh-driving detection (2026-08-11)

Part: **ICM-42688-P**, confirmed against Zephyr's mainline driver support
before writing any devicetree (`deps/zephyr/drivers/sensor/tdk/icm42688`,
compatible `invensense,icm42688`) — SPI-only, no I2C backend in this
driver, added as a second device on the existing `&arduino_spi` bus (second
`cs-gpios` entry, D9/`gpio0 9`) alongside the MCP2515. All devicetree
property/macro names (`accel-odr`, `accel-fs`, `ICM42688_DT_ACCEL_LN`, etc.)
verified against the actual binding YAML and `dt-bindings/sensor/icm42688.h`
before use — confirmed the ICM-42688-P node correctly resolves onto
`spi@b000/icm42688@1` in the final build's `zephyr.dts`.

**New files, split per your explicit instruction** (threshold math kept
separate from IMU driver I/O, specifically so the math is unit-testable
without the native_sim/CAN bridge working):

| File | Purpose |
|---|---|
| `src/harsh_driving.h`/`.c` | Pure threshold-math logic — classifies one accelerometer sample into acceleration/braking/cornering/none. **Zero Zephyr or driver includes** (just `<math.h>`) — this isn't just "logically separate," it's a genuinely standalone translation unit that could run under `west twister --platform unit_testing` or a bare host compile, no Zephyr build involved at all. Default thresholds are placeholder starting values (±3.5-4.0 m/s²), not derived from any vehicle spec — need real-data tuning later. |
| `src/imu_icm42688.h`/`.c` | Sensor I/O: polls the ICM-42688-P via the generic Sensor API (`sensor_sample_fetch()`/`sensor_channel_get(SENSOR_CHAN_ACCEL_XYZ)`, `sensor_value_to_float()`) at 100ms, no interrupt/trigger mode (simpler, this workload doesn't need trigger-mode latency). Calls `harsh_driving_evaluate()`, and on a non-`NONE` result publishes a `harsh_driving_detected` event through `uplink_mqtt_publish_event()` using the same event envelope shape as the DTC event (extended to include `timestamp`, reusing `app_sensors_get_last_known_time()`, for consistency — not explicitly specified for this event type in your schema message, flagging the extension). |

**Files changed:** `app/boards/nrf9160dk_nrf9160_ns.overlay` (new SPI CS +
`icm42688` node), `app/prj.conf` (`CONFIG_SENSOR`, `CONFIG_ICM42688`),
`app/CMakeLists.txt`, `src/main.c` (`imu_icm42688_init()` call),
`src/app_sensors.h`/`.c` (new `app_sensors_get_last_known_time()` getter).

**Build: clean pristine build, zero compiler diagnostics**, full sysbuild +
signing.

**Known limitations, stated up front rather than glossed over:**
- Fixed axis convention (X=longitudinal, Y=lateral), no runtime calibration
  for actual device mounting orientation — stated in `harsh_driving.h`.
- Thresholds are placeholders pending real driving data.
- Entirely unverified at runtime — see `ISSUES.md`'s Phase 5 section, now
  filled in with specifics (this was tracked as a running list per your
  request, not scattered across comments).

---

## Phase 6 complete — OTA update, replacing Golioth's package/cohort/deployment system (2026-08-11)

### Step 1 — baseline confirmed untouched

Checked, not assumed: `git diff` against the original checkout shows
`sysbuild.conf` (`SB_CONFIG_BOOTLOADER_MCUBOOT=y`) has **zero changes**
since Phase 1, and `pm_static.yml`'s only change across all of Phases 1-6
is the Phase 3 `EMPTY_1`→`telemetry_fifo` rename (same address/size) — no
`mcuboot_primary`/`mcuboot_secondary`/`mcuboot_pad`/`app`/`tfm` partition
boundary was ever touched. `grep -ril golioth build/` across the *entire*
build tree (app + mcuboot + tfm images) returns only the pre-existing,
already-documented cosmetic `golioth-led` devicetree alias — zero Golioth
glue anywhere between MCUboot/TF-M and the app, confirming the sysbuild
23-step mcuboot+tfm+app build was never Golioth-coupled to begin with.

### Step 2 — research before implementing

Confirmed `fota_download` is wired into this checkout's Kconfig tree
already (`deps/nrf/subsys/net/lib/Kconfig` sources it) and read
`deps/nrf/samples/cellular/http_update/application_update` end-to-end
before writing any code. Key findings that shaped the implementation:

- `fota_download_start_with_image_type()` already wraps the *entire*
  download → `dfu_target_write()` → `dfu_target_done()` →
  `dfu_target_schedule_update()` pipeline internally (traced in
  `fota_download.c`'s `DOWNLOADER_EVT_DONE` handler) — `ota_update.c` does
  **not** call `dfu_target`/MCUboot image-manager APIs directly, exactly
  per the ground rule to not hand-roll what these libraries already do.
- The scheduled swap is always a MCUboot **test** swap
  (`boot_request_upgrade_multi(img_num, BOOT_UPGRADE_TEST)`, never
  `PERMANENT`) — confirming the hard constraint ("signature verification
  must still happen before any image is marked bootable") was never at
  risk of being weakened, since MCUboot's own signature check runs before
  it will even boot a test-swapped image, exactly as it does for a
  locally-flashed image.
- `fota_download` does **not** reboot the device itself, and MCUboot does
  **not** auto-confirm a successful test-swap as permanent — both are
  explicit app responsibilities (`sys_reboot()` after
  `FOTA_DOWNLOAD_EVT_FINISHED`; `boot_write_img_confirmed()` on next boot).
  Both wired in (`ota_update.c`, `main.c`).
- CA cert provisioning uses the exact same `modem_key_mgmt_write()`
  mechanism `uplink_mqtt.c` already uses — confirmed via the sample's own
  `cert_provision()`, not assumed by analogy.

### Step 3 — MQTT trigger, HTTP(S) transfer

`src/uplink_mqtt.c` gained subscribe capability: `trackers/{device_id}/ota`
(QoS 1) is subscribed on every connect (`subscribe_ota_topic()`), and a new
`MQTT_EVT_PUBLISH` case reads the payload (`mqtt_read_publish_payload_blocking()`),
routes it to a registered handler (`uplink_mqtt_set_ota_handler()`), and
acks QoS 1 messages (`mqtt_publish_qos1_ack()`) — none of this existed
before; `uplink_mqtt.c` only published previously.

`src/ota_update.c` (new) is the registered handler: parses the small JSON
pointer (`{"version":"...","url":"..."}`, via `zephyr/data/json.h`, same
library `app_state.c` used before it was removed in Phase 2), splits the
URL into host/path, and calls
`fota_download_start_with_image_type(..., DFU_TARGET_IMAGE_TYPE_MCUBOOT)`.
Two networks, two jobs, exactly as specified — MQTT never carries the
firmware image itself.

### Step 4 — server hostname + CA/sec-tag decision (flagged as asked)

`CONFIG_FOTA_UPDATE_SERVER_HOSTNAME` added (`app/Kconfig`), empty default,
same pattern as `CONFIG_UPLINK_MQTT_BROKER_HOSTNAME`. **One synthesis I
made that wasn't fully specified, flagged rather than silently decided**:
your step 3 said the MQTT message carries "a download URL," but step 4
asked for a configured server hostname "same pattern as
`CONFIG_UPLINK_MQTT_BROKER_HOSTNAME`" (a single trusted target, not a
per-message value) — I reconciled these by keeping "url" as a full URL in
the MQTT payload, but having `ota_update.c` **require** its host to match
`CONFIG_FOTA_UPDATE_SERVER_HOSTNAME` when that's configured (rejecting
otherwise), so a compromised/malicious publish on the OTA topic can't
redirect the device to an arbitrary host. While `FOTA_UPDATE_SERVER_HOSTNAME`
is still empty (its "placeholder" state), validation is skipped with a
loud `LOG_WRN` rather than blocking all testing — reconsider whether you
want this the other way once you've stood up real hosting.

**CA/sec-tag decision, explicitly flagged rather than silently picked**:
implemented a **separate** `CONFIG_FOTA_UPDATE_SEC_TAG` (distinct default
value from `CONFIG_UPLINK_MQTT_SEC_TAG`), CA-only (server-authenticated
HTTPS, not mutual TLS like the MQTT broker connection) — matching NCS's
own sample's pattern (a dedicated `TLS_SEC_TAG`, not reuse). Reasoning:
research confirmed sec_tags are freely reusable across independent TLS
connections (nothing in `modem_key_mgmt.h`/`nrf_modem`'s docs restricts
this), so reusing `CONFIG_UPLINK_MQTT_SEC_TAG` was technically legal, but
would present the MQTT broker's mTLS **client certificate** to whatever
host serves firmware images — harmless if that host ignores it, but
semantically muddled, and blocks using a firmware host with a different
(e.g. public) CA than your MQTT broker's likely-private one. If you want
to reuse the MQTT sec_tag anyway (same CA, no separate provisioning
step), that's a one-line change in `ota_update.c`'s `fota_download_start_with_image_type()`
call — flagging the choice, not the difficulty of changing it.

### Step 5 — signing key: confirmed, not re-implemented (no new signing code)

`dfu_application.zip` (already produced by the existing sysbuild + MCUboot
signing step, unchanged by any of this) contains `app.signed.bin` +
`manifest.json`. Confirmed **byte-for-byte identical**
(`diff` against `build/app/zephyr/zephyr.signed.bin`) — same file, same
signature, same key MCUboot already trusts, just packaged two ways. **What
needs uploading to the update server is one of these two identical
files** (`zephyr.signed.bin` directly, or `app.signed.bin` unzipped from
`dfu_application.zip` — not the `.zip` itself, since
`fota_download_start_with_image_type()` fetches a bare binary stream over
HTTP(S), not an archive). No new signing logic was written or needed —
this is a publishing-process fact, not a code change.

### Step 6 — disclosure discipline, hardware test recommendation

Added to `ISSUES.md`'s new Phase 6 section, flagged as the one thing in
this project that most needs a *real* hardware test before trusting it
(your framing, and correct) — "does the device still boot" isn't proven by
a clean compile the way CAN/MQTT/IMU logic-correctness roughly is.
Documented a concrete 4-step real-hardware test sequence there (flash
baseline → bump version, host the signed bin, publish a real OTA MQTT
message → confirm `FOTA_DOWNLOAD_EVT_FINISHED` + reboot + new
`fw_version` in telemetry → don't trust it until that's actually been done
once). Native_sim verification remains blocked for the same macOS/Linux
reason as Phases 3-5.

**Build: clean pristine build, zero compiler diagnostics**, full sysbuild +
signing (`dfu_application.zip` present and verified). One real bug caught
and fixed during this phase, worth noting since it's a good example of why
the "rebuild after every step" discipline matters: `CONFIG_FOTA_DOWNLOAD=y`
alone produced a **link** error (`undefined reference to
'fota_download_init'`), not a Kconfig warning — its dependency
`CONFIG_DFU_TARGET` has no default (only `DFU_TARGET_MCUBOOT`, its child,
defaults to `y`, but only once `DFU_TARGET` itself is already on), so the
whole `fota_download` library silently never got compiled until
`CONFIG_DFU_TARGET=y` was added explicitly.

## Workspace hygiene, attribution, and README pass (post-Phase-6)

Housekeeping across the whole workspace, not gated on any new phase —
done so the tree is in a state that can actually be pushed to a personal
GitHub repo correctly. Findings below are what was actually on disk,
not assumed from standard west-workspace conventions.

### Step 1 — real workspace structure

Top level of `~/Downloads/alpha/obd-tracker/` (the manifest `west init`
was run in — not literally `~/obd-tracker`, noting the actual path since
it differs from the original setup transcript above after the mid-project
move):

| Dir | Size | What it actually is |
|---|---|---|
| `.venv` | 402M | Python virtualenv for `west`/toolchain scripts. Not part of the repo. |
| `.west` | 4.0K | West's own bookkeeping (`.west/config` — one file, points `manifest.path` at `app`). Not part of the repo. |
| `app` | 7.4M | **The actual project — this is the one real git repo, `origin` = `golioth/reference-design-can-asset-tracker.git`.** |
| `build` | 97M | sysbuild output (app + mcuboot + net-tools staging etc). Build artifact, not source. |
| `deps` | ~2.9G (was 3.3G before cleanup) | West-managed vendor checkouts: `nrf`, `zephyr`, `nrfxlib`, `bootloader/mcuboot`, `tools/net-tools`, and the transitively-imported `modules/*` tree (mbedtls, tf-m, hal_nordic, cmsis_6, zcbor, tinycrypt, segger, qcbor, oberon-psa-crypto). All real, independent git checkouts (see `.git` table below) — none of these are mine, all are fetched by `west update` per `app/west.yml`. |
| `modules` | 0 (emptied, then removed) | Was **not** a normal west-managed location for this manifest — turned out to hold a single stale leftover, see Step 2. |

`find ~/Downloads/alpha/obd-tracker -maxdepth 3 -name .git -type d` (before cleanup) returned, in addition to `app/.git`:
`deps/nrf`, `deps/nrfxlib`, `deps/zephyr`, `deps/tools/net-tools`,
`deps/bootloader/mcuboot`, `deps/modules/{crypto/mbedtls,
crypto/oberon-psa-crypto, crypto/tinycrypt, hal/nordic, hal/cmsis_6,
lib/battery-monitor, lib/zcbor, lib/network-info, debug/segger,
tee/tf-m/trusted-firmware-m, tee/tf-m/qcbor}`, plus two more that turned
out to be stale rather than legitimate — `modules/lib/golioth-firmware-sdk`
and `deps/deps/nrf` (both covered under Step 2). All the legitimate ones
are exactly what `west list` currently resolves (verified — see below);
each is a real, independent git clone (west's normal behavior for
git-type manifest projects), correctly not-mine, and correctly outside
`app/`'s repo boundary.

One more real vendored checkout worth calling out specifically because of
*where* it lands: `minmea` (`app/src/lib/minmea`, from
`kosma/minmea.git`) resolves **inside** `app/`'s own working tree per
`west.yml`'s `path: app/src/lib/minmea`, and does have its own `.git`
(`app/src/lib/minmea/.git`). Checked whether this creates a nested-repo/
gitlink problem for `app`'s own history: it doesn't — `app/.gitignore`
already has an explicit `src/lib/minmea` entry (upstream, not something I
added), confirmed via `git check-ignore -v src/lib/minmea` inside `app/`.
`git ls-files` inside `app` confirms none of minmea's files are tracked by
`app`'s repo. This is intentional, working as designed: minmea is fetched
fresh by `west update` on any machine that clones `app` and runs west
against it, not meant to be committed into `app`'s own history.

`build/` lands as a **sibling of `app/` at the workspace root**
(confirmed: `ls ~/Downloads/alpha/obd-tracker/app/build` → no such file;
`build/` only exists at the workspace root). It is entirely outside
`app/`'s repo boundary already — no `.gitignore` entry is *required* for
it, though `app/.gitignore` already carries a `build*/` entry upstream as
a defensive no-op (matters only if someone runs `west build` with CWD
inside `app/` instead of the workspace root, which would then nest a
`build/` inside `app/`) — kept as-is, harmless and technically correct
insurance rather than a currently-active exclusion.

**Answer to "is `app/` correctly the one and only directory that should
become the GitHub repo?": yes**, confirmed by direct inspection, not
assumed. Nothing on disk contradicts this once the two stale directories
below are removed.

### Step 2 — Golioth-named directory audit vs. current manifest

`app/west.yml` (post-Phase-2) currently declares exactly two
Golioth-named projects, both under `deps/modules/lib/`. Cross-referencing
every Golioth-named directory found on disk against this:

| Directory | Manifest status | Classification | Notes |
|---|---|---|---|
| `deps/modules/lib/network-info` (`zephyr-network-info`) | Declared, `revision: v1.2.0` | **Still declared, still fetched — but consumer disabled.** | Kept per your explicit Phase 2 call (function-level split: generic query/format logic vs. Golioth-RPC-typed glue). Currently `CONFIG_NETWORK_INFO=n` in `socs/nrf9160_ns.conf` (Phase 2), so none of this code is actually compiled into the app right now — it's fetched and present on disk, correctly tracked by the manifest, just dormant. Confirmed against `AUDIT.md`'s own Phase 2 section (§"`zephyr-network-info` / `golioth-battery-monitor` — function-level split"), not re-derived from scratch. |
| `deps/modules/lib/battery-monitor` (`golioth-battery-monitor`) | Declared, `revision: v1.1.0` | **Still declared, still fetched — partially used.** | Kept per the same Phase 2 decision. Its generic functions (`read_battery_data()`, `get_batt_v_str()`, `get_batt_pct_str()`, `log_battery_data()`) are called from `app_sensors.c`, gated behind `CONFIG_ALUDEL_BATTERY_MONITOR` — off by default for the nRF9160-DK target (Aludel-specific, and Aludel board support was removed in Phase 2), so this path is currently dormant on the actual target board too, but the library itself is legitimately still a dependency. |
| `modules/lib/golioth-firmware-sdk` | **Not in manifest at all** (removed Phase 2) | **Stale — confirmed and deleted.** | This was the actual golioth-firmware-sdk vendor checkout (60M, its own independent `.git`, zero uncommitted changes — pure untouched vendor code). `west update` doesn't delete a project's checkout when the project is removed from the manifest, so this sat on disk since Phase 2 despite no longer being reachable from `west.yml`. Deleted, then verified via a full `west update` run: it did **not** reappear, and every project `west update` touched matched exactly the 15 non-`app` entries `west list` currently reports. This is the actual proof the Phase 2 manifest edit is self-sufficient — not merely "looks right," but confirmed non-dependent on this leftover silently still being present. |

**Additional finding, outside the Golioth-specific scope of this step but
the same category of problem and too significant (369M) not to report
here**: `deps/deps/nrf` — a second, complete, stale checkout of the `nrf`
project at the *old* (buggy) resolved path from the `import.path-prefix`
double-application bug documented earlier in this file (Phase 2's west.yml
fix section). The manifest fix (`path: nrf` bare, letting `path-prefix:
deps` add the `deps/` itself) was correct and was verified by diff at the
time, but the *previous*, buggy resolution had evidently already been
checked out to disk before the fix and was never cleaned up — `west
update` only reads the current manifest going forward, it doesn't notice
or remove a project's now-orphaned prior path. Confirmed orphaned via
`west list` (current resolution is `deps/nrf`, not `deps/deps/nrf`) and
via `git status --short` inside it (clean, no uncommitted changes — pure
vendor code). Deleted alongside the golioth-firmware-sdk leftover, and
covered by the same `west update` re-verification: did not reappear.
Both empty parent directories (`modules/lib`, `modules`, `deps/deps`) were
removed afterward since nothing else used them.

### Step 3 — `.gitignore` (`app/.gitignore`)

Upstream already ships a `.gitignore` in `app/` with `build*/`, `.vscode`,
`.cache`, `credentials.conf`, `__pycache__/`, and `src/lib/minmea`. Given
Step 1's finding that `build/` lives entirely outside `app/`'s boundary and
`.venv` is never nested inside `app/` (confirmed: `find app -iname
"*.venv*"` — no results) and no stray `.pyc`/`__pycache__` exist under
`app/` currently, **no new entries were needed** — the existing file
already covers everything Step 1 actually found, including the one
genuinely-relevant case (`build*/` as insurance against running `west
build` with the wrong CWD). Confirmed `.vscode` isn't tracked upstream
(`git ls-files | grep -i vscode` → empty, and no `.vscode/` directory
exists on disk) — so keeping it in `.gitignore` is correct as-is, not a
case of ignoring something deliberately shared. No edits made to
`.gitignore`.

### Step 4 — License and attribution

- **NOTICE**: upstream `golioth/reference-design-can-asset-tracker` has no
  `NOTICE` file — confirmed both by its absence on disk and
  `git log --all --full-history -- NOTICE` returning nothing (never
  existed in this repo's history either). Apache-2.0 §4(d) only obligates
  carrying forward an *existing* NOTICE's content; since none exists
  upstream, none is required here. No `NOTICE` file created.
- **LICENSE**: present, full Apache-2.0 text, untouched (`head -5`
  confirms it's the standard license preamble). Left as-is.
- **Per-file copyright headers**: spot-checked every file edited across
  Phases 2-6 that originated upstream (`src/app_sensors.c`,
  `src/app_rpc.c`, `src/app_settings.c`, `src/app_state.c`, `src/main.c`,
  `CMakeLists.txt`, `Kconfig`, `prj.conf`) — all still carry their
  original `Copyright (c) 202x, Golioth, Inc.` / `SPDX-License-Identifier:
  Apache-2.0` header, confirming the "preserve history, edit in place"
  ground rule was actually followed, not just assumed. New files written
  this project (`uplink_mqtt.c`/`.h`, `buffer_fifo.c`/`.h`,
  `obd_j1979.c`/`.h`, `dtc_decode.c`/`.h`, `harsh_driving.c`/`.h`,
  `imu_icm42688.c`/`.h`, `ota_update.c`/`.h`, `ota_update_certs.h`) carry
  a bare `SPDX-License-Identifier: Apache-2.0` line without a Golioth
  copyright notice, since they contain no upstream code — correct as-is.

**One dead-code finding surfaced during this pass, unrelated to
license/attribution but caught while reading these files closely**:
`src/json_helper.h` (a `struct app_state`/`app_state_descr` JSON
descriptor left over from the removed Golioth LightDB State demo) is
referenced by **nothing** — not even `app_state.c`, which is the file it
was seemingly written to support (`grep -rl "json_helper" src/` → zero
hits anywhere including `app_state.c` itself). This is different from
`app_state.c`/`.h` and `app_rpc.c`/`.h`, which are *intentional* no-op
stubs per Phase 2's documented decision (kept for their eventual MQTT
re-wiring, per their own TODO comments) — `json_helper.h` has no such
role and no caller at all. Deleted as pure dead weight; rebuild after
deletion confirmed clean (see Step 7).

### Step 6 — tracking-doc reconciliation

`TESTING.md` already existed (Phase 3.5, documenting the macOS/native_sim
blocker) — added the requested compile/native_sim/hardware verification
table to it rather than creating a second file (see `TESTING.md`).
`ISSUES.md` cross-checked against this pass's findings — no
contradictions found; the stale-directory and dead-file findings above
are workspace/build-tree facts, not module-verification-status facts, so
they didn't require changes there. `README.md` was fully rewritten (Step
5) directly from current source + this file + `ISSUES.md`, not from the
original project description.

### Step 7 — git state

`.gitignore` unchanged (Step 3 found no gap). Deleted `src/json_helper.h`.
Added `README.md` (full rewrite), `TESTING.md` update (verification
table). `AUDIT.md`/`ISSUES.md` updated. Rebuilt clean (see below), then
staged and committed **locally only** — no remote added, no push. `origin`
already points at `golioth/reference-design-can-asset-tracker.git`
(inherited from the original `west init -m`, never repointed) — flagging
this explicitly: it is not this project's repo, don't `git push origin`
against it. A personal remote needs to be added deliberately once a GitHub
repo URL exists.

## Docs centralization, CI fixes, and commit-message cleanup

Follow-up pass, same session as the hygiene pass above.

- **Moved `AUDIT.md`, `ISSUES.md`, `TESTING.md` into `docs/`** (`git mv`,
  history preserved) and added `docs/ARCHITECTURE.md` — a single-source-
  of-truth overview doc for any agent (or human) picking this project up
  cold: goal checklist, architecture diagram, module map, hard
  constraints, and a documentation map pointing to the other three docs.
  It's explicitly instructed (in its own text) to be kept current by
  whoever finishes the next unit of work — see its "Keeping this document
  current" section rather than duplicating that instruction here.
- **`README.md`** gained a "Documentation" section pointing at all four
  `docs/` files, a full Setup/Building/Testing walkthrough (previously
  just a terse two-command build block), and a CI build-status badge.
  All bare `AUDIT.md`/`ISSUES.md`/`TESTING.md` references elsewhere in
  `README.md` updated to `docs/...` paths to match the move.
- **CI workflows were stale and would have failed regardless of this
  pass** — found while checking whether a real test suite exists (it
  doesn't; `.github/workflows/test.yml` is a build/compile check only,
  no `tests/` directory, no twister unit tests, `sample.yaml` is just
  NCS's own sample-registration metadata). Two independent problems
  fixed:
  1. `test.yml` and `release.yml` both still had a job/matrix entry for
     `aludel_elixir/nrf9160/ns` — that board's `.conf`/`.overlay` were
     deleted in Phase 2 (Aludel Elixir support removed), so that job
     would fail on every run. Removed.
  2. `build_zephyr.yml`'s container reference
     (`golioth/golioth-zephyr-base:${ZEPHYR_SDK}-SDK-v0`) uses a tag
     format that no longer exists — checked the live Docker Hub tag
     list rather than assuming, and the image is now tagged
     `sdk-<version>` (only `sdk-0.17.4` and `sdk-1.0.1` currently
     published). Updated the container reference to the new tag format
     and bumped the default `ZEPHYR_SDK` input to `0.17.4` (all three
     workflow files). Note this doesn't exactly match this workspace's
     own resolved SDK version (`deps/zephyr/SDK_VERSION` = `0.17.1`) —
     `0.17.4` is the closest published container tag, not a byte-exact
     match. Flagging this rather than treating it as fully resolved: if
     CI behaves differently from a local build, this version skew is the
     first thing to check.
- **Commit history cleanup**: the two prior local commits
  ("Replace Golioth Cloud transport..." and "Workspace hygiene...") were
  rewritten (`git reset --soft` + recommit, safe since neither had been
  pushed anywhere) to drop the `Co-Authored-By` trailer, per your
  explicit request. This and all subsequent commits in this project
  omit that trailer.
