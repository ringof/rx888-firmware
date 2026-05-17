---
title: Diagnostics side-channel
nav_order: 7
permalink: /diagnostics_side_channel/
description: GETSTATS (0xB3) - a 26-byte EP0 vendor request that exposes FX3 firmware counters - DMA progress, GPIF state, PIB errors, Si5351 PLL/CLK0, boot count.
---

# Firmware-Side Diagnostics for Host Streamer Software

> **Status: Partially implemented.**
>
> The core diagnostic side-channel is live as **GETSTATS (`0xB3`)**, a
> 26-byte EP0 vendor request returning DMA counts, GPIF state, PIB
> error counts, I2C failure counts, streaming-fault counts, Si5351
> PLL status, boot count, and Si5351 CLK0 power-state diagnostics.
> See [`api.md` §GETSTATS](api.md) for the canonical wire format and
> [`SDDC_FX3/docs/debugging.md` §6](../SDDC_FX3/docs/debugging.md)
> for host-side usage examples.
>
> The extended `DIAGFX3` (`0xB7`) command and the 24-byte `fx3_diag_t`
> structure described in §§ "Future extensions" below remain **design
> proposals — not yet implemented**.

## Overview

The `glDMACount` variable, already incremented on every DMA buffer
completion, can serve as the foundation for a diagnostic side-channel
between the FX3 firmware and the host streamer (`rx888_stream`).
Combined with the RTOS tick counter and several other counters that are
either already present or trivially added, the firmware can expose a
real-time telemetry structure that lets the host detect missed samples,
clock drift, DMA congestion, USB transport errors, and other conditions
that would corrupt the SDR data pipeline.

---

## What glDMACount actually measures

Every time the GPIF fills a DMA buffer (16,384 bytes = 8,192 16-bit
samples), the DMA engine fires a `CY_U3P_DMA_CB_PROD_EVENT` callback
and `glDMACount` increments (`StartStopApplication.c:62`).

This makes `glDMACount` a **free-running sample counter at buffer
granularity**.  Since each buffer holds a fixed number of samples, and
the fill rate is determined by the ADC clock, the count rate is directly
proportional to the ADC sampling frequency:

```
buffers_per_second = sample_rate / 8192
```

| Sample rate | Expected buffers/sec | Expected count delta per 100 ms |
|------------|---------------------|-------------------------------|
| 64 MSPS | 7,812.5 | ~781 |
| 32 MSPS | 3,906.25 | ~391 |
| 16 MSPS | 1,953.125 | ~195 |
| 4 MSPS | 488.28 | ~49 |
| 2 MSPS | 244.14 | ~24 |

The firmware already has `CyU3PGetTime()` (maps to `tx_time_get()`),
which returns RTOS ticks since boot.  The default ThreadX tick on FX3
is 1 ms.  With a count and a timestamp, you have a throughput meter.

---

## What was implemented: GETSTATS (`0xB3`)

The firmware implements the `GETSTATS` vendor request, which delivers
the highest-value subset of the originally proposed telemetry.  It
chose a flat `memcpy`-based layout over a C struct to avoid packing
and alignment ambiguity on the ARM926EJ-S target.

| Offset | Size | Field | Proposal field | Status |
|--------|------|-------|----------------|--------|
| 0--3   | 4    | DMA buffer completions | `dma_buf_count` | **Implemented** |
| 4      | 1    | GPIF state             | `gpif_state`    | **Implemented** |
| 5--8   | 4    | PIB error count        | `pib_overrun_count` | **Implemented** (combined, not split by type) |
| 9--10  | 2    | Last PIB error arg     | *(not in proposal)* | **Implemented** (bonus diagnostic) |
| 11--14 | 4    | I2C failure count      | *(not in proposal)* | **Implemented** (bonus diagnostic) |
| 15--18 | 4    | Streaming fault count  | `streaming_faults` | **Implemented** (EP underruns + watchdog recoveries) |
| 19     | 1    | Si5351 status (reg 0)  | `si5351_status`   | **Implemented** |
| 20--23 | 4    | `boot_count`           | *(not in proposal)* | **Implemented** (per-boot counter for mid-test reset detection) |
| 24     | 1    | Si5351 CLK0_CONTROL (reg 16) | *(not in proposal)* | **Implemented** (live read; bit 7 = CLK0_PDN) |
| 25     | 1    | `clk0_result`          | *(not in proposal)* | **Implemented** (`si5351_clk0_enabled()`; same value `GpifPreflightCheck()` consults) |
| 26--29 | 4    | `glPpsCount`           | *(not in proposal)* | **Implemented** (synthetic-PPS successful partial-commit count; issue #125) |
| 30--33 | 4    | `glPpsCommitFailCount` | *(not in proposal)* | **Implemented** (wrap-up attempts where both producer sockets refused; issue #125) |

**Not yet implemented** from the original proposal:

| Field | Reason deferred |
|-------|-----------------|
| `timestamp_ms` | DMA count alone is sufficient for rate computation when the host timestamps its polls |
| `pib_underrun_count` | Combined into single PIB error counter |
| `usb_phy_errors` / `usb_link_errors` | Requires `CyU3PUsbGetErrorCounts()` — low priority, host can't act on it |
| `streaming` | Host already knows (it issues START/STOP) |
| `recovery_count` | Watchdog recovery IS implemented; count is embedded in `glCounter[2]` (GETSTATS offset 15-18) |

Full protocol details: [`SDDC_FX3/docs/debugging.md` §5](../SDDC_FX3/docs/debugging.md).

---

## Future extensions

### Originally proposed telemetry structure

```c
typedef struct fx3_diag {
    /* ---- Counters (monotonically increasing, wrap at 32 bits) ---- */
    uint32_t dma_buf_count;     /* DMA buffers produced (= glDMACount) */
    uint32_t timestamp_ms;      /* RTOS tick at time of snapshot */

    /* ---- Error counters ---- */
    uint16_t pib_overrun_count; /* PIB write overrun events (thread 0+1) */
    uint16_t pib_underrun_count;/* PIB read underrun events */
    uint16_t streaming_faults;  /* EP underruns + watchdog recoveries */
    uint16_t usb_phy_errors;    /* USB 3.0 PHY error count (8b10b, CRC) */
    uint16_t usb_link_errors;   /* USB 3.0 link error count */

    /* ---- Instantaneous state ---- */
    uint8_t  gpif_state;        /* Current GPIF SM state (0-9) */
    uint8_t  si5351_status;     /* Si5351 register 0: PLL lock bits */
    uint8_t  streaming;         /* glIsApplnActive */
    uint8_t  recovery_count;    /* Number of autonomous recoveries */
} fx3_diag_t;                   /* 24 bytes total */
```

This fits comfortably in the 64-byte EP0 buffer and can be delivered
as a vendor request response.  Most of the core fields are already
covered by GETSTATS; a future DIAGFX3 command would add the
remaining fields (timestamp, USB PHY/link errors, PLL lock, recovery
count) as a superset.

### Where each field comes from

| Field | Source | In GETSTATS? | Notes |
|-------|--------|:------------:|-------|
| `dma_buf_count` | `glDMACount` in `StartStopApplication.c` | **Yes** | Offset 0--3 |
| `timestamp_ms` | `CyU3PGetTime()` from ThreadX | No | Host-side timestamps suffice |
| `pib_overrun_count` | `glCounter[0]` via `PibErrorCallback` | **Yes** | Offset 5--8 (combined, not split by type) |
| `pib_underrun_count` | `PibErrorCallback` on `RD_UNDERRUN` | No | Folded into combined PIB count |
| `streaming_faults` | `glCounter[2]` via `USBEventCallback` + watchdog | **Yes** | Offset 15--18 |
| `usb_phy_errors` | `CyU3PUsbGetErrorCounts(&phy, &link)` | No | SDK API available, not yet wired |
| `usb_link_errors` | Same API, second output | No | SDK API available, not yet wired |
| `gpif_state` | `CyU3PGpifGetSMState(&state)` | **Yes** | Offset 4 |
| `si5351_status` | `I2cTransfer(0x00, 0xC0, 1, &status, CyTrue)` | **Yes** | Offset 19 |
| `streaming` | `glIsApplnActive` | No | Host already knows |
| `recovery_count` | `glWdgRecoveryCount` / `glCounter[2]` | **Yes** | Offset 15--18 (shared with EP underrun counter) |
| *(bonus)* last PIB arg | `glLastPibArg` in `PibErrorCallback` | **Yes** | Offset 9--10, not in original proposal |
| *(bonus)* I2C failures | `glCounter[1]` in `I2cTransfer` | **Yes** | Offset 11--14, not in original proposal |

---

## Delivery mechanism

### Chosen: New vendor command via EP0 (Option A)

**Implemented as GETSTATS (`0xB3`), 34 bytes.**  This follows Option A
from the original proposal — a dedicated vendor request returning
diagnostic counters via EP0, independent of the bulk data stream.

```
Host sends:  bRequest=0xB3, wValue=0, wIndex=0, wLength=34, direction=IN
FX3 returns: 34 bytes, packed little-endian (see api.md §GETSTATS for
             the canonical layout, or SDDC_FX3/docs/debugging.md §6
             for the firmware-side view)
```

The host calls this periodically (every 100-500 ms) alongside its
normal streaming.  EP0 control transfers do not interfere with the
bulk data stream on EP1.

If a future DIAGFX3 (`0xB7`) superset is added, it would extend the
same pattern with additional fields (timestamp, USB error counts, PLL
lock status).  GETSTATS would remain available for lightweight polling.

### Considered alternatives (not chosen)

**Option B — Extend TESTFX3:** The existing `TESTFX3` (0xAC) returns
only 4 bytes.  It could be extended, but coupling diagnostics to the
device-info query was undesirable.

**Option C — In-band metadata:** Embedding diagnostic frames in the
bulk data stream would require host-side parsing at 128 MB/s and change
the sample format.  Not recommended.

---

## What the host can derive from the telemetry

### 1. Missed samples (firmware side)

**Method:** Compare consecutive `dma_buf_count` snapshots with the
expected count based on elapsed time and configured sample rate.

```python
dt_ms = diag.timestamp_ms - prev.timestamp_ms
expected_bufs = (sample_rate / 8192) * (dt_ms / 1000.0)
actual_bufs = diag.dma_buf_count - prev.dma_buf_count
missed_bufs = expected_bufs - actual_bufs

if missed_bufs > 0:
    missed_samples = missed_bufs * 8192
```

A positive delta means the GPIF/DMA pipeline dropped buffers before
they reached USB.  This catches:
- DMA overruns (ADC clock too fast for DMA throughput)
- GPIF thread starvation
- Buffer congestion during USB link recovery

### 2. Missed samples (USB transport)

**Method:** The host also counts received bulk transfers.  Compare
the host-side count with `dma_buf_count`:

```python
usb_transport_loss = diag.dma_buf_count - host_received_count
```

Any delta represents data that the firmware produced and sent over
USB but the host failed to receive (USB errors, dropped transfers,
host-side buffer overflow).

### 3. ADC clock drift detection

**Method:** Compute the actual buffer rate and compare to the
programmed sample rate:

```python
measured_rate = actual_bufs * 8192 / (dt_ms / 1000.0)
drift_ppm = (measured_rate - sample_rate) / sample_rate * 1e6
```

The Si5351A synthesizes the ADC clock from a 27 MHz crystal through
a fractional-N PLL.  Real-world drift sources include:

- Crystal aging and temperature coefficient (~10-50 ppm)
- PLL lock settling after frequency change
- Integer boundary spurs at certain synthesis ratios
- Power supply noise coupling into the VCO

At buffer granularity (8192 samples), the measurement resolution is:

```
1 buffer = 8192 samples
At 64 MSPS: 1 buffer = 128 us
Over a 1-second window: 7812.5 expected buffers
Resolution: 1/7812.5 = 128 ppm per buffer
```

For sub-ppm drift measurement, the host needs to accumulate over
longer windows (10+ seconds) or use the sample data itself
(correlate against a known reference).

This level of precision is sufficient to detect:
- PLL unlock/relock events (thousands of ppm)
- Gross frequency errors (wrong divider programmed)
- Temperature-induced drift over minutes (tens of ppm)

It is **not** sufficient for precision frequency calibration (which
needs sub-ppb and should use a GPS-disciplined reference or the
sample data directly).

### 4. PLL lock status

**Method:** Check `si5351_status` bit 5 directly:

```python
pll_a_locked = not (diag.si5351_status & 0x20)
pll_b_locked = not (diag.si5351_status & 0x40)
sys_init     = bool(diag.si5351_status & 0x80)
```

This is the most direct indicator of clock health.  If PLL A is
unlocked, the ADC clock is either absent or unstable and all sample
data is garbage.  The host should stop processing and alert the user
immediately.

### 5. DMA/USB health

**Method:** Monitor the error counters:

```python
if diag.pib_overrun_count > prev.pib_overrun_count:
    # GPIF is writing faster than DMA can drain
    # ADC clock may be too fast, or USB is congested

if diag.streaming_faults > prev.streaming_faults:
    # Streaming pipeline fault (EP underrun or watchdog recovery)
    # May cause a glitch or gap in the sample stream

if diag.usb_phy_errors > threshold:
    # USB cable/connector quality issue
    # Expect data corruption or retransmissions

if diag.usb_link_errors > threshold:
    # USB link-layer problems
    # May indicate hub issues, power management interference
```

### 6. GPIF state machine health

**Method:** Check `gpif_state` for stuck conditions:

```python
BUSY_WAIT_STATES = {5, 7, 8, 9}  # TH0_BUSY, TH1_BUSY, TH1_WAIT, TH0_WAIT

if diag.gpif_state in BUSY_WAIT_STATES:
    consecutive_busy += 1
else:
    consecutive_busy = 0

if consecutive_busy > 3:  # stuck for 300+ ms
    # GPIF is stalled, DMA is congested
    # Host should consider stopping and restarting
```

Note: because the GPIF cycles through states at ~100 MHz and the
diagnostic snapshot is taken at EP0 request time, a single snapshot
showing a BUSY state is not alarming -- the state machine may be in
BUSY for a few nanoseconds between buffers during normal operation.
Only **consecutive** BUSY readings across multiple polls indicate a
real stall.

---

## Downstream impact on SDR applications

These diagnostics directly address failure modes that corrupt the SDR
signal processing pipeline:

| Condition | Effect on SDR | Diagnostic signal |
|-----------|--------------|-------------------|
| Missed samples | Phase discontinuities, spectrum artifacts, broken demodulation | `dma_buf_count` delta vs expected |
| Clock drift | Frequency offset in demodulated signals, IQ imbalance | Measured rate vs programmed rate |
| PLL unlock | Total garbage data, noise floor appears to rise to 0 dBFS | `si5351_status` bit 5 |
| USB overrun | Dropped sample blocks, periodic clicks in audio | `pib_overrun_count` increasing |
| USB link errors | Bit errors in sample data, CRC failures | `usb_phy_errors`, `usb_link_errors` |
| GPIF stall | Stream stops, application hangs | `gpif_state` stuck in BUSY/WAIT |
| Streaming fault | Gap in sample stream | `streaming_faults` increasing |

A host application like HDSDR or SDR# (via ExtIO) could use this
telemetry to:

1. **Display a health indicator** (green/yellow/red) in the UI
2. **Annotate the waterfall** with markers where data integrity is
   compromised
3. **Automatically pause recording** when sample integrity is lost
4. **Log diagnostic events** for post-analysis of reception quality
5. **Trigger re-initialization** of the ADC clock when PLL unlock is
   detected

---

## Firmware implementation sketch (for future DIAGFX3 extension)

> The code below is a **reference sketch** for the proposed DIAGFX3
> superset command.  For the implemented GETSTATS handler, see
> `SDDC_FX3/USBHandler.c` (the `case GETSTATS:` block).

### Snapshot function

```c
void fx3_diag_snapshot(fx3_diag_t *diag)
{
    diag->dma_buf_count   = glDMACount;
    diag->timestamp_ms    = CyU3PGetTime();

    diag->pib_overrun_count  = glPibOverrunCount;   /* new global */
    diag->pib_underrun_count = glPibUnderrunCount;  /* new global */
    diag->streaming_faults   = glCounter[2];         /* EP underruns + watchdog */

    CyU3PUsbGetErrorCounts(&diag->usb_phy_errors,
                           &diag->usb_link_errors);

    CyU3PGpifGetSMState(&diag->gpif_state);

    uint8_t si_status = 0;
    I2cTransfer(0x00, 0xC0, 1, &si_status, CyTrue);
    diag->si5351_status = si_status;

    diag->streaming      = glIsApplnActive ? 1 : 0;
    diag->recovery_count = glRecoveryCount;         /* new global */
}
```

### Vendor request handler addition

```c
case DIAGFX3:  /* 0xB7 */
    if (bReqType == 0xC0) {  /* vendor, device-to-host */
        fx3_diag_t diag;
        fx3_diag_snapshot(&diag);
        CyU3PMemCopy(glEp0Buffer, (uint8_t *)&diag, sizeof(diag));
        CyU3PUsbSendEP0Data(sizeof(diag), glEp0Buffer);
        isHandled = CyTrue;
    }
    break;
```

### Host-side query (libusb)

```c
fx3_diag_t diag;
int ret = libusb_control_transfer(dev,
    LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
    0xB7,           /* DIAGFX3 */
    0, 0,           /* wValue, wIndex */
    (uint8_t *)&diag,
    sizeof(diag),
    1000);          /* timeout ms */
```

---

## Cost and constraints

**Actual cost (GETSTATS, implemented):**
- Handler runs inline in the EP0 callback — one `CyU3PGpifGetSMState()`
  call, three `memcpy` operations, and one I2C register read (~50 µs)
- 26-byte EP0 response per poll; at 10 polls/sec: 260 bytes/sec
- Counters reuse the existing `glCounter[20]` array — zero additional BSS

**Projected cost (DIAGFX3 extension, if implemented):**
- `fx3_diag_snapshot()` would take ~50 us (dominated by the I2C read of
  Si5351 status and the `CyU3PUsbGetErrorCounts` call)
- Called only on host request via EP0, not in the streaming hot path
- No impact on bulk transfer throughput
- ~6 additional global counters (24 bytes of BSS)
- `fx3_diag_t` structure (24 bytes on stack during EP0 handler)
- Negligible compared to 512 KB SRAM

**I2C bus contention:**
- The Si5351 register read shares the I2C bus with other operations
- During streaming, no other I2C traffic is expected (attenuator and
  VGA use GPIO bit-bang, not I2C)
- If contention is a concern, the Si5351 status read can be made
  conditional (only read every Nth poll, or only when `dma_buf_count`
  delta is anomalous)

---

## Appendix A: Build system and SDK compatibility

### What you need to build

All diagnostic features described in this document are **application-level
code changes** that link against the existing, unchanged SDK libraries.
The build requires:

1. **`arm-none-eabi-gcc`** cross-compiler (ARM926EJ-S target, thumb-interwork)
2. **`make`**
3. **A host GCC** to compile the `elf2img` utility from `SDK/util/elf2img/elf2img.c`

The build flow is:

```
arm-none-eabi-gcc  →  SDDC_FX3.elf  →  elf2img  →  SDDC_FX3.img
```

### SDK libraries are pre-compiled, not rebuilt

The `.a` files in `SDK/fw_lib/1_3_4/fx3_release/` are pre-compiled
ARM static archives (`elf32-littlearm`, architecture `armv5tej`).
Cypress/Infineon does not provide source code for these libraries.
They cannot be rebuilt -- only replaced with copies from a fresh SDK
install.

The repo contains a cherry-picked subset of the full FX3 SDK 1.3.4
(build 40):

| Component | Path | Purpose |
|-----------|------|---------|
| Pre-compiled libraries | `SDK/fw_lib/1_3_4/fx3_release/*.a` | 4 linked: `libcyfxapi.a`, `libcyu3threadx.a`, `libcyu3lpp.a`, `libcyu3sport.a` |
| SDK headers | `SDK/fw_lib/1_3_4/inc/` | API declarations |
| Build config | `SDK/fw_build/fx3_fw/` | Makefiles, linker scripts (`fx3.ld`), startup assembly |
| elf2img tool | `SDK/util/elf2img/elf2img.c` | Converts ELF to FX3 bootable `.img` |

### No SDK library changes needed

Every API used by the diagnostic features already exists in the
current 1.3.4 libraries:

| API | Library | Status |
|-----|---------|--------|
| `CyU3PGetTime()` | libcyu3threadx.a | Exists |
| `CyU3PUsbGetErrorCounts()` | libcyfxapi.a | Exists |
| `CyU3PGpifGetSMState()` | libcyfxapi.a | Exists |
| `CyU3PPibRegisterCallback()` | libcyfxapi.a | Exists |
| `CyU3PGpifDisable()` | libcyfxapi.a | Exists |
| `CyU3PDmaMultiChannelReset()` | libcyfxapi.a | Exists |
| `CyU3PUsbFlushEp()` | libcyfxapi.a | Exists |
| `CyU3PUsbSendEP0Data()` | libcyfxapi.a | Exists |
| `I2cTransfer()` | Application code (i2cmodule.c) | Exists |

The GETSTATS vendor command (implemented) and the proposed DIAGFX3
extension are application-level additions to `.c` files, linked
against the unchanged libraries.

### If installing a fresh FX3 SDK

If the full Infineon/Cypress FX3 SDK is installed (e.g., for the
GPIF II Designer tool or documentation), the following applies:

1. **Version match**: The repo is pinned to `CYSDKVERSION=1_3_4` in
   the makefile.  If a newer SDK version is installed, either update
   `CYSDKVERSION` or copy the new libraries into the existing
   `1_3_4/` directory structure.

2. **ABI compatibility**: Minor SDK version bumps (1.3.x) are
   backward-compatible at the API level.  A hypothetical 1.4.x or
   2.x release could break ABI.

3. **Library link order**: The makefile specifies a strict link order
   required by the GNU linker: `libcyu3sport.a` → `libcyu3lpp.a` →
   `libcyfxapi.a` → `libcyu3threadx.a` → `libc.a` → `libgcc.a`.
   A fresh SDK install's libraries slot in identically.

4. **`cyfxtx.c` template**: The memory allocator and exception
   handlers (`SDDC_FX3/cyfxtx.c`) are an SDK-provided template
   compiled with the application.  A new SDK may ship an updated
   version -- diff before replacing.

---

## Appendix B: GPIF state machine scope

### What does NOT require GPIF state machine changes

All diagnostic and telemetry features in this document work by
**observing** the existing state machine from the outside:

- `CyU3PGpifGetSMState()` reads the current state index (0-9)
  without affecting the state machine
- `glDMACount` is incremented in the DMA produce-event callback,
  independent of the GPIF configuration
- PIB error callbacks fire on DMA-level errors detected by the
  hardware, not by the state machine
- The GETSTATS vendor command (and any future DIAGFX3 extension) is
  an EP0 control transfer, completely independent of the bulk
  streaming GPIF path

The existing `SDDC_GPIF.h` (generated by GPIF II Designer) does not
need to be modified for any feature described in this document.

### What required GPIF state machine changes (now implemented)

**Status: implemented.**  See
[gpif-and-recovery.md](gpif-and-recovery.md) for the full design.

The GPIF state machine was updated to add `!FW_TRG → IDLE` exit
transitions to TH0_RD (state 2), TH0_WAIT (state 9), and TH1_WAIT
(state 8) — three states that each had one free transition slot.
This enables reliable soft-stop via `CyU3PGpifDisable(CyFalse)`
within 3 clock cycles (~47 ns at 64 MHz).

The changes were made in the GPIF II Designer project
(`SDDC_FX3/SDDC_GPIF/projectfiles/gpif2model.xml`) and regenerated
into `SDDC_FX3/SDDC_GPIF.h`.  Soak testing (500+ cycles) confirmed
zero device crashes with the soft-stop architecture.

**Note**: The diagnostic side-channel features (GETSTATS, PIB
callback, DMA monitoring) do NOT require any GPIF state machine
changes.  They observe the existing SM from the outside.  The
soft-stop change is a separate reliability improvement that reduced
the streaming fault rate visible in GETSTATS.
