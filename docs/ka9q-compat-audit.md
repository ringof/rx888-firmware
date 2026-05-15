---
title: ka9q-radio compat audit
nav_order: 10
permalink: /ka9q-compat-audit/
---

# ka9q-radio ↔ SDDC_FX3 Compatibility Audit

Status: **container-side patches retired in v0.1.0** — the only host-side workaround (patch 03) is no longer needed; see §8 for the firmware fix.

This document captures a static compatibility audit of ka9q-radio's
`rx888.so` plugin against the SDDC_FX3 firmware in this repository.
Every claim cites a specific source file; behavior is named by
function so the analysis stays valid across line shifts.

## Reproducer

- Firmware: `SDDC_FX3/SDDC_FX3.img` (current tree).
- ka9q-radio: pinned in `docker/ka9q-radio/Dockerfile` via
  `ARG KA9Q_RADIO_SHA`. As of writing:
  `f78cff9cc8c7fa33ea49aac9176d9263c535a332`.
- Container build/run:

  ```
  docker build -t ka9q-radio docker/ka9q-radio/
  docker run --rm -it --privileged \
    -v /dev/bus/usb:/dev/bus/usb \
    -v $(pwd)/SDDC_FX3:/firmware \
    --network host \
    ka9q-radio
  ```

- Failure observed (before container patches):

  ```
  loading rx888 firmware file /firmware/SDDC_FX3.img, done
  Error or device could not be found
  rx888_usb_init() failed
  device setup returned -1
  ```

## Vendor-command compatibility matrix

| ka9q sends | bRequest | SDDC supports | Notes |
|---|---|---|---|
| STARTFX3 | 0xAA | ✅ | |
| STOPFX3 | 0xAB | ✅ | |
| TESTFX3 | 0xAC | ✅ | |
| GPIOFX3 | 0xAD | ✅ | LED bit positions differ — see §4. |
| I2CWFX3 | 0xAE | ✅ | |
| I2CRFX3 | 0xAF | ✅ | |
| RESETFX3 | 0xB1 | ✅ | |
| STARTADC | 0xB2 | ✅ | Both sides sleep ~1 s; see §7. |
| TUNERINIT | 0xB4 | ❌ STALL | Only inside `#if 0` in `rx888.c`. |
| TUNERTUNE | 0xB5 | ❌ STALL | Only inside `#if 0`. |
| SETARGFX3 wIndex 1 (R82XX_ATTENUATOR) | 0xB6 | ❌ STALL | VHF only — see §3. |
| SETARGFX3 wIndex 2 (R82XX_VGA)        | 0xB6 | ❌ STALL | VHF only. |
| SETARGFX3 wIndex 10 (DAT31_ATT)       | 0xB6 | ✅ | HF path. |
| SETARGFX3 wIndex 11 (AD8340_VGA)      | 0xB6 | ✅ | HF path. |
| TUNERSTDBY | 0xB8 | ❌ STALL | **Called on the HF path** — see §2. |
| READINFODEBUG | 0xBA | ✅ | |

Sources: ka9q `src/rx888.c` and `src/rx888.h` at the pinned SHA;
SDDC firmware `docs/architecture.md` §"Command table" and
`docs/LICENSE_ANALYSIS.md` §"What was removed".

## Findings

### 1. `sleep(1)` after firmware upload *(ka9q-side, documented; no patch)*

After firmware upload, `rx888_usb_init()` in ka9q's `src/rx888.c`
sleeps once for one second, then performs a **single** scan for
`0x04b4:0x00f1` and returns "Error or device could not be found" if
absent.  The author's own comment on the `sleep(1)` flags the value
as a guess.

Original misdiagnosis: this was *thought* to be the cause of the
container's "Error or device could not be found" failure.  It was
not.  The actual cause was that libusb 1.0.26's hotplug listener
subscribes to systemd-udevd's filtered netlink events, and udevd
does not run inside the container, so libusb's cached device list
never updates and `libusb_get_device_list()` keeps returning a stale
list — even after a 10 s polling loop.  `LIBUSB_DEBUG=4` confirmed
the polling loop ran all 50 iterations seeing the same 8 cached
devices, never observing the re-enumerated `04b4:00f1`, even though
the host kernel had it fully enumerated (per `usbmon` and host-side
`lsusb`).

Fix: at `docker run` time, bind-mount `/run/udev:/run/udev:ro` so
libusb's udev backend can see host udev events.  With that mount in
place, the kernel finishes SuperSpeed enumeration ~430 ms after the
last firmware byte is acked (per `usbmon`), and the upstream
`sleep(1)` + single rescan succeeds reliably.  Verified working on
the project's reference hardware.

A polling-with-timeout pattern in place of the fixed `sleep(1)`
would be more robust on pathologically slow hosts, but it is not
required for SDDC streaming to work, so we do not patch it — the
cost of a not-strictly-necessary upstream ask outweighs the benefit
on hypothetical hardware.

### 2. TUNERSTDBY (0xB8) on the HF path *(ka9q-side, cosmetic; no patch)*

ka9q's `rx888_set_hf_mode()` and `rx888_start_rx()` both fire
`command_send(...,TUNERSTDBY,0)`.  SDDC firmware removed all R82xx
commands (see `docs/LICENSE_ANALYSIS.md`), so 0xB8 returns a clean
USB STALL via the firmware's default handler.  ka9q's `command_send`
ignores the return value, so streaming is unaffected and init
proceeds normally.  The only effect is bus-level noise: two STALLed
control transfers per session.

We do not patch this either — purely cosmetic, would not change
observable behavior, and removing two unconditional command sends
that "happen to harmlessly fail" is exactly the kind of upstream
patch most maintainers will reject as not-their-bug.

### 3. R82xx VHF path *(deferred, out of scope)*

ka9q's `rx888.c` issues `R82XX_ATTENUATOR` and `R82XX_VGA` on the
VHF init path, plus `TUNERINIT` and `TUNERTUNE` inside an `#if 0`
block.  VHF will not work end-to-end because SDDC removed every
R82xx code path; ka9q itself flags VHF as broken in a top-of-file
comment ("VHF tuner does not work yet -- KA9Q, 17 Aug 2024").
HF is unaffected.

### 4. GPIO LED bit-position mismatch *(cosmetic)*

| Bit | ka9q (`rx888.h`) | SDDC firmware (`docs/architecture.md` §"GPIOFX3 bitmask protocol") |
|-----|-------|----------|
| 10  | `LED_YELLOW` | unused |
| 11  | `LED_RED`    | `LED_BLUE` |
| 12  | `LED_BLUE`   | unused |

Driving `LED_BLUE` from ka9q is a no-op on hardware; driving
`LED_RED` lights the blue LED. No streaming impact. Either side
could converge to the other's mapping; deferred.

### 5. SuperSpeed gate *(host topology, not SDDC)*

ka9q's `rx888.c` rejects anything below `LIBUSB_SPEED_SUPER`
silently inside its device scan loop, then exits with the same
"device could not be found" string.  If the device is plugged into
a USB-2 port or behind a hub that downgrades, ka9q reports
identical-looking output.  Worth flagging in user docs.

### 6. PID `0x00F1` matches ✅

ka9q's `Loaded_product_id = 0x00f1` in `rx888.c` matches SDDC
firmware's advertised PID (see `docs/architecture.md` §"USB
descriptors").

### 7. STARTADC settling

SDDC firmware's `STARTADC` handler polls `si5351_pll_locked()` for
up to ~100 ms after programming the PLL, returning as soon as PLL A
reports lock (see `docs/architecture.md` §"Clock synthesis").  ka9q
also sleeps ~1 s host-side around its direct Si5351 programming in
`rx888_set_samprate()`.  The firmware-side wait used to be a fixed
~1 s sleep before commit `13b2091`; together with ka9q's host-side
sleep the round trip was ~2 s.  As of v0.1.0 the firmware contributes
≤100 ms and the round trip is dominated by ka9q's host-side wait.

### 8. Show-stopper: missing `STARTADC` before `STARTFX3` *(resolved firmware-side in v0.1.0)*

> **Resolved in firmware** (commit
> [`13b2091`](https://github.com/ringof/rx888-firmware/commit/13b2091),
> released in **v0.1.0**).  `si5351_clk0_enabled()` now reads CLK0_CONTROL
> register 16 bit 7 over I2C instead of consulting a stale
> `glAdcClockEnabled` host-cache flag.  `GpifPreflightCheck()` therefore
> sees the live chip state, ka9q-radio's direct Si5351 programming
> path passes preflight without `STARTADC`, and patch 03 has been
> retired (kept in-tree as `.patch.disabled` for archaeology — see
> `docker/ka9q-radio/patches/README.md`).  The analysis below is
> preserved as a record of the original failure mode.


ka9q programs the Si5351 directly via raw `I2CWFX3` writes (in
`rx888_set_samprate()`) and intentionally never calls `STARTADC`
(the `docker/ka9q-radio/README.md` originally documented this as a
feature: "bypasses firmware STARTADC").  Against stock RX888
firmware where `STARTADC` only does Si5351 setup, this works.

Pre-v0.1.0 SDDC firmware tracked ADC-clock readiness via a separate
global flag `glAdcClockEnabled` in `SDDC_FX3/driver/Si5351.c`, set
`CyTrue` *only* inside `si5351aSetFrequencyA(freq>0)`, called *only*
from the `STARTADC` handler in `SDDC_FX3/USBHandler.c`.  The
`STARTFX3` handler then ran `GpifPreflightCheck()` in
`SDDC_FX3/StartStopApplication.c` before starting the GPIF state
machine:

    if (!si5351_clk0_enabled())  return CyFalse;  // glAdcClockEnabled
    if (!si5351_pll_locked())    return CyFalse;

`si5351_clk0_enabled()` returned the bare flag — it did not query
the chip — so even though ka9q had correctly programmed Si5351 via
I2C and the PLL was physically locked, the flag remained `CyFalse`.
`STARTFX3` stalled EP0, GPIF never started, no bulk data flowed,
and radiod timed out:

    rx888 running
    No rx888 data for 5 seconds, quitting

By comparison, `rx888_stream` (the test harness) sent the commands
in the order SDDC required:

    DAT31_ATT → AD8340_VGA → STARTADC(samprate) → STARTFX3 → ...

The original container-side fix was to insert
`command_send(...,STARTADC,samprate)` before `STARTFX3` in
`rx888_start_rx()`: STARTADC reprograms Si5351 to the same
frequency ka9q already wrote (harmless duplicate), sets
`glAdcClockEnabled = CyTrue`, polls PLL lock (already locked,
returns immediately), and `STARTFX3`'s preflight then passes.

That fix shipped as `docker/ka9q-radio/patches/03-startadc-before-startfx3.patch`
through commit `13b2091`, at which point the firmware-side fix in
§8's resolution banner above made the host-side patch unnecessary.
The disabled patch file remains in-tree as
`03-startadc-before-startfx3.patch.disabled` for archaeology; see
`docker/ka9q-radio/patches/README.md`.

## Container-side requirements (no patches needed)

A bind-mount of `/run/udev:/run/udev:ro` is **required** at
`docker run` time so libusb's hotplug listener inside the container
sees host udev events.  Without it libusb's cached device list never
updates after the FX3 re-enumerates, and `rx888_usb_init()` exits
with "Error or device could not be found".  See §1 for the analysis.

## Patches applied in this container

**None as of v0.1.0.**  The only patch that ever shipped (patch 03 —
inserting `STARTADC` before `STARTFX3`) was retired once the firmware
began reporting Si5351 CLK0 state truthfully; see §8.  The disabled
patch is preserved in-tree as
`docker/ka9q-radio/patches/03-startadc-before-startfx3.patch.disabled`
and is skipped by the Dockerfile's `*.patch` glob.  See
`docker/ka9q-radio/patches/README.md` for the historical record.

The findings in §1 (`sleep(1)`) and §2 (TUNERSTDBY) are documented
above but do **not** become container patches: §1 has a working
container-level workaround (the udev mount), and §2 is purely
cosmetic.  Carrying patches we don't strictly need would dilute any
future ask we make of the upstream maintainer.

## Open work

- Long-run streaming stability against the v0.1.0 container is not
  yet measured.  Initial confirmation in v0.1.0 prep: container
  reaches "rx888 running" and starts streaming without host-side
  patches.
- File issues for VHF support (firmware-side R82xx return) and the
  LED bit-position decision; both are non-blocking for HF receive.
