# ka9q-radio ↔ SDDC_FX3 Compatibility Audit

Status: **initial findings, container-side patches landed**

This document captures a static compatibility audit of ka9q-radio's
`rx888.so` plugin against the SDDC_FX3 firmware in this repository.
It is intentionally narrow: every claim cites a specific source line.

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
| TUNERINIT | 0xB4 | ❌ STALL | Only inside `#if 0` (`rx888.c:950–980`). |
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

### 1. Show-stopper: 1 s re-enumeration timeout *(ka9q-side)*

`src/rx888.c:700`:

```c
sleep(1); // how long should this be?
```

After firmware upload, ka9q sleeps once for one second, then performs
a **single** scan for `0x04b4:0x00f1` and returns "Error or device
could not be found" if absent (`rx888.c:803–804`). The author's own
comment flags the value as a guess. FX3 boot + udev settle on Linux
can easily exceed 1 s, more so under Docker bind-mounted
`/dev/bus/usb`.

The user-observed log shows zero "found rx888 vendor 04b4, device
00f1" lines, confirming the second scan ran before the device
re-appeared. **This is the immediate cause of `device setup returned
-1`.**

Fix (two parts, both required under Docker):

1. **Container side: bind-mount `/run/udev:/run/udev:ro`** when running
   the container.  libusb 1.0.26's hotplug listener subscribes to
   systemd-udevd's filtered netlink events; udevd does not run inside
   the container, so without this mount libusb's cached device list
   never updates and `libusb_get_device_list()` keeps returning a
   stale list that includes the dead bootloader and excludes the
   loaded device.  Verified by `LIBUSB_DEBUG=4`: the polling loop ran
   all 50 iterations seeing the same 8 cached devices, never
   observing the re-enumerated `04b4:00f1`, even though the host
   kernel had it fully enumerated (per `usbmon` and host-side
   `lsusb`).  This was the actual original cause of "Error or device
   could not be found" — not the timing race we initially suspected.

2. **ka9q side: poll for the loaded PID for ~10 s, then sleep 1 s**
   before returning, instead of the upstream `sleep(1)`.  This is a
   secondary defence: with the udev mount in place, hotplug events
   propagate within ~1 s, and the polling loop typically breaks out
   in the first iteration; the 1 s settle delay then covers any
   residual lag in the kernel's SuperSpeed enumeration (config
   selection + U1/U2 LPM enable) before the upstream rescan at
   `rx888.c:717` runs.  See
   `docker/ka9q-radio/patches/01-poll-reenumeration.patch`.

Without (1) the patch alone is insufficient — the polling can never
observe the re-enumerated device.  Without (2) the upstream's bare
`sleep(1)` is fragile across hosts even when udev events do arrive.

### 2. TUNERSTDBY (0xB8) on the HF path *(ka9q-side, non-fatal)*

`src/rx888.c:943` (`rx888_set_hf_mode`) and `src/rx888.c:1003`
(`rx888_start_rx`) both fire `command_send(...,TUNERSTDBY,0)`. SDDC
firmware removed all R82xx commands (`docs/LICENSE_ANALYSIS.md`),
so 0xB8 returns a clean USB STALL. Streaming likely survives because
`command_send` ignores the error, but the bus is noisier than it
should be.

Fix: drop both calls. See
`docker/ka9q-radio/patches/02-drop-tunerstdby-hf.patch`.

### 3. R82xx VHF path *(deferred, out of scope)*

`rx888.c:921` (`R82XX_ATTENUATOR`), `rx888.c:937` (`R82XX_VGA`),
`rx888.c:972–973` (TUNERINIT, TUNERTUNE — inside `#if 0`).

VHF will not work end-to-end because SDDC removed every R82xx code
path; ka9q itself flags VHF as broken (`rx888.c:11`,
"VHF tuner does not work yet -- KA9Q, 17 Aug 2024"). HF is unaffected.

### 4. GPIO LED bit-position mismatch *(cosmetic)*

| Bit | ka9q (`rx888.h:131–133`) | SDDC firmware (`docs/architecture.md` §"GPIOFX3 bitmask protocol") |
|-----|-------|----------|
| 10  | `LED_YELLOW` | unused |
| 11  | `LED_RED`    | `LED_BLUE` |
| 12  | `LED_BLUE`   | unused |

Driving `LED_BLUE` from ka9q is a no-op on hardware; driving
`LED_RED` lights the blue LED. No streaming impact. Either side
could converge to the other's mapping; deferred.

### 5. SuperSpeed gate *(host topology, not SDDC)*

`src/rx888.c:769` rejects anything below `LIBUSB_SPEED_SUPER`
silently inside the scan loop, then exits with the same
"device could not be found" string. If the device is plugged into a
USB-2 port or behind a hub that downgrades, ka9q reports
identical-looking output. Worth flagging in user docs.

### 6. PID `0x00F1` matches ✅

ka9q's `Loaded_product_id = 0x00f1` (`rx888.c:36`) matches SDDC
firmware's advertised PID (`docs/architecture.md` §687–692).

### 7. STARTADC settling

SDDC firmware sleeps 1000 ms inside the `STARTADC` handler after
PLL programming (`docs/architecture.md` §"Frequency setting
algorithm"); ka9q also sleeps ~1 s host-side around PLL config
(`rx888.c:400`). Total ~2 s; slower than necessary but not broken.

### 8. Show-stopper: missing `STARTADC` before `STARTFX3` *(ka9q-side)*

ka9q programs the Si5351 directly via raw `I2CWFX3` writes
(`rx888.c:331`, `rx888.c:340`) and intentionally never calls
`STARTADC` (the `docker/ka9q-radio/README.md` originally documented
this as a feature: "bypasses firmware STARTADC").  Against stock
RX888 firmware where `STARTADC` only does Si5351 setup, this works.

SDDC firmware tracks ADC-clock readiness via a separate global flag
`glAdcClockEnabled` (`SDDC_FX3/driver/Si5351.c:55`), set `CyTrue`
*only* inside `si5351aSetFrequencyA(freq>0)`
(`SDDC_FX3/driver/Si5351.c:250`), called *only* from the `STARTADC`
handler (`SDDC_FX3/USBHandler.c:219`).  The `STARTFX3` handler then
runs `GpifPreflightCheck()`
(`SDDC_FX3/StartStopApplication.c:99`) before starting the GPIF
state machine:

    if (!si5351_clk0_enabled())  return CyFalse;  // glAdcClockEnabled
    if (!si5351_pll_locked())    return CyFalse;

`si5351_clk0_enabled()` returns the bare flag
(`SDDC_FX3/driver/Si5351.c:174`) — it does not query the chip — so
even though ka9q has correctly programmed Si5351 via I2C and the
PLL is physically locked, the flag remains `CyFalse`.  `STARTFX3`
stalls EP0 (`SDDC_FX3/USBHandler.c:333`), GPIF never starts, no
bulk data flows, and radiod times out:

    rx888 running
    No rx888 data for 5 seconds, quitting

By comparison, `rx888_stream` (the test harness) sends the commands
in the order SDDC requires (`rx888_stream.c:1185 / 1193`):

    DAT31_ATT → AD8340_VGA → STARTADC(samprate) → STARTFX3 → ...

Fix: send `STARTADC` with the configured sample rate just before
`STARTFX3` in `rx888_start_rx()`.  STARTADC reprograms Si5351 to
the same frequency ka9q already wrote (harmless duplicate), sets
`glAdcClockEnabled = CyTrue`, polls PLL lock (already locked,
returns immediately), and `STARTFX3`'s preflight then passes.  See
`docker/ka9q-radio/patches/03-startadc-before-startfx3.patch`.

## Patches applied in this container

`docker/ka9q-radio/patches/01-poll-reenumeration.patch` — poll
`libusb_get_device_list` every 200 ms, up to 10 s, for
`0x04b4:0x00f1` after firmware upload, then settle 1 s before
returning.  Replaces `sleep(1)` at `rx888.c:700`.

`docker/ka9q-radio/patches/02-drop-tunerstdby-hf.patch` — remove
`TUNERSTDBY` at `rx888.c:943` and `rx888.c:1003`.

`docker/ka9q-radio/patches/03-startadc-before-startfx3.patch` —
insert `command_send(...,STARTADC,samprate)` before `STARTFX3` in
`rx888_start_rx` so SDDC's `GpifPreflightCheck()` passes.

The Dockerfile applies them with `git apply --whitespace=nowarn`
against the pinned SHA, in alphabetical order.  When upstream
ka9q-radio merges equivalent fixes, bump `KA9Q_RADIO_SHA` and drop
the corresponding patch.

A bind-mount of `/run/udev:/run/udev:ro` is also required at
`docker run` time so libusb's hotplug listener inside the container
sees host udev events; without it the polling patch can never
observe the re-enumerated device.  See §1 for the analysis.

## Open work

- Confirm container streams continuously (>1 minute) with all three
  patches applied and the udev mount present.  Initial confirmation:
  container reaches "rx888 running" and starts streaming; long-run
  stability not yet measured.
- Decide whether to upstream patches 01–03 to `ka9q/ka9q-radio`.
- File issues for VHF support (firmware-side R82xx return) and the
  LED bit-position decision; both are non-blocking for HF receive.
