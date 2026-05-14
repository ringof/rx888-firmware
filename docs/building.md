---
title: Building
nav_order: 3
permalink: /building/
---

# Building the firmware
{: .no_toc }

1. TOC
{:toc}

## Requirements

- Debian or Ubuntu-based Linux distribution
- `arm-none-eabi-gcc` toolchain (tested with 13.2.1)
- `gcc` (host compiler, for the `elf2img` utility)
- `make`

Install the cross-compiler on Debian/Ubuntu:

```
sudo apt install gcc-arm-none-eabi
```

The Cypress FX3 SDK (v1.3.4) is included in the `SDK/` directory and
is built as part of the firmware build — no separate SDK install
is required.

## Build

```
cd SDDC_FX3
make clean && make all
```

This produces `SDDC_FX3.img`, the FX3 firmware image that can be
flashed to the device.

## Flashing

The image is uploaded to RAM each cold boot via the FX3 BootROM USB
protocol.  The [`rx888_tools`](https://github.com/ringof/rx888_tools)
project ships `rx888_stream`, which acts as both the firmware uploader
(when the device is in the bootloader state, PID `00f3`) and the IQ
capture tool (once the firmware is running, PID `00f1`).  See the
`rx888_tools` README for usage.

For details on the BootROM protocol itself, see
[Cypress AN76405 / EZ-USB FX3 Boot Options](https://www.cypress.com/) —
this firmware uses the standard "RAM upload via vendor command" path.

## CI builds

The `build.yml` GitHub Actions workflow builds the firmware on every
push and pull-request.  Successful builds publish `SDDC_FX3.img` as a
workflow artifact; tagged releases publish it as a release asset via
`release.yml`.

See [Releases](https://github.com/ringof/rx888-firmware/releases) for
the most recent firmware images.
