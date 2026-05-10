# SDDC_FX3 Firmware

[![Latest release](https://img.shields.io/github/v/release/ringof/ExtIO_sddc?include_prereleases&label=latest%20release)](https://github.com/ringof/ExtIO_sddc/releases/latest)
[![Build](https://github.com/ringof/ExtIO_sddc/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/ringof/ExtIO_sddc/actions/workflows/build.yml?query=branch%3Amain)

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

The firmware implements a layered recovery architecture so that
streaming failures are almost always recoverable without power-cycling
the device.

### GPIF soft-stop

The GPIF II state machine includes `!FW_TRG` exit transitions on all
active states that can stall (TH0_RD, TH1_RD, TH0_WAIT, TH1_WAIT).
When the host issues STOPFX3, the firmware deasserts FW_TRG and the
state machine exits to IDLE within 3 clock cycles (~47 ns at 64 MHz).
`CyU3PGpifDisable(CyFalse)` then disables a quiescent machine with no
DMA debris.  If the SM fails to reach IDLE (e.g. dead external clock),
the firmware falls back to `CyU3PGpifDisable(CyTrue)` (force-stop).

### Watchdog recovery

If the DMA pipeline stalls mid-stream (glDMACount stops advancing while
the SM is in a BUSY or WAIT state), a watchdog in the main application
loop detects it after 300 ms.  It tears down the GPIF and DMA channel,
then rebuilds the pipeline — soft-stop first, force-stop as fallback.
Recovery is capped at `WDG_MAX_RECOVERY_DEFAULT` (5) attempts per
session; after that the watchdog waits for the host to issue a new
STARTFX3.  The cap prevents unbounded recovery loops when the host is
not draining data (e.g. application crash).

### Host restart

STOPFX3 followed by STARTFX3 resets all pipeline state: DMA channels,
GPIF configuration, counters, and the watchdog recovery cap.  This is
the heaviest recovery path and provides a clean slate regardless of
what state the firmware was in.

### Recovery hierarchy

| Layer | Trigger | Action | Latency |
|-------|---------|--------|---------|
| Soft-stop | STOPFX3 / watchdog | FW_TRG deassert → SM exits to IDLE | ~1 ms |
| Force-stop | Soft-stop failure | `CyU3PGpifDisable(CyTrue)` | ~1 ms |
| Watchdog | DMA stall 300 ms | Tear down + rebuild pipeline | ~300 ms |
| Host restart | Application decision | STOPFX3 + STARTFX3 | ~100 ms |

Soak testing (500+ randomized cycles) demonstrates 100% health-check
pass rate and zero device crashes with this architecture.

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
