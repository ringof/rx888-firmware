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

Fix: poll for the loaded PID for ~10 s. See
`docker/ka9q-radio/patches/01-poll-reenumeration.patch`.

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

## Patches applied in this container

`docker/ka9q-radio/patches/01-poll-reenumeration.patch` — poll
`libusb_get_device_list` every 200 ms, up to 10 s, for
`0x04b4:0x00f1` after firmware upload. Replaces `sleep(1)` at
`rx888.c:700`.

`docker/ka9q-radio/patches/02-drop-tunerstdby-hf.patch` — remove
`TUNERSTDBY` at `rx888.c:943` and `rx888.c:1003`.

The Dockerfile applies them with `git apply --whitespace=nowarn`
against the pinned SHA. When upstream ka9q-radio merges equivalent
fixes, bump `KA9Q_RADIO_SHA` and drop the corresponding patch.

## Open work

- Confirm container streams cleanly with both patches applied (this
  is the validation step for landing this audit).
- Decide whether to upstream patches A and B to `ka9q/ka9q-radio`.
- File issues for VHF support (firmware-side R82xx return) and the
  LED bit-position decision; both are non-blocking for HF receive.
