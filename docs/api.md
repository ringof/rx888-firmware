---
title: USB API reference
nav_order: 2
permalink: /api/
description: Complete USB vendor-command protocol for the RX888mk2 FX3 firmware - STARTFX3, STOPFX3, GETSTATS 26-byte layout, SETARGFX3, GPIO bitmap, Si5351 I2C.
---

# USB API reference
{: .no_toc }

Complete USB vendor-command protocol exposed by the rx888-firmware FX3
image.  Every claim on this page cites a source file and line in the
firmware tree; if behavior differs between this page and the source, the
source is authoritative — please open an issue.

Reference firmware version: **2.3**
(`FIRMWARE_VER_MAJOR=2`, `FIRMWARE_VER_MINOR=3` —
`SDDC_FX3/protocol.h:10-11`).

1. TOC
{:toc}

---

## USB device identity

The FX3 enumerates with two different PIDs depending on whether the
firmware has been uploaded to RAM:

| State            | VID    | PID    | Product string | Notes |
|------------------|--------|--------|----------------|-------|
| Bootloader (cold-boot, no firmware) | `04b4` | `00f3` | (FX3 BootROM)  | Host uses this PID to upload the firmware image with the standard FX3 BootROM protocol; see [Building]({{ '/building/' | relative_url }}). PID value from the `rx888_tools` udev rule referenced in `README.md`. |
| Programmed (firmware running)       | `04b4` | `00f1` | `RX888mk2`     | VID/PID from `SDDC_FX3/USBDescriptor.c:27-28` (SS) and `:46-47` (HS); product string from `SDDC_FX3/USBDescriptor.c:218-224`. |

The serial number string is generated at boot from the FX3 die ID
(`SDDC_FX3/USBDescriptor.c:274-294`) — 16 hex characters, stable for a
given device.

## USB endpoints

The firmware exposes a single vendor-specific interface with one bulk IN
endpoint plus EP0 for control transfers.  Endpoint address and packet
sizes come from `SDDC_FX3/USBDescriptor.c` and `SDDC_FX3/Application.h`.

| Speed        | Endpoint           | Type            | Max packet | Burst | Source |
|--------------|--------------------|-----------------|------------|-------|--------|
| SuperSpeed   | `0x81` (EP1 IN)    | Bulk, IQ stream | 1024 B     | 16    | `USBDescriptor.c:118-131`, `Application.h:64-65` |
| High Speed   | `0x81` (EP1 IN)    | Bulk            | 512 B      | n/a   | `USBDescriptor.c:158-164` |
| Full Speed   | `0x81` (EP1 IN)    | Bulk            | 64 B       | n/a   | `USBDescriptor.c:191-197` |
| Any          | EP0                | Control         | 512 B (SS) / 64 B (HS,FS) | — | `USBDescriptor.c:26`, `:45` |

EP0 vendor requests are limited to **64 bytes of data-phase payload**
(`CYFX_SDRAPP_MAX_EP0LEN`, `SDDC_FX3/USBHandler.c:47`).  Requests with
`wLength > 64` are rejected with a STALL on EP0 before any data phase
runs (`USBHandler.c:187-193`).

The streaming IQ pipeline uses 4 DMA buffers of 16 KB each
(`Application.h:41-44`), assembled in a multi-channel ping-pong from
two PIB sockets to USB socket 1.  The IQ data is **16-bit signed,
little-endian, single-channel** (the LTC2208 ADC output sign-extended
to 16 bits; randomization can be enabled via the `RAND` GPIO bit — see
[`GPIOFX3`](#gpiofx3)).

## Vendor command summary

All vendor commands target `bmRequestType` with `Type=Vendor`,
`Recipient=Device`.  Command code is `bRequest`.  Source of truth:
`enum FX3Command` in `SDDC_FX3/protocol.h:14-41` and the dispatch in
`SDDC_FX3/USBHandler.c:195-528`.

| `bRequest` | Name              | Dir   | Data length | Brief                                          |
|------------|-------------------|-------|-------------|------------------------------------------------|
| `0xAA`     | [`STARTFX3`](#startfx3) | OUT | 0 | Start GPIF engine; begin IQ stream on EP1.    |
| `0xAB`     | [`STOPFX3`](#stopfx3)   | OUT | 0 | Stop GPIF engine; drain DMA.                  |
| `0xAC`     | [`TESTFX3`](#testfx3)   | IN  | 4 | Read hardware/firmware version + counters.    |
| `0xAD`     | [`GPIOFX3`](#gpiofx3)   | OUT | 4 | Set GPIO control word.                        |
| `0xAE`     | [`I2CWFX3`](#i2cwfx3)   | OUT | n | Write `wLength` bytes to I2C address `wIndex` at register `wValue`. |
| `0xAF`     | [`I2CRFX3`](#i2crfx3)   | IN  | n | Read `wLength` bytes from I2C address `wIndex` at register `wValue`. |
| `0xB1`     | [`RESETFX3`](#resetfx3) | OUT | 0 | Reset FX3 to BootROM (PID `00f3`).            |
| `0xB2`     | [`STARTADC`](#startadc) | OUT | 4 | Program Si5351 to ADC clock frequency (Hz).   |
| `0xB3`     | [`GETSTATS`](#getstats) | IN  | ≤64 | Diagnostic counters (see byte layout below). |
| `0xB6`     | [`SETARGFX3`](#setargfx3) | OUT | n | Set parameter `wIndex` to value `wValue`.  |
| `0xB7`     | [`SYNTH_PPS`](#synth_pps) | —   | 0 | **Diagnostic.** Software-driven in-band PPS marker (issue #125). |
| `0xBA`     | [`READINFODEBUG`](#readinfodebug) | IN/OUT | ≤64 | Debug console RX (wValue=ASCII) / TX. |
| `0xCE`     | [`HANGFX3`](#hangfx3)   | —   | 0 | **Test-only.** Sleep `wValue` ms in EP0 handler. |
| `0xCF`     | [`HANGMAIN`](#hangmain) | —   | 0 | **Test-only.** Arm main-thread infinite spin. |

> **Direction is from the host's perspective.** `OUT` = data flows
> host → device; `IN` = device → host; `—` = no data phase (status only).

---

## Command details

### STARTFX3

Source: `SDDC_FX3/USBHandler.c:332-393`.

Starts the GPIF state machine and the PIB → USB DMA pipeline.  Once
`STARTFX3` returns success, the device begins producing IQ samples on
EP1 IN (`0x81`).

| Field         | Value               |
|---------------|---------------------|
| `bRequest`    | `0xAA`              |
| `bmRequestType` | `0x40` (OUT, Vendor, Device) |
| `wValue`      | 0 (ignored)         |
| `wIndex`      | 0 (ignored)         |
| `wLength`     | 0                   |

**Behavior:**

1. Disables USB Link Power Management for the duration of the stream
   (`USBHandler.c:333`).
2. **Preflight:** verifies the Si5351 ADC clock is running and locked
   via `GpifPreflightCheck()` (`USBHandler.c:343-349`).  If not, the
   status phase is STALLed.  Sequence requirement: host must call
   [`STARTADC`](#startadc) (with a non-zero frequency) **before**
   `STARTFX3`.
3. Force-stops any running GPIF SM, resets the multi-channel DMA, and
   flushes the consumer endpoint (`USBHandler.c:356-362`).
4. Reloads the GPIF waveform and re-arms the SM.

**Failure mode:** any internal failure (preflight, DMA setup, or GPIF
load) results in a STALL on the status phase
(`USBHandler.c:343-348, :388`).  Streaming will not start.  No retry
is performed inside the firmware — the host must STALL-clear,
diagnose, and re-issue.

### STOPFX3

Source: `SDDC_FX3/USBHandler.c:395-424`.

Stops the GPIF state machine cleanly and drains the DMA pipeline.

| Field         | Value               |
|---------------|---------------------|
| `bRequest`    | `0xAB`              |
| `bmRequestType` | `0x40`            |
| `wValue`      | 0 (ignored)         |
| `wIndex`      | 0 (ignored)         |
| `wLength`     | 0                   |

**Behavior:** deasserts the `FW_TRG` input, sleeps ~1 ms for the SM to
quiesce, disables GPIF (soft if SM reached IDLE, force otherwise),
resets the DMA channel, and flushes the consumer endpoint
(`USBHandler.c:404-418`).  Re-enables LPM (`:396`).

**Always succeeds from the host's perspective** — the firmware does
not STALL even on an unclean stop; the diagnostic counter `glCounter[1]`
is bumped instead.  Verify cleanly via [`GETSTATS`](#getstats) byte
[8] (GPIF SM state == `1` IDLE).

### TESTFX3

Source: `SDDC_FX3/USBHandler.c:433-442`.

Reads back the hardware-config byte, firmware version, and vendor
request counter.  Side effect: setting `wValue=1` enables debug-flag
output to the host-side debug console.

| Field         | Value               |
|---------------|---------------------|
| `bRequest`    | `0xAC`              |
| `bmRequestType` | `0xC0` (IN, Vendor, Device) |
| `wValue`      | 0 (normal) or 1 (set debug flag) |
| `wIndex`      | 0 (ignored)         |
| `wLength`     | 4                   |

**Response (4 bytes):**

| Byte | Field         | Source |
|------|---------------|--------|
| 0    | `glHWconfig`  | `RX888r2 = 0x04`, `NORADIO = 0x00` (`protocol.h:75-78`) |
| 1    | `FIRMWARE_VER_MAJOR` | `protocol.h:10` |
| 2    | `FIRMWARE_VER_MINOR` | `protocol.h:11` |
| 3    | `glVendorRqtCnt`     | Wraps at 256.  Useful as a liveness probe across calls. |

### GPIOFX3

Source: `SDDC_FX3/USBHandler.c:197-204`, dispatched to
`rx888r2_GpioSet()` in `SDDC_FX3/radio/rx888r2.c:17-27`.

Sets the device GPIO control word in a single 32-bit OUT data phase.
Only the bits described in the [GPIO bit map](#gpio-bit-map) below are
acted on; other bits are ignored.

| Field         | Value               |
|---------------|---------------------|
| `bRequest`    | `0xAD`              |
| `bmRequestType` | `0x40`            |
| `wValue`      | 0 (ignored)         |
| `wIndex`      | 0 (ignored)         |
| `wLength`     | 4                   |
| Data          | 32-bit little-endian control word |

`GPIOFX3` is the host's primary lever for controlling LEDs, the front-
end shutdown line, the ADC dither/randomization options, bias-tee
power, and front-end path selection.

### I2CWFX3

Source: `SDDC_FX3/USBHandler.c:281-294`.

Writes to a peripheral on the FX3 I2C bus.  Used by host applications
to drive any I2C device on the RX888mk2 — Si5351 clock generator,
R828D tuner registers, etc.

| Field         | Value                                                  |
|---------------|--------------------------------------------------------|
| `bRequest`    | `0xAE`                                                 |
| `bmRequestType` | `0x40`                                               |
| `wValue`      | Slave-device register address (subaddress); passed as the I2C `regAddr` field |
| `wIndex`      | I2C slave address shifted into the I2C controller's address register (see firmware's `I2cTransfer()`) |
| `wLength`     | Bytes to write (≤ 64)                                 |
| Data          | Payload                                                |

The I2C controller is configured for 7-bit addresses; consult
`SDDC_FX3/i2cmodule.c` for the exact subaddress encoding if your host
target needs multi-byte sub-addresses.  STALLs the status phase if the
I2C transaction returns an SDK error
(`USBHandler.c:291`).

### I2CRFX3

Source: `SDDC_FX3/USBHandler.c:296-304`.

Reads from a peripheral on the FX3 I2C bus.

| Field         | Value                                                  |
|---------------|--------------------------------------------------------|
| `bRequest`    | `0xAF`                                                 |
| `bmRequestType` | `0xC0`                                               |
| `wValue`      | Slave-device register address (subaddress)             |
| `wIndex`      | I2C slave address (see `I2CWFX3` note)                 |
| `wLength`     | Bytes to read (≤ 64)                                  |

Response: `wLength` bytes from the slave.  If the I2C transaction
fails, no data is sent and the status phase is unhandled (host should
treat as a transient error and retry).

### RESETFX3

Source: `SDDC_FX3/USBHandler.c:426-431`.

Hard-resets the FX3.  The host-side debug message
`"HOST RESETTING CPU"` is printed, the firmware sleeps 100 ms, then
calls `CyU3PDeviceReset(CyFalse)`.  After this command the device
re-enumerates as VID/PID `04b4:00f3` (BootROM); the host must re-upload
the firmware image to restore normal operation.

| Field         | Value               |
|---------------|---------------------|
| `bRequest`    | `0xB1`              |
| `bmRequestType` | `0x40`            |
| `wValue`      | 0 (ignored)         |
| `wIndex`      | 0 (ignored)         |
| `wLength`     | 0                   |

### STARTADC

Source: `SDDC_FX3/USBHandler.c:206-253`.

Programs the Si5351 ADC clock to the specified frequency in Hz.  Sent
before `STARTFX3` to bring the LTC2208 sample clock up to the desired
rate.

| Field         | Value                                                  |
|---------------|--------------------------------------------------------|
| `bRequest`    | `0xB2`                                                 |
| `bmRequestType` | `0x40`                                               |
| `wValue`      | 0 (ignored)                                            |
| `wIndex`      | 0 (ignored)                                            |
| `wLength`     | 4                                                      |
| Data          | 32-bit little-endian frequency in Hz                   |

**Behavior:** if the GPIF SM is currently running, the firmware
implicitly stops it before reprogramming the clock (changing the
Si5351 output while the SM is running wedges the PIB)
(`USBHandler.c:217-227`).  After programming, the firmware polls
Si5351 PLL lock for up to 100 ms before returning
(`USBHandler.c:240-246`).

**Failure mode:** if `si5351aSetFrequencyA` returns a non-success
status, the status phase is left unhandled and a debug message is
emitted (`USBHandler.c:248-251`).

### GETSTATS

Source: `SDDC_FX3/USBHandler.c:256-289`.

Reads back diagnostic counters and state.  The firmware fills a
fixed-layout buffer of 34 bytes total.

| Field         | Value               |
|---------------|---------------------|
| `bRequest`    | `0xB3`              |
| `bmRequestType` | `0xC0`            |
| `wValue`      | 0 (ignored)         |
| `wIndex`      | 0 (ignored)         |
| `wLength`     | ≤ 64 (firmware sends 34) |

**Byte layout** (little-endian for multi-byte fields):

| Offset | Width | Name              | Description |
|--------|-------|-------------------|-------------|
| 0–3    | u32   | `glDMACount`      | DMA completion count since the most recent `STARTFX3`. Reset to 0 on `STARTFX3`/`STOPFX3`. (`USBHandler.c:375`, `:419`.) |
| 4      | u8    | `gpifState`       | GPIF state machine state at the moment the request was serviced.  IDLE = 1.  255 means `CyU3PGpifGetSMState` failed. |
| 5–8    | u32   | `glCounter[0]`    | Free-running counter incremented in the main loop; useful as a liveness probe. |
| 9–10   | u16   | `glLastPibArg`    | Last argument captured from the PIB error path (see `gpif-and-recovery.md`). |
| 11–14  | u32   | `glCounter[1]`    | Counter 1 — currently used for unclean-stop events. |
| 15–18  | u32   | `glCounter[2]`    | Counter 2 — EP underrun count (`USBHandler.c:563`). |
| 19     | u8    | Si5351 reg 0      | Live read of Si5351 status register (PLL lock bits).  Useful for confirming the ADC clock health without separately issuing `I2CRFX3`. (`USBHandler.c:271`.) |
| 20–23  | u32   | `boot_count`      | Increments once per firmware `health_init()` call.  Use to detect mid-test resets: snapshot before, compare after; mismatch means the device reset.  (`USBHandler.c:275`, `SDDC_FX3/health.c:health_boot_count`.) |
| 24     | u8    | Si5351 CLK0_CONTROL (reg 16) | Live I2C read of the Si5351 CLK0 output-driver control register.  Bit 7 is `CLK0_PDN` — set means CLK0 powered down, clear means CLK0 enabled.  Returns `0xFF` if the I2C read fails.  (`USBHandler.c:283-285`.) |
| 25     | u8    | `clk0_result`     | Result of the firmware's `si5351_clk0_enabled()` query: `1` = CLK0 enabled (bit 7 clear and I2C read succeeded), `0` = disabled or I2C error.  This is the same value `GpifPreflightCheck()` consults at [`STARTFX3`](#startfx3) time.  (`USBHandler.c:286`.) |
| 26–29  | u32   | `glPpsCount`      | Successful partial-commit count emitted by the synthetic-PPS module since boot (issue #125).  Bumped on each successful [`SYNTH_PPS`](#synth_pps) wrap-up. |
| 30–33  | u32   | `glPpsCommitFailCount` | Wrap-up attempts where both producer sockets refused the commit — typically because streaming is idle or the channel is between buffers.  Expected to stay 0 during active streaming. |

The firmware sends exactly 34 bytes via `CyU3PUsbSendEP0Data`; hosts
should request `wLength=34` (or up to 64) and read the prefix that
fits.  Older host tools that requested `wLength=26` or `wLength=24`
continue to work — the firmware returns only the prefix requested,
omitting the newer fields.

### SETARGFX3

Source: `SDDC_FX3/USBHandler.c:305-330`.

Generic "set parameter by index" sidedoor.  `wIndex` selects which
parameter; `wValue` is the new value.  A small EP0 data phase is sent
but its contents are ignored — `wValue` carries the data.

| Field         | Value               |
|---------------|---------------------|
| `bRequest`    | `0xB6`              |
| `bmRequestType` | `0x40`            |
| `wValue`      | New parameter value |
| `wIndex`      | Parameter selector (see below) |
| `wLength`     | n (sent but ignored — host may send 0 or a small payload; payload bytes are discarded) |

Defined parameter selectors (`enum ArgumentList` in
`SDDC_FX3/protocol.h:80-84`):

| `wIndex` | Name           | `wValue` range            | Effect | Source |
|----------|----------------|---------------------------|--------|--------|
| 10       | `DAT31_ATT`    | 0 – 63 (6-bit, 0.5 dB)    | Sets the DAT-31 step attenuator.  6 bits shifted to `ATT_LE`. | `rx888r2_SetAttenuator()`, `radio/rx888r2.c:78-82` |
| 11       | `AD8370_VGA`   | 0 – 255 (raw 8-bit register) | Sets the AD8370 VGA gain register.  See AD8370 datasheet for the gain-code mapping. | `rx888r2_SetGain()`, `radio/rx888r2.c:84-89` |
| 14       | `WDG_MAX_RECOV` | 0 (unlimited) – 255      | Caps consecutive watchdog recoveries.  Default `WDG_MAX_RECOVERY_DEFAULT = 5` (`protocol.h:86`).  After the cap, the firmware reboots instead of attempting another recovery — used by host-side tests to bound flakiness. | `protocol.h:83`, `USBHandler.c:318-322` |

Unrecognized `wIndex` values are signaled by STALLing the status phase
(`USBHandler.c:323-328`).  Recognized calls bump `glVendorRqtCnt`.

> Note: protocol-numbered selectors that are absent from the table
> (e.g. 0–9, 12–13) are **not** currently dispatched by the firmware.
> They are reserved for future use and will STALL.

### SYNTH_PPS

> **Diagnostic, permanent — safe in production.**  Modeled on the
> `HANGFX3` / `HANGMAIN` permanence pattern.

Source: `SDDC_FX3/USBHandler.c` (`case SYNTH_PPS`) and `SDDC_FX3/synth_pps.c`.

Software-driven in-band PPS marker (issue #125).  When started, fires
`CyU3PDmaMultiChannelSetWrapUp()` on the active producer socket of the
streaming DMA channel at the configured cadence.  Each successful
wrap-up closes the current DMA buffer with whatever sample bytes it
holds, producing a short USB bulk transfer on EP1 IN (`actual_length <
length`) that the host can use as an in-band sample-stream marker.

The mechanism is independent of any GPIF state-machine change and
needs no external hardware; the companion hardware-PPS issue tracks
the parallel external-edge path.

| Field         | Value                                |
|---------------|--------------------------------------|
| `bRequest`    | `0xB7`                               |
| `bmRequestType` | `0x40`                             |
| `wValue`      | Action: `0` stop, `1` start, `2` oneshot |
| `wIndex`      | Period in ms (only when `wValue=1`)  |
| `wLength`     | 0                                    |

**Actions:**

- `wValue=0` — stop the periodic timer.  ACK; STALL only on SDK failure.
- `wValue=1` — start the periodic timer.  `wIndex=0` selects the
  default 1000 ms period; otherwise `wIndex` must be in `[10, 60000]`
  milliseconds (ACK on success; STALL on out-of-range or SDK failure).
- `wValue=2` — fire one immediate wrap-up.  ACK even when streaming is
  idle (the request was valid; the absence of a commit shows up as
  `glPpsCommitFailCount++` in `GETSTATS` rather than as an EP0 STALL).
- `wValue ≥ 3` — STALL (invalid action).

**Observability** — counters in `GETSTATS`:

- offset 26–29 `glPpsCount` — successful wrap-ups since boot.
- offset 30–33 `glPpsCommitFailCount` — wrap-up attempts where both
  producer sockets refused.  Expected to stay 0 during active
  streaming; non-zero typically means the timer fired between buffers
  or while streaming was stopped.

**Host-side detection of the marker** is by reading
`xfer->actual_length < xfer->length` on each EP1 IN bulk URB
completion.  See `tests/rx888_tools/pps_probe.c` (planned) for a
canonical libusb-only consumer.  The marker's position in the sample
stream is `total_samples + actual_length / 2` (16-bit samples).

### READINFODEBUG

Source: `SDDC_FX3/USBHandler.c:481-520`.

Bidirectional channel for the firmware's debug console — used by
`rx888_tools` and host test code to receive log lines and to inject
single-character commands.

| Field         | Value                                                  |
|---------------|--------------------------------------------------------|
| `bRequest`    | `0xBA`                                                 |
| `bmRequestType` | `0xC0` (IN — see notes)                              |
| `wValue`      | 0 (poll for output) or ASCII char to push as input    |
| `wIndex`      | 0 (ignored)                                            |
| `wLength`     | Up to 64; firmware returns ≤ `MAXLEN_D_USB - 1 = 255` bytes total, paginated 63 bytes at a time |

**Input direction:** `wValue` non-zero is interpreted as the ASCII
code of one input character pushed to the firmware console.  `0x0D`
(`\r`) signals "command complete" and triggers command execution.
Other values are appended to the input buffer.

**Output direction:** every call drains up to 63 bytes of pending
debug text into the response buffer (terminated with `\0`,
total ≤ 64 bytes).  If no debug text is queued, the firmware STALLs
the status phase — host loops typically poll on a short interval
until non-empty output appears.

Tracing of this command is suppressed in the firmware trace stream
(see `TraceSerial()` in `USBHandler.c:64`) — otherwise polling would
flood the trace buffer.

### HANGFX3

> **Test-only.** Safe in production: the health watchdog will recover
> within a few seconds.  Do not invoke from normal host code.

Source: `SDDC_FX3/USBHandler.c:451-462`.

Deterministically wedges the EP0 vendor-request handler by sleeping
`wValue` milliseconds inside the handler thread.  Used by the host-side
`test_health_recovery` scenario in `tests/fx3_cmd.c` to validate the
Level-4 recovery cascade (full device reset on EP0 lockup, issues #104
and #105).

| Field         | Value               |
|---------------|---------------------|
| `bRequest`    | `0xCE`              |
| `bmRequestType` | `0x40`            |
| `wValue`      | Hang duration in ms |
| `wIndex`      | 0 (ignored)         |
| `wLength`     | 0                   |

If `wValue ≥ EP0_HANDLER_TIMEOUT_MS` (2000 ms, `SDDC_FX3/health.c:30`)
the health watchdog will fire and reset the device before the sleep
returns.  If `wValue` is shorter, the sleep completes and the firmware
ACKs the setup as normal.

### HANGMAIN

> **Test-only.** Safe in production: the FX3 hardware watchdog will
> hard-reset the device within ~5 s.  Do not invoke from normal host
> code.

Source: `SDDC_FX3/USBHandler.c:473-478`.

Arms the main thread to enter an infinite spin on its next iteration.
The EP0 setup is ACKed immediately so the host sees a clean reply;
the main thread freezes shortly after, the heartbeat stops, the
WD-clear timer stops petting the FX3 HWDT, and the HWDT fires at
`HWDT_PERIOD_MS` (5000 ms, `SDDC_FX3/health.c:53`) — hard-resetting
the device.  Used by the host-side `test_main_recovery` scenario to
validate Level-5 of the recovery cascade.

| Field         | Value               |
|---------------|---------------------|
| `bRequest`    | `0xCF`              |
| `bmRequestType` | `0x40`            |
| `wValue`      | 0 (ignored)         |
| `wIndex`      | 0 (ignored)         |
| `wLength`     | 0                   |

---

## GPIO bit map

The 32-bit control word for [`GPIOFX3`](#gpiofx3).  Pin assignments
come from `enum GPIOPin` in `SDDC_FX3/protocol.h:62-73`.  Bits not
listed here are ignored (no FX3 GPIO is wired up for them).

| Bit | Mask         | Constant   | Function                              | Notes |
|-----|--------------|------------|---------------------------------------|-------|
| 5   | `0x00000020` | `SHDWN`    | Front-end shutdown line               | Active-high in the control word; drives the SHDN pin. |
| 6   | `0x00000040` | `DITH`     | LTC2208 dither enable                 | Enables ADC dither for SFDR improvement. |
| 7   | `0x00000080` | `RANDO`    | LTC2208 output randomization          | When set, ADC output is randomized — host must un-randomize before processing. |
| 8   | `0x00000100` | `BIAS_HF`  | HF antenna-port bias-tee power        | |
| 9   | `0x00000200` | `BIAS_VHF` | VHF antenna-port bias-tee power       | |
| 11  | `0x00000800` | `LED_BLUE` | Blue front-panel LED                  | Used as a liveness/activity indicator. |
| 13  | `0x00002000` | `ATT_SEL0` | Attenuator bank select bit 0          | Front-end RF path / attenuator selection. |
| 14  | `0x00004000` | `ATT_SEL1` | Attenuator bank select bit 1          | |
| 15  | `0x00008000` | `VHF_EN`   | VHF front-end enable                  | Selects the VHF path through the R828D tuner.  See the **scope and limitations** note on the [home page]({{ '/' | relative_url }}). |
| 16  | `0x00010000` | `PGA_EN`   | PGA enable (active-low)               | The firmware inverts this bit: `mdata & PGA_EN` clears the FX3 GPIO output (`radio/rx888r2.c:25`). |

> **Sticky note for host integrators.** The firmware reads each bit
> independently — it does not maintain a per-call delta against a
> shadow register.  Every `GPIOFX3` call sets the *full* GPIO state,
> so each call must include the bits for any feature that should stay
> on.

---

## SETARGFX3 parameter list

Quick reference of the [`SETARGFX3`](#setargfx3) `wIndex` values
recognized by this firmware.  Values not listed here STALL the status
phase.

| `wIndex` | Constant         | `wValue` range            | Default |
|----------|------------------|---------------------------|---------|
| 10       | `DAT31_ATT`      | 0–63                      | (unset — device boots with attenuator latched to whatever was last written) |
| 11       | `AD8370_VGA`     | 0–255 (8-bit register)    | (unset) |
| 14       | `WDG_MAX_RECOV`  | 0 = unlimited; 1–255      | 5 (`WDG_MAX_RECOVERY_DEFAULT`, `protocol.h:86`) |

---

## Test-only command behavior summary

The two test commands ([`HANGFX3`](#hangfx3), [`HANGMAIN`](#hangmain))
exist to validate the firmware's recovery layers and are safe to
invoke against production firmware — the device self-recovers.  The
README's "Firmware Robustness" section has the full cascade
description.

| Command  | Triggers recovery layer   | Latency to recovery | Test scenario                         |
|----------|---------------------------|---------------------|---------------------------------------|
| `HANGFX3`| Level 4 (`CyU3PDeviceReset`) | ~ `EP0_HANDLER_TIMEOUT_MS` (2 s) + re-enumeration | `tests/fx3_cmd.c test_health_recovery` |
| `HANGMAIN`| Level 5 (FX3 HWDT)        | ~ `HWDT_PERIOD_MS` (5 s) | `tests/fx3_cmd.c test_main_recovery`  |

---

## Versioning

Firmware version is exposed two ways:

1. As the `glFWconfig` 16-bit field returned by [`TESTFX3`](#testfx3),
   high byte = major, low byte = minor.
2. As `FIRMWARE_VER_MAJOR` and `FIRMWARE_VER_MINOR` macros in
   `SDDC_FX3/protocol.h:10-11`.

Releases are tagged `vMAJOR.MINOR[.patch][-suffix]` on GitHub; see
[Releases](https://github.com/ringof/rx888-firmware/releases).
