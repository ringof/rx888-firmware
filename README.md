# RX888mk2 Firmware

[![Latest release](https://img.shields.io/github/v/release/ringof/rx888-firmware?include_prereleases&label=latest%20release)](https://github.com/ringof/rx888-firmware/releases/latest)
[![Build](https://github.com/ringof/rx888-firmware/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/ringof/rx888-firmware/actions/workflows/build.yml?query=branch%3Amain)

Cypress FX3 USB controller firmware for the **RX888 mk2** (also written
RX888mk2) direct-sampling SDR receiver. HF reception (0–32 MHz) via the
on-board LTC2208 ADC; the R828D VHF tuner is detected but not driven by
the firmware (see Limitations).

## Compatible host applications

- **[ka9q-radio](https://github.com/ka9q/ka9q-radio)** — multichannel
  software-defined-radio receiver with native RX888 support. Validated
  end-to-end via the bundled Docker test container; see
  [`docker/ka9q-radio/`](docker/ka9q-radio/) for setup.
- **[rx888_tools](https://github.com/ringof/rx888_tools)** — streamer,
  DSP utilities, and udev rules maintained alongside this firmware. The
  `rx888_stream` binary from this project is the firmware uploader and
  USB capture tool used by the test harness in [`tests/`](tests/).
- **ExtIO-based Windows hosts** (HDSDR, SDR Console) — historically
  supported via the upstream
  [ExtIO_sddc](https://github.com/ik1xpv/ExtIO_sddc) project. This fork
  is firmware-only and does not include the ExtIO DLL.

## Limitations

This firmware operates the **HF direct-sampling path only**. The R828D
VHF tuner is detected during hardware identification but is not
controlled by the firmware — the GPL-licensed R82xx driver has been
removed to resolve a license conflict with the proprietary Cypress SDK.
VHF reception via the R828D requires a host-side tuner driver
communicating over the I2C passthrough commands (`I2CWFX3`/`I2CRFX3`).

## Building

### Requirements

- Debian or Ubuntu-based Linux distribution
- `arm-none-eabi-gcc` toolchain (tested with 13.2.1)
- `gcc` (host compiler, for the `elf2img` utility)
- `make`

Install the cross-compiler on Debian/Ubuntu:

```
sudo apt install gcc-arm-none-eabi
```

The Cypress FX3 SDK (v1.3.4) is included in the `SDK/` directory.

### Build

```
cd SDDC_FX3
make clean && make all
```

This produces `SDDC_FX3.img`, which can be flashed to the device.

## Testing

Hardware tests require an RX888mk2 connected via USB and `libusb-1.0-0-dev`:

```
sudo apt install libusb-1.0-0-dev
git submodule update --init
cd tests && make
./fw_test.sh --firmware ../SDDC_FX3/SDDC_FX3.img
```

### USB device permissions

The FX3 USB device must be accessible to the user running the tests.
Install the udev rules from the
[rx888_tools](https://github.com/ringof/rx888_tools) submodule and
reload:

```
sudo cp tests/rx888_tools/udev/99-rx888.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

This grants read/write access to both the bootloader (PID `00f3`) and
programmed (PID `00f1`) device endpoints.  Without these rules,
`rx888_stream` and `fx3_cmd` will fail with `LIBUSB_ERROR_ACCESS`
unless run as root.

### USB buffer memory

The streaming test submits many large USB transfers concurrently. Linux
limits per-device USB buffer memory to 16 MB by default, which is not
enough and will cause `LIBUSB_ERROR_NO_MEM` failures. Raise (or
disable) the limit before running the tests:

```
sudo sh -c 'echo 0 > /sys/module/usbcore/parameters/usbfs_memory_mb'
```

To make this persistent across reboots, create
`/etc/modprobe.d/usbcore.conf`:

```
options usbcore usbfs_memory_mb=0
```

This builds `fx3_cmd` (vendor command exerciser) and `rx888_stream`
(from the [rx888_tools](https://github.com/ringof/rx888_tools) submodule),
uploads the firmware, and runs an automated test suite.

## Firmware Robustness

Recovery is owned by a single module — `SDDC_FX3/health.c` — that
collects liveness signals from across the firmware and applies a
documented cascade of remedies when the device is unhealthy.  This
section describes the architecture and the implementation status of
each cascade level.  See `PLAN_RECOVERY.md` at the repo root for the
design rationale and PR sequencing.

### Health interface

Three functions form the contract.  Every recovery contribution goes
through them — no parallel watchdog mechanisms, no inline cleanup in
handlers.

| Function | Role |
|---|---|
| `health_record_event(event)` | Callers observe a liveness event (EP0 callback exit, DMA progress, …) and report it.  Cheap; safe from any thread. |
| `health_evaluate()` | Pure function called periodically from the main loop.  Examines accumulated state and returns a structured `health_status_t`.  Does not take action. |
| `health_recover(status)` | Called when evaluation reports unhealthy.  Picks and applies a remedy from the cascade based on the failure reason. |

Adding a new failure-mode detection means adding a new event type and
a new evaluation branch — not writing a new watchdog.

### Recovery cascade

| Level | Remedy | Appropriate for | Latency | Status |
|---|---|---|---|---|
| 1 | Soft-stop FW_TRG + `DmaMultiChannelReset` + `GpifSMStart` | Streaming wedge | ~300 ms | **Implemented** (existing watchdog in `RunApplication.c`; pending migration into `health_recover()` per `PLAN_RECOVERY.md` §5 PR N) |
| 2 | EP0 stall+unstall + `FlushEp` on EP1 | EP0 stuck state | ~10 ms | **Not implemented yet** |
| 3 | `StopApplication` + `StartApplication` | Application-level state corruption | ~100 ms | **Not implemented yet** |
| 4 | `CyU3PDeviceReset(CyFalse)` | Anything else / catastrophic | full re-enumeration | **Implemented** in `health_recover(HEALTH_WEDGED_EP0)` for vendor-handler hangs (#104, #105) |
| 5 | FX3 hardware watchdog timer (`CyU3PSysWatchDogConfigure` + periodic pet) | Levels 1–4 themselves wedged (i.e. main thread itself dead) | up to 10 s | **Implemented** in `health_init()` + `health_pet()` (every 100 ms from main-loop iterations); fires only if main thread stops calling pet for >10 s |

### What's covered today

- **Streaming wedges** — DMA producer stuck waiting for the host to
  drain, GPIF state machine in `TH0_WAIT`/`TH1_WAIT`/`TH0_RD` for
  >300 ms.  The existing watchdog in `SDDC_FX3/RunApplication.c`
  detects and recovers; observed reliable across `wedge_recovery`
  scenarios in `fw_test.sh` and `soak_test.sh`.
- **EP0 vendor-handler hangs** — a vendor request callback wedged
  inside an SDK call for >2 s.  `health_evaluate()` detects via the
  EP0 enter/exit timestamp; `health_recover(HEALTH_WEDGED_EP0)` fires
  `CyU3PDeviceReset(CyFalse)` (Level 4).  Validated end-to-end by the
  `test_health_recovery` host scenario which uses the firmware-side
  `HANGFX3` test command to trigger the failure deterministically.
- **Catastrophic main-thread hang** — if the main thread itself stops
  calling `health_pet()` for >10 s, the FX3 hardware watchdog fires
  and resets the device (Level 5).  Pure defense in depth; covers any
  failure mode in which Levels 1-4 (which depend on the main thread
  to fire) become unreachable.

### What's not covered yet — known gaps

- **EP0 / vendor-handler hangs** (issue #104, #105).  When a vendor
  handler such as STOPFX3 hangs inside an FX3 SDK call, EP0 stops
  responding while USB enumeration remains alive.  Today's watchdog
  cannot fire (its gating conditions are streaming-specific) and
  there is no other recovery layer below it; manual USB power-cycle
  is the only recourse.  Target: cascade level 4 in PR 2.

- **`gpif_soft_stop` 1 ms timing window** (issue #106).  An
  intermittent (~10% per cycle) where the firmware's 1 ms wait after
  `FW_TRG` deassertion is insufficient for the SM to reach IDLE,
  causing a force-stop fallback.  Independent of the recovery
  architecture; tracked separately.

### Soak status

Latest 1-hour soak on current `main`: 2099 cycles, 1 transient
`rapid_start_stop` failure (self-recovered, 0.05% per-cycle rate),
zero unrecoverable wedges, health checks 2099/2099.  Specific failure
modes covered by `health.c` cascade levels will be retested after each
level lands.

## Docker — ka9q-radio Compatibility Testing

The `docker/ka9q-radio/` directory provides a containerized
[ka9q-radio](https://github.com/ka9q/ka9q-radio) environment for
end-to-end firmware validation against a real-world SDR application.

ka9q-radio is a widely used SDR receiver that drives the RX888mk2
directly via libusb — it programs the Si5351 clock via I2C passthrough,
controls GPIO and attenuator settings, and streams IQ data through the
GPIF pipeline.  Testing against it exercises firmware code paths
(firmware upload, `STARTFX3`/`STOPFX3` cycling, `I2CWFX3`, `GPIOFX3`,
`SETARGFX3`) that the low-level test harness in `tests/` does not cover.

The Docker container builds only the `radiod` core, the `rx888.so`
plugin, and control utilities — no other SDR hardware drivers are
included.  It runs with `--privileged` (required because the FX3
changes USB PID during firmware upload) and `--network host` (for
ka9q-radio's multicast output).

```
docker build -t ka9q-radio docker/ka9q-radio/
docker run --rm -it --privileged \
  -v /dev/bus/usb:/dev/bus/usb \
  -v /run/udev:/run/udev:ro \
  -v $(pwd)/SDDC_FX3:/firmware \
  -v $(pwd)/wisdom:/var/lib/ka9q-radio \
  --network host \
  ka9q-radio
```

See `docker/ka9q-radio/README.md` for detailed usage and known
compatibility notes.

## Repository Layout

```
SDDC_FX3/           Firmware source code
  driver/           IC drivers (Si5351 clock generator)
  radio/            Radio hardware support (rx888r2)
SDK/                Cypress FX3 SDK 1.3.4 (headers, libraries, build system)
tests/              Hardware test tools
  rx888_tools/      Submodule: USB streamer and firmware uploader
docker/             Docker-based integration testing
  ka9q-radio/       ka9q-radio radiod + rx888 plugin container
docs/               Project documentation and analysis
  architecture.md   Firmware architecture overview
  gpif-and-recovery.md  GPIF state machine design and recovery layers
  diagnostics_side_channel.md  Debug and diagnostics interface
  wedge_detection.md  DMA stall detection and watchdog design
  LICENSE_ANALYSIS.md  License audit of SDK and dependencies
```

## License

MIT License — see [LICENSE.txt](LICENSE.txt).

The Cypress FX3 SDK in `SDK/` is covered by the Cypress Software License
Agreement (included in `SDK/license/license.txt`).

## Acknowledgments

This project is derived from
[ExtIO_sddc](https://github.com/ik1xpv/ExtIO_sddc) by Oscar Steila
(IK1XPV). Thanks to all who contributed to the original project:

- Oscar Steila (IK1XPV) — original author
- Howard Su
- Franco Venturi
- Hayati Ayguen
- Ruslan Migirov
- Vladisslav2011
- Phil Ashby (phlash)
- Alberto di Bene (I2PHD)
- Mario Taeubel
