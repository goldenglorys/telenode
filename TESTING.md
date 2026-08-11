# Phase 3.5 — native_sim + vcan0 host-side testing

## Status: blocked on this host — requires a Linux environment

This development environment is **macOS (Darwin)**. Zephyr's POSIX
architecture (`native_sim`, which `boards/native_sim.overlay` targets) has a
hard, unconditional requirement for a Linux host, enforced directly by
Zephyr's own build system:

```
$ west build -p -b native_sim app
...
CMake Error at deps/zephyr/arch/posix/CMakeLists.txt:4 (message):
  The POSIX architecture only works on Linux.  If on Windows or macOS
  consider using a virtual machine to run a Linux guest.
```

This isn't a Kconfig/devicetree issue fixable in this repo — `native_sim`
compiles to a native host executable, and the `zephyr,native-linux-can`
driver it needs for CAN specifically wraps **Linux SocketCAN**
(`linux/can.h`, `AF_CAN`/`PF_CAN`, `vcan` kernel module) — none of which
exist on macOS. Confirmed on this host: no `ip` command, no
`/usr/include/linux`. There is no code-level workaround; this requires an
actual Linux kernel — a Linux VM (e.g. UTM, Multipass, Docker Desktop's
Linux VM doesn't expose a real kernel network namespace the same way,
Lima/colima, or a real Linux machine/cloud box) or dual-boot/native Linux.

## What's done and ready to use once on Linux

- **`app/boards/native_sim.overlay`** — written and reviewed, not yet build-
  verified (couldn't be, per above). Mirrors
  `deps/zephyr/tests/drivers/can/host/boards/native_sim.overlay` exactly for
  the CAN node (`can0`, `zephyr,native-linux-can`, disables the default
  `can_loopback0`, points `zephyr,canbus` at `can0`). Additionally aliases
  `click-uart` to native_sim's built-in second UART (`uart1`, a
  `zephyr,native-pty-uart` — disabled by default, enabled here) so NMEA/GPS
  parsing can still be exercised by feeding sentences into its host-side PTY
  peer, and aliases `gnss7-sel`/`sw1` to native_sim's emulated `&gpio0`
  controller using the same "borrow the `gpio-leds` binding for a plain GPIO
  alias" trick the real hardware overlay
  (`boards/nrf9160dk_nrf9160_ns.overlay`) already uses for `gnss7-sel` — this
  was necessary because `main.c`'s `user_btn`/`sw1` GPIO setup and
  `app_sensors.c`'s `gnss7_sel`/`click_uart` aliases are used unconditionally
  (not `#ifdef`-guarded), so the build needs *something* real to resolve
  those devicetree aliases to, even if it's emulated rather than real
  hardware.
- Research (both driver mechanics and the exact host-side vcan0/zcan0 setup
  convention) is recorded in `AUDIT.md`'s Phase 3.5 section once that's
  filled in — see the citations there for `zephyr,native-linux-can`'s
  binding, Kconfig, and the working test-overlay this one was copied from.

## Steps to actually run this, once on a Linux host

1. `sudo modprobe vcan && sudo ip link add dev vcan0 type vcan`
2. `sudo ip link property add dev vcan0 altname zcan0 && sudo ip link set up vcan0`
   (native_sim's own devicetree defaults `host-interface` to `"zcan0"` —
   this altname step is what makes `vcan0` answer to that name; equally
   valid alternatives: create the interface directly as `zcan0`, or override
   `host-interface = "vcan0";` in the overlay instead of using an altname.)
3. `west build -p -b native_sim app` (no `--sysbuild` — native_sim doesn't
   use Partition Manager/MCUboot; if `west build` still tries to invoke
   sysbuild because `sysbuild.conf` is present, pass `--no-sysbuild`
   explicitly)
4. `./build/zephyr/zephyr.exe`
5. In another terminal, inject CAN traffic against `vcan0` with
   `python-can`/`cangen`/`canplayer` as usual, and confirm `zephyr.exe`'s log
   output shows frames being received/processed by `app_sensors.c`'s CAN
   thread. If Phase 3's MQTT module is wired up and a local Mosquitto broker
   is reachable, also confirm telemetry gets published — note this second
   part additionally requires native TLS credentials wired up separately,
   since `CONFIG_MODEM_KEY_MGMT` (the nRF9160 modem-offload credential path
   `uplink_mqtt.c` uses) depends on `CONFIG_NRF_MODEM_LIB`, which isn't
   available on native_sim; `uplink_mqtt_init()`'s credential-provisioning
   step degrades to a no-op warning there rather than failing outright, but
   an actual TLS handshake to a broker won't succeed without further,
   native_sim-specific credential wiring not yet built. CAN/OBD frame
   processing and `buffer_fifo` logic don't depend on this and are fully
   testable via log output alone.

## Scope limit (per the original plan, restated for clarity)

`native_sim` is a POSIX-architecture host build — once running on Linux, it
validates CAN/OBD/DTC-decode/MQTT-publish/buffering *logic*, not the real
nRF9160 modem, GNSS, or SPI/MCP2515 hardware path. Don't let a passing
`native_sim` run imply anything about radio- or SPI-timing-correctness on
real hardware — it's a logic/integration test only.

## Verification status, per module

One row per module, populated honestly from what's actually been done —
not what's merely been written. "Compile-verified" means it builds clean
as part of the full sysbuild; nothing below has been run.

| Module | Compile-verified | native_sim-verified | Real-hardware-verified |
|---|---|---|---|
| `obd_j1979.c`/`.h` (Mode 01 PIDs) | Yes | No — blocked, see above | No |
| `dtc_decode.c`/`.h` (Mode 03 DTCs) | Yes | No — blocked, see above | No |
| `app_sensors.c` GPS (NMEA RMC+GGA) parsing | Yes | No — blocked, see above | No |
| `app_sensors.c` DTC-event dedup | Yes | No — blocked, see above | No |
| `uplink_mqtt.c`/`.h` (connect/publish/subscribe) | Yes | No — blocked, see above; also needs a reachable broker, which native_sim alone doesn't provide | No |
| `buffer_fifo.c`/`.h` (NVS offline buffer) | Yes | No — blocked, see above | No |
| `harsh_driving.c`/`.h` (threshold math) | Yes | N/A — no Zephyr dependency at all; could be run via `west twister --platform unit_testing` without native_sim, but hasn't been | No |
| `imu_icm42688.c`/`.h` (SPI sensor I/O) | Yes | No — blocked, see above | No |
| `ota_update.c`/`.h` + MQTT OTA subscribe path | Yes | No — blocked, see above | **No — see `ISSUES.md`'s Phase 6 section; this is the single highest-priority item to verify on real hardware** |

Nothing in this project has been native_sim- or hardware-verified yet.
Every "No" above traces back to the same root cause (macOS host, Linux-only
`native_sim`) except the OTA fetch/reboot/swap cycle, which needs real
hardware regardless of native_sim's availability.
