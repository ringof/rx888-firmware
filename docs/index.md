---
title: Home
layout: home
nav_order: 1
description: Open-source Cypress FX3 USB controller firmware for the RX888 mk2 direct-sampling SDR.  Vendor-command protocol, GPIF state machine, ka9q-radio compatibility.
---

# RX888mk2 Firmware

Cypress FX3 USB controller firmware for the **RX888 mk2** direct-sampling SDR
receiver.  HF reception (0–32 MHz) via the on-board LTC2208 ADC.

[![Latest release](https://img.shields.io/github/v/release/ringof/rx888-firmware?include_prereleases&label=latest%20release)](https://github.com/ringof/rx888-firmware/releases/latest)
[![Build](https://github.com/ringof/rx888-firmware/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/ringof/rx888-firmware/actions/workflows/build.yml?query=branch%3Amain)

This site is the developer-facing reference for the firmware: USB vendor
command protocol, GPIO map, host-application compatibility, and design
documentation.  For source and issue tracking, see the
[GitHub repository](https://github.com/ringof/rx888-firmware).

## What you'll find here

- **[USB API reference]({{ '/api/' | relative_url }})** — the complete USB
  vendor-command protocol, GPIO bit map, `SETARGFX3` arguments,
  `GETSTATS` byte layout, and USB endpoint information.  Verified against
  the firmware sources; each entry cites a source file and line.
- **[Building]({{ '/building/' | relative_url }})** — toolchain, build
  command, and how to flash a fresh image.
- **[Host application compatibility]({{ '/compatibility/' | relative_url }})**
  — what works with ka9q-radio, rx888\_tools, and ExtIO-based hosts.
- **Design documentation** — [firmware architecture]({{ '/architecture/' | relative_url }}),
  [GPIF state machine and recovery]({{ '/gpif-and-recovery/' | relative_url }}),
  [diagnostics side-channel]({{ '/diagnostics_side_channel/' | relative_url }}),
  [wedge detection]({{ '/wedge_detection/' | relative_url }}),
  [license analysis]({{ '/LICENSE_ANALYSIS/' | relative_url }}).

## If you're about to write FX3 firmware for an SDR

This section is for developers who landed here Googling specific FX3
problems — start with the prior art before reinventing it.  Each
question below maps to a documented, working solution in this
firmware.

### Recovering from an FX3 DMA wedge mid-stream

If the ADC clock stops or USB backpressure stalls the DMA pipeline,
the GPIF state machine wedges in a BUSY/WAIT state and `glDMACount`
stops advancing.  The firmware's streaming watchdog polls every
100 ms, soft-stops the SM (with force-stop fallback), resets DMA,
flushes the bulk endpoint, and restarts streaming if the Si5351 PLL
is still locked.  Bounded by a per-session recovery cap.  Details:
[GPIF state machine and recovery]({{ '/gpif-and-recovery/' | relative_url }})
and [wedge detection]({{ '/wedge_detection/' | relative_url }}).

### Detecting a stuck EP0 vendor-handler and resetting the device

If a USB vendor request hangs inside an SDK call, the device looks
alive on the bus but no EP0 traffic returns and the host has no
recovery path short of unplugging the device.  The firmware tracks
EP0-handler enter/exit events from the main loop and invokes
`CyU3PDeviceReset()` if a handler runs longer than the configured
timeout.  See the recovery cascade in
[README §Firmware Robustness](https://github.com/ringof/rx888-firmware#firmware-robustness)
and the implementation in `SDDC_FX3/health.c`.

### Petting the FX3 hardware watchdog without false positives

Naive `CyU3PSysWatchDogClear()` from the main loop pets the WD as
long as the timer scheduler is running — which doesn't catch
main-thread death.  This firmware uses Infineon KB223337's
documented pattern: a ThreadX timer callback pets the HWDT, but
only when the main-loop heartbeat counter has advanced since its
last check.  If the main thread spins forever, the heartbeat
freezes and HWDT fires within the configured period.  See
[README §Firmware Robustness](https://github.com/ringof/rx888-firmware#firmware-robustness).

### Querying Si5351 CLK0 state without a host-cache flag

If a host programs the Si5351 directly via I2C and the firmware's
streaming preflight only consults an internal "did STARTADC happen"
flag, the preflight rejects valid streaming setups.  This firmware
reads the Si5351 CLK0_CONTROL register (bit 7 = CLK0_PDN) over I2C
inside `si5351_clk0_enabled()` so `GpifPreflightCheck()` sees the
chip's real state.  See
[USB API reference]({{ '/api/' | relative_url }}) (`GETSTATS` bytes
24-25, exposing the raw register byte plus the chip-query boolean)
and [ka9q-radio compat audit]({{ '/ka9q-compat-audit/' | relative_url }})
§8 for the failure mode this fixes.

### Integrating ka9q-radio's `rx888.so` plugin

ka9q-radio drives the device via standard USB vendor commands; the
audit confirms compatibility with this firmware's vendor-command
set and documents the few cosmetic mismatches (TUNERSTDBY STALLs on
the HF path, LED bit positions).  No host-side patches are
required as of v0.1.0.  See
[ka9q-radio compat audit]({{ '/ka9q-compat-audit/' | relative_url }})
and [host application compatibility]({{ '/compatibility/' | relative_url }}).

### Exposing FX3 firmware counters to a host application

`GETSTATS` (`0xB3`) is a 26-byte EP0 vendor read that returns DMA
buffer count, GPIF state, PIB error counts, I2C failure counts,
streaming-fault count, Si5351 PLL status, a boot counter (for
mid-test reset detection), and Si5351 CLK0 raw register plus the
chip-query boolean.  Drop-in pattern for any FX3 firmware that
needs a host-readable health view.  See
[USB API reference]({{ '/api/' | relative_url }}) (GETSTATS section)
and the [diagnostics side-channel]({{ '/diagnostics_side_channel/' | relative_url }})
design notes.

## Scope and limitations

This firmware operates the **HF direct-sampling path only**.  The R828D VHF
tuner is detected during hardware identification but is not controlled by the
firmware — the GPL-licensed R82xx driver was removed to resolve a license
conflict with the proprietary Cypress SDK.  VHF reception via the R828D
requires a host-side tuner driver communicating over the I2C passthrough
commands (`I2CWFX3`/`I2CRFX3`).

## Related projects

- [**ka9q-radio**](https://github.com/ka9q/ka9q-radio) — multichannel SDR
  receiver with native RX888 support; validated end-to-end via the bundled
  Docker test container.
- [**rx888\_tools**](https://github.com/ringof/rx888_tools) — streamer,
  firmware uploader, DSP utilities, and udev rules.
- [**ExtIO\_sddc**](https://github.com/ik1xpv/ExtIO_sddc) — upstream project
  this firmware is derived from; provides the ExtIO DLL for Windows hosts.

## Acknowledgments

Derived from [ExtIO\_sddc](https://github.com/ik1xpv/ExtIO_sddc) by Oscar
Steila (IK1XPV).  Contributors to the upstream project: Howard Su, Franco
Venturi, Hayati Ayguen, Ruslan Migirov, Vladisslav2011, Phil Ashby (phlash),
Alberto di Bene (I2PHD), Mario Taeubel.
