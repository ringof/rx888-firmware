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
