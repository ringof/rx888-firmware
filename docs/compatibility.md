---
title: Host application compatibility
nav_order: 4
permalink: /compatibility/
description: Host application compatibility for the RX888mk2 firmware - ka9q-radio (Docker container), rx888_tools / rx888_stream, ExtIO-based Windows hosts.
---

# Host application compatibility
{: .no_toc }

The firmware exposes a stable USB vendor-command interface (see
[USB API reference]({{ '/api/' | relative_url }})) that any host
application can drive over libusb (or a platform equivalent).  The
list below covers known-working hosts; new hosts can be brought up
against the API reference.

1. TOC
{:toc}

## ka9q-radio

[**ka9q-radio**](https://github.com/ka9q/ka9q-radio) is a
multichannel SDR receiver with native RX888 support.  It drives the
firmware directly via libusb: programs the Si5351 clock via I2C
passthrough (`I2CWFX3`), controls GPIO and attenuator settings, and
streams IQ data through the GPIF pipeline.

End-to-end validation is performed via the bundled Docker test
container in [`docker/ka9q-radio/`](https://github.com/ringof/rx888-firmware/tree/main/docker/ka9q-radio).
The container builds only the `radiod` core, the `rx888.so` plugin,
and control utilities.

Audit notes on the compatibility boundary live at
[ka9q-radio compatibility audit]({{ '/ka9q-compat-audit/' | relative_url }}).

## rx888\_tools

[**rx888\_tools**](https://github.com/ringof/rx888_tools) is the
sibling project maintained alongside this firmware.  It provides:

- `rx888_stream` — firmware uploader and USB capture tool used by
  the test harness in `tests/`.
- DSP utilities.
- udev rules granting non-root access to the FX3 USB device for both
  bootloader (`04b4:00f3`) and programmed (`04b4:00f1`) states.

This repository's `tests/` tree pulls `rx888_tools` in as a submodule;
see [Building]({{ '/building/' | relative_url }}) for the test build
flow.

## ExtIO-based Windows hosts

The original
[ExtIO\_sddc](https://github.com/ik1xpv/ExtIO_sddc) project provides
the **ExtIO DLL** that integrates the RX888 with Windows host
applications such as HDSDR and SDR Console.  This fork of the
firmware is **firmware-only** and does not include the ExtIO DLL —
Windows users should consult the upstream project for host-side
software, but can flash an updated firmware image from this project
once a Windows-side uploader is in place.

## Writing a new host application

The USB API is the only stable interface between the firmware and the
host.  A minimal host application needs to:

1. Detect the device by VID/PID (`04b4:00f3` cold, `04b4:00f1` after
   firmware upload).
2. Upload the firmware image via the FX3 BootROM protocol if the
   device is in cold state.
3. Issue [`STARTADC`]({{ '/api/' | relative_url }}#startadc) with the
   desired ADC clock frequency in Hz.
4. Configure the front end via
   [`GPIOFX3`]({{ '/api/' | relative_url }}#gpiofx3) and
   [`SETARGFX3`]({{ '/api/' | relative_url }}#setargfx3).
5. Issue [`STARTFX3`]({{ '/api/' | relative_url }}#startfx3) and read
   16-bit signed IQ samples from bulk endpoint `0x81`.
6. On shutdown, issue [`STOPFX3`]({{ '/api/' | relative_url }}#stopfx3)
   and close the device cleanly.

For diagnostics, use [`GETSTATS`]({{ '/api/' | relative_url }}#getstats)
and [`READINFODEBUG`]({{ '/api/' | relative_url }}#readinfodebug).
The [`HANGFX3`]({{ '/api/' | relative_url }}#hangfx3) and
[`HANGMAIN`]({{ '/api/' | relative_url }}#hangmain) test-only commands
are useful for verifying that your host's recovery and re-enumeration
logic handles full device resets cleanly.
