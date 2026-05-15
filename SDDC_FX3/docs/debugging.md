# FX3 Firmware Debug Reference

The SDDC_FX3 firmware provides two debug channels, an interactive
console, and a real-time vendor-request trace.  All are available on
the RX888mk2 without hardware modifications.

---

## 1. Debug Channels

There are two independent output paths.  Only one is active at a time,
selected at compile time.

### 1.1 Compile-Time Selection

Controlled by `_DEBUG_USB_` in `protocol.h`:

| `_DEBUG_USB_` | `DebugPrint()` expands to | Output path |
|---------------|---------------------------|-------------|
| Defined (default) | `DebugPrint2USB()` | USB EP0 via `READINFODEBUG` |
| Not defined | `CyU3PDebugPrint()` | UART TX at 115200 baud |

Console input works on both paths: UART RX callback or host-sent
characters in the `READINFODEBUG` vendor request `wValue` field.

### 1.2 UART Debug Console

#### Hardware

- UART TX/RX pins are active whenever `InitializeDebugConsole()` runs
  (always -- called early in `CyFxApplicationDefine`)
- 115200 baud, 8N1, no flow control
- Directly accessible via the UART test points on the RX888mk2 PCB

#### Setup

UART is initialized in `DebugConsole.c:InitializeDebugConsole()`:

1. `CyU3PUartInit()` -- start UART driver
2. Configure 115200 baud, TX+RX enabled, DMA mode
3. `CyU3PDebugInit()` -- attach SDK debug driver to UART
4. Create DMA channel for UART RX, single-byte transfers
5. `UartCallback()` fires on each received byte

#### Usage

Connect a 3.3V UART adapter to the UART test points.  Any terminal at
115200/8N1 will show debug output and accept console commands.

### 1.3 USB Debug-over-EP0

This is the default and most practical channel -- no extra hardware
needed.

#### Protocol

Uses vendor request `READINFODEBUG` (`0xBA`) on the default control
endpoint (EP0).  Each control transfer is bidirectional:

**Host-to-device (input character):**

| `wValue` | Meaning |
|-----------|---------|
| `0x00` | No input character -- just polling for output |
| `0x0D` | Carriage return -- triggers command execution |
| Any other | ASCII character accumulated into `glConsoleInBuffer[32]` (lowercased) |

Characters are accumulated one per control transfer.  The 32-byte
input buffer silently stops accepting characters when full; sending CR
flushes and executes the command.

**Device-to-host (debug output):**

| Condition | Data phase |
|-----------|-----------|
| `glDebTxtLen > 0` | Firmware copies up to 63 bytes from `glBufDebug[]` to the data phase (limited by `CYFX_SDRAPP_MAX_EP0LEN - 1`).  Remaining data is shifted via `memmove` and returned on subsequent polls. |
| `glDebTxtLen == 0` | Firmware STALLs EP0 to signal "no data available" |

The last byte of the data phase is a NUL terminator placed by the
firmware.  Usable text length is `actual_transfer_length - 1`.

#### Activation

USB debug output requires two conditions:

1. `glIsApplnActive == CyTrue` (device is enumerated and configured)
2. `glFlagDebug == true`

**`glFlagDebug` starts as `false`.**  The host enables it by sending:

```
TESTFX3 (0xAC) with wValue=1
```

The `TESTFX3` response is 4 bytes:

| Byte | Contents |
|------|----------|
| 0 | `glHWconfig` (e.g., 0x04 = RX888r2) |
| 1 | `glFWconfig` high byte |
| 2 | `glFWconfig` low byte |
| 3 | `glVendorRqtCnt` (rolling 8-bit counter of vendor requests) |

When `wValue=1`, this also sets `glFlagDebug = true` as a side effect.
Sending `TESTFX3` with `wValue=0` disables debug mode again.

Until debug mode is enabled, all `DebugPrint()` calls are silently
discarded.  This prevents buffer overflow during early init when no
host is polling.

#### Output Buffer

| Item | Detail |
|------|--------|
| Buffer | `glBufDebug[256]` in `DebugConsole.c` (size = `MAXLEN_D_USB` from `protocol.h`) |
| Fill counter | `glDebTxtLen` (declared `volatile uint16_t`) |
| Formatter | `MyDebugSNPrint()` -- simplified printf supporting `%d`, `%x`, `%s`, `%u`, `%c` |
| Overflow | If `glDebTxtLen + len > MAXLEN_D_USB`, the message is silently dropped (see note below). |
| Synchronization | Writer (`DebugPrint2USB`, application thread) and reader (`READINFODEBUG` handler, USB callback context) use `CyU3PVicDisableAllInterrupts()`/`CyU3PVicEnableInterrupts()` to protect buffer access |

**DebugPrint2USB overflow fix (latent upstream bug):** The original upstream
firmware called `CyU3PThreadSleep(100)` when the buffer was full, intending to
let the host drain it.  This is unsafe when `DebugPrint` is called from the USB
EP0 callback context (e.g. inside STARTFX3/STOPFX3 handlers), because the USB
thread is blocked in the same callback — the host *cannot* drain the buffer
during the sleep.  With multiple DebugPrint calls in the handler, cumulative
sleep time could exceed the host's 1-second control transfer timeout, causing
the device to become unresponsive.  The fix removes the sleep: if the buffer is
full, the message is dropped.  This matches the existing behavior when
`glFlagDebug == false`.

---

## 2. Interactive Console Commands

Available via both UART and USB debug input.  Commands are
case-insensitive (all input is lowercased).  Enter/CR executes.

| Command | Action | Example output |
|---------|--------|----------------|
| `?` or empty | Print help and current DMA transfer count | `Enter commands: threads, stack, gpif, reset; DMAcnt = 1a3f` |
| `threads` | List all ThreadX RTOS threads by name, walking the thread linked list | `This : '03:AppThread'` / `Found: '01:IdleThread'` |
| `stack` | Show free stack space in the application thread (compares against `0xEFEFEFEF` fill pattern) | `Stack free in AppThread is 1248/2048` |
| `gpif` | Query and display current GPIF state machine state number | `GPIF State = 3` |
| `reset` | Software reset the FX3 (returns to bootloader) | `RESETTING CPU` (then device re-enumerates) |
| anything else | Echo back the unrecognized input | `Input: 'foo'` |

### Typical Console Session (via USB)

```
$ fx3_cmd debug
debug: enabled (hwconfig=0x04 fw=2.2)
debug: polling for output, type commands + Enter (Ctrl-C to quit)
TESTFX3
?
Enter commands:
threads, stack, gpif, reset;
DMAcnt = 0

threads
threads:
This : '03:AppThread'
Found: '00:CyIdleThread'
Found: '01:CyHelperThread'

stack
stack:
Stack free in AppThread is 1312/2048

gpif
GPIF State = 0
```

### Command Dispatch

Console input from either path accumulates in `glConsoleInBuffer[32]`.
On CR, a `USER_COMMAND_AVAILABLE` event is posted to `glEventAvailable`.
The main `ApplicationThread` polls the queue every 100ms and calls
`MsgParsing()` which decodes the 32-bit event by label (top 8 bits):

| Label | Action |
|-------|--------|
| 0 | Print USB event name from `EventName[]` |
| 1 | Print vendor request bytes (dead code -- nothing posts this label) |
| 2 | Print PIB error code (posted by `PibErrorCallback` on first overflow per streaming session) |
| 0x0A (`USER_COMMAND_AVAILABLE`) | Call `ParseCommand()` |

---

## 3. TraceSerial -- Vendor Request Logger

When `TRACESERIAL` is defined in `Application.h` (default), the
`TraceSerial()` function in `USBHandler.c` logs every EP0 vendor
request to the debug output after it is processed.

### What Gets Logged

| Request | Format | Example |
|---------|--------|---------|
| `SETARGFX3` | `SETARGFX3  <param_name>  <value>` | `SETARGFX3  DAT31_ATT  15` |
| `GPIOFX3` | `GPIOFX3  0x<word>` | `GPIOFX3  0x800` |
| `STARTADC` | `STARTADC  <freq>` | `STARTADC  64000000` |
| `STARTFX3` / `STOPFX3` / `RESETFX3` | Command name only | `STOPFX3` |
| Known commands (others) | `<name>  0x<byte0>  0x<byte1>` | `TESTFX3  0x4  0x2` |
| Unknown bRequest | `0x<code>  0x<byte0>  0x<byte1>` | `0xcc  0x0  0x0` |
| `READINFODEBUG` | Suppressed (avoids infinite recursion) | -- |

### SETARGFX3 Parameter Names

The `SETARGFX3List[]` maps `wIndex` to human-readable names:

| wIndex | Name |
|--------|------|
| 0--9 | `"0"` through `"9"` (unused/reserved) |
| 10 | `DAT31_ATT` (DAT-31 attenuator, 0--63) |
| 11 | `AD8370_VGA` (AD8370 VGA gain, 0--255) |
| 12 | `PRESELECTOR` |
| 13 | `VHF_ATTENUATOR` |
| >= 14 | Out of bounds -- logged as `<index>  <value>` |

### Bounds Checking

TraceSerial validates array indices before use:

- `bRequest` is checked against `FX3_CMD_BASE` (0xAA) through
  `FX3_CMD_BASE + FX3_CMD_COUNT` (0xBB).  Out-of-range requests are
  logged as hex (`0x<code>`) instead of indexing `FX3CommandName[]`.
- `SETARGFX3` `wIndex` is checked against `SETARGFX3_LIST_COUNT` (14).
  Out-of-range indices are logged numerically instead of indexing
  `SETARGFX3List[]`.

---

## 4. Error Logging Helpers

`Support.c` provides two helpers used throughout the firmware:

| Function | Behavior |
|----------|----------|
| `CheckStatus(name, status)` | On success: prints `"<name> = Successful"`.  On failure: prints error name and calls `ErrorBlinkAndReset()` (blinks LED, then resets FX3). |
| `CheckStatusSilent(name, status)` | On success: silent.  On failure: same as `CheckStatus`. |

Both are gated by `glDebugTxEnabled` -- they are no-ops until the
UART/debug subsystem is initialized.

---

## 5. GPIF Watchdog Recovery

The application thread (`RunApplication.c:216-295`) runs a watchdog
that detects and recovers from DMA stalls caused by host-side
backpressure.  All watchdog activity is logged to the debug console
with a `WDG:` prefix.

### Recovery Cap

A per-session recovery cap (`glWdgMaxRecovery`, default
`WDG_MAX_RECOVERY_DEFAULT` = 5) limits how many consecutive watchdog
recoveries are attempted before the watchdog gives up and waits for
an explicit `STARTFX3` from the host.  The cap counter
(`glWdgRecoveryCount`) resets to zero whenever DMA resumes normally
(i.e., the stall was transient), and also resets on `STARTFX3` and
`STOPFX3` (new streaming session).

### Detection

Every 100 ms the main loop compares `glDMACount` to its previous value.
If the count has not advanced and the GPIF state machine is in one of
the backpressure states (5 = TH0_BUSY, 7 = TH1_BUSY, 8 = TH1_WAIT,
9 = TH0_WAIT), the stall counter increments:

```
WDG: stall 1/3 SM=5 DMA=48210
WDG: stall 2/3 SM=5 DMA=48210
WDG: stall 3/3 SM=5 DMA=48210
```

If DMA resumes or the SM exits the stuck states before the counter
reaches 3, the stall is cleared:

```
WDG: DMA resumed (48210->48215), stall cleared
WDG: stall cleared SM=2 (was 1/3)
```

### Recovery (after 3 consecutive stalls = 300 ms)

When the stall counter reaches 3 (and the recovery cap has not been
exceeded), the watchdog tears down and rebuilds the streaming pipeline:

```
WDG: === RECOVERY START === SM=5 DMA=48210 recov=1/5
```

It then checks if the Si5351 PLL A is still locked:

- **PLL locked:** auto-restarts streaming (DMA SetXfer, GPIF SMStart,
  assert FW_TRG).

  ```
  WDG: === RECOVERY DONE === rst=0 flush=0 xfer=0 sm=0
  ```

- **PLL unlocked:** leaves the pipeline torn down and waits for the
  host to reconfigure the clock and send `STARTFX3`.

  ```
  WDG: === RECOVERY WAIT === rst=0 flush=0 xfer=0 sm=0
  ```

### Counter

Each recovery increments `glCounter[2]`, readable via `GETSTATS`
at offset 15--18.  This counter is also incremented by EP underrun
events in `USBHandler.c` -- both indicate streaming faults.  The
counter resets to zero on USB re-enumeration (`StartApplication`),
not on `STOPFX3`/`STARTFX3`.

### Observing the watchdog

From `fx3_cmd debug`, provoke a stall and watch the log:

```
!adc 64000000        # set ADC clock to 64 MHz
!start               # start streaming (host is not reading EP1)
                     # ... wait ~300ms, watchdog fires ...
WDG: stall 1/3 SM=5 DMA=4
WDG: stall 2/3 SM=5 DMA=4
WDG: stall 3/3 SM=5 DMA=4
WDG: === RECOVERY START ===
...
```

The `wedge_recovery` test in `fx3_cmd` exercises this path
programmatically and checks `glCounter[2]` via `GETSTATS`.

---

## 6. Diagnostic Counters (GETSTATS)

The `GETSTATS` vendor request (`0xB3`) returns a read-only snapshot of
firmware-internal counters and GPIF state.  It uses no new storage --
all counters live in the pre-existing `glCounter[20]` array.

### Protocol

Read-only EP0 vendor request (IN direction):

```
bRequest = 0xB3 (GETSTATS)
wValue   = 0
wIndex   = 0
wLength  = 26    (firmware sends exactly 26 bytes; host may request
                  fewer and the firmware will truncate to the
                  requested length)
```

### Response Layout

26 bytes, packed, little-endian (native ARM byte order):

| Offset | Size | Field | Source |
|--------|------|-------|--------|
| 0--3 | 4 | DMA buffer completions | `glDMACount` (incremented by DMA callback) |
| 4 | 1 | GPIF state machine state | `CyU3PGpifGetSMState()` sampled at read time |
| 5--8 | 4 | PIB error count | `glCounter[0]`, incremented in `PibErrorCallback` |
| 9--10 | 2 | Last PIB error arg | `glLastPibArg`, saved in `PibErrorCallback` |
| 11--14 | 4 | I2C failure count | `glCounter[1]`, incremented in `I2cTransfer` on error |
| 15--18 | 4 | Streaming fault count | `glCounter[2]`, incremented by GPIF watchdog on each recovery + on EP underruns (see section 5) |
| 19 | 1 | Si5351 status (reg 0) | `I2cTransfer(0x00, 0xC0, …)` sampled at read time |
| 20--23 | 4 | `boot_count` | `health_boot_count()`; increments once per `health_init()`.  Use to detect mid-test resets (Level 4 `CyU3PDeviceReset`, Level 5 HWDT, manual `RESETFX3`, or power cycle): snapshot before, compare after, mismatch means the device reset. |
| 24 | 1 | Si5351 CLK0_CONTROL (reg 16) | Live `I2cTransfer(0x10, 0xC0, …)`.  Bit 7 is `CLK0_PDN`: set means CLK0 is powered down, clear means CLK0 is enabled.  Returns `0xFF` if the I2C read fails. |
| 25 | 1 | `clk0_result` | `si5351_clk0_enabled()` (1 = CLK0 enabled, 0 = disabled or I2C error).  Same value `GpifPreflightCheck()` consults at `STARTFX3` time. |

### Counter Behavior

- **Reset:** All counters are zeroed in `StartApplication()` alongside
  `glDMACount = 0`.  This occurs on USB enumeration (`SET_CONFIGURATION`)
  -- not on `STOPFX3`/`STARTFX3` vendor commands.
- **Atomicity:** 32-bit aligned stores are atomic on the ARM926EJ-S core.
  Individual counter increments need no locking.  The multi-field
  snapshot is best-effort (no cross-counter consistency guarantee), which
  is acceptable for diagnostics.
- **EP0 buffer:** The handler clears `glEp0Buffer` before building the
  response (`CyU3PMemSet`), matching the `I2CRFX3` pattern.

### Host Usage

```
$ fx3_cmd stats
PASS stats: dma=52420 gpif=9 pib=12 last_pib=0x1005 i2c=4 faults=0 pll=0x00
```

---

## 7. Host-Side Tools

### 7.1 `fx3_cmd` -- Vendor Command Exerciser

Built from `tests/fx3_cmd.c`.  Provides both interactive and automated
access to all debug facilities.

```
cd tests && make
./fx3_cmd <command> [args...]
```

#### Interactive Debug Console

```
./fx3_cmd debug
```

- Sends `TESTFX3 wValue=1` to enable debug mode
- Puts stdin in raw mode (character-at-a-time, no echo)
- Polls `READINFODEBUG` every 50ms
- Typed characters are sent in `wValue`; Enter sends CR
- Ctrl-C to quit (SIGINT handler restores terminal from raw mode before exit)

##### Local Command Escape (`!`)

While in the debug console, typing `!` switches to **local command
mode**.  This lets you issue host-side `fx3_cmd` subcommands without
leaving the debug session or releasing the USB interface.

```
debug: type commands + Enter for FX3, '!' for local commands, Ctrl-C to quit
TESTFX3
?
Enter commands:
threads, stack, gpif, reset;
DMAcnt = 0

!stats
fx3> stats
PASS stats: dma=0 gpif=0 pib=0 last_pib=0x0000 i2c=0 faults=0 pll=0x00

!stop_gpif_state
fx3> stop_gpif_state
PASS stop_gpif_state: GPIF state=0 after STOP (SM properly stopped)

gpif
GPIF State = 0
```

**Behavior:**

| Key | In local mode | In normal mode |
|-----|---------------|----------------|
| `!` | -- | Enter local command mode, show `fx3> ` prompt |
| Printable chars | Echoed locally, buffered | Sent to FX3 via `READINFODEBUG wValue` |
| Enter | Dispatch buffered command to `do_*()` handler | Send CR to FX3 (triggers `ParseCommand`) |
| Backspace | Erase last buffered char | Sent to FX3 |
| Ctrl-C / Escape | Cancel local command, return to normal | Ctrl-C quits the debug session |

All subcommands from the `fx3_cmd` command line are available via `!`,
including test commands (`!stop_gpif_state`, `!wedge_recovery`, etc.)
and commands with arguments (`!adc 64000000`, `!att 15`, `!i2cr 0xC0 0 1`).
Type `!help` for the full list.

**Note:** Debug output polling continues between keystrokes in local
mode, but is paused during command execution (some test commands may
take several seconds).  This is intentional — the user explicitly
initiated the command and is waiting for its result.

#### Automated Test Commands

These are non-interactive subcommands for use in scripts and CI.  Each
prints `PASS`/`FAIL` and exits 0/1.

| Command | Tests | Issue |
|---------|-------|-------|
| `fx3_cmd test` | Device probe -- reads hwconfig, firmware version | -- |
| `fx3_cmd gpio <bits>` | Set GPIO control word | -- |
| `fx3_cmd adc <freq>` | Set ADC clock frequency | -- |
| `fx3_cmd att <0-63>` | Set DAT-31 attenuator | -- |
| `fx3_cmd vga <0-255>` | Set AD8370 VGA gain | -- |
| `fx3_cmd start` / `stop` | Start/stop GPIF streaming | -- |
| `fx3_cmd i2cr <addr> <reg> <len>` | I2C read | -- |
| `fx3_cmd i2cw <addr> <reg> <byte>...` | I2C write | -- |
| `fx3_cmd raw <code>` | Send arbitrary vendor request (hex bRequest) | -- |
| `fx3_cmd ep0_overflow` | Send wLength > 64 -- must STALL | #6 |
| `fx3_cmd oob_brequest` | Send bRequest=0xCC (outside FX3CommandName bounds) | #21 |
| `fx3_cmd oob_setarg` | Send SETARGFX3 wIndex=0xFFFF (outside SETARGFX3List bounds) | #20 |
| `fx3_cmd console_fill` | Send 35 chars to 32-byte glConsoleInBuffer | #13 |
| `fx3_cmd debug_race` | 50 rapid interleaved SETARGFX3 + READINFODEBUG polls | #8 |
| `fx3_cmd debug_poll` | Send `?` + CR, verify help text comes back | #26 |
| `fx3_cmd pib_overflow` | Start streaming without reading EP1, verify PIB error in debug output | #10 |
| `fx3_cmd stack_check` | Send `stack` command, parse watermark, verify > 25% free | #12 |
| `fx3_cmd stats` | Read and display all GETSTATS diagnostic counters | -- |
| `fx3_cmd stats_i2c` | Verify I2C failure counter increments after NACK on absent address | -- |
| `fx3_cmd stats_pib` | Verify PIB error counter is non-zero (opportunistic after `pib_overflow`) | -- |
| `fx3_cmd stats_pll` | Verify Si5351 PLL A locked and SYS_INIT clear via GETSTATS byte 19 | -- |
| `fx3_cmd stop_gpif_state` | START+STOP, verify GPIF SM state is 0, 1, or 255 (not stuck in read) | -- |
| `fx3_cmd stop_start_cycle` | Cycle STOP+START 5 times, verify bulk data flows each cycle | -- |
| `fx3_cmd pll_preflight` | Verify STARTFX3 is rejected (STALL) when ADC clock is off | -- |
| `fx3_cmd wedge_recovery` | Provoke DMA backpressure wedge, verify STOP+START recovers data flow | -- |
| `fx3_cmd soak [N [seed]]` | Randomized stress test: N cycles (default 100) of random scenarios with health checks | -- |
| `fx3_cmd reset` | Reboot FX3 to bootloader | -- |

### 7.2 `fw_test.sh` -- Automated TAP Test Suite

```
cd tests && make
./fw_test.sh --firmware ../SDDC_FX3/SDDC_FX3.img
```

Runs 36 tests (39 with streaming) in TAP format:

| # | Test | What it verifies |
|---|------|-----------------|
| 1 | Firmware upload | Device enumerates at PID 0x00F1 |
| 2 | Device probe | hwconfig = 0x04 (RX888r2) |
| 3 | GPIO | Set LED_BLUE |
| 4 | ADC clock | Set sample rate |
| 5--6 | Attenuator | Min (0) and max (63) |
| 7--8 | VGA | Min (0) and max (255) |
| 9 | Stop | Clean state |
| 10--12 | Stale tuner commands | 0xB4, 0xB5, 0xB8 all STALL (R82xx removed) |
| 13 | I2C NACK | Read from absent address 0xC2 correctly fails |
| 14 | ADC clock-off | STARTADC freq=0 exercises I2cTransferW1 path |
| 15 | EP0 overflow | wLength > 64 STALLs (issue #6) |
| 16 | OOB bRequest | bRequest=0xCC survives (issue #21) |
| 17 | OOB SETARGFX3 wIndex | wIndex=0xFFFF survives (issue #20) |
| 18 | Console buffer fill | 35-char input survives (issue #13) |
| 19 | Debug buffer race | 50 rapid poll cycles survive (issue #8) |
| 20 | Debug console over USB | `?` command returns help text (issue #26) |
| 21 | Stack watermark | Free > 25% of 2048 bytes after init (issue #12) |
| 22 | GETSTATS readout | GETSTATS (0xB3) returns 26 bytes with sane values |
| 23 | GETSTATS I2C counter | I2C failure count increments after NACK on absent address |
| 24 | GETSTATS PLL status | Si5351 PLL A locked, SYS_INIT clear |
| 25 | GPIF stop state | GPIF SM state is 0, 1, or 255 after STOPFX3 (not stuck in read) |
| 26 | Stop/start cycle | 5 STOP+START cycles with bulk read each -- detects GPIF wedge |
| 27 | PLL preflight | STARTFX3 rejected (STALL) when ADC clock is off |
| 28 | Wedge recovery | DMA backpressure wedge, then STOP+START recovers data flow |
| 29 | Vendor request counter wrap | `glVendorRqtCnt` wraps at 256 |
| 30 | Stale vendor codes | Dead-zone bRequest values produce STALL |
| 31 | SETARGFX3 gap indices | Near-miss wIndex values survive |
| 32 | GPIO extremes | All-zeros and all-ones GPIO patterns survive |
| 33 | I2C write NACK | Write to absent I2C address correctly fails |
| 34 | HW smoke test | ADC produces data after GPIO extremes |
| 35 | PIB overflow | GPIF overflow produces "PIB error" in debug output (issue #10) |
| 36 | GETSTATS PIB counter | PIB error count > 0 after overflow (issue #10) |
| 37--39 | Streaming (optional) | Data capture, byte count, non-zero data |

Options:

```
--firmware PATH        Firmware .img file (required)
--stream-seconds N     Streaming duration (default: 5)
--rx888-stream PATH    Path to rx888_stream binary
--skip-stream          Skip streaming tests (tests 31--33)
--sample-rate HZ       ADC sample rate (default: 32000000)
```

---

## 8. File Map

| File | Debug role |
|------|-----------|
| `DebugConsole.c` | UART init, USB debug buffer, printf formatter, console command parser, `ConsoleAccumulateChar()` |
| `USBHandler.c` | `READINFODEBUG` EP0 handler, `TraceSerial()` vendor request logger |
| `Support.c` | `CheckStatus()` / `CheckStatusSilent()` -- error logging with LED blink on failure |
| `protocol.h` | `_DEBUG_USB_` / `MAXLEN_D_USB` compile-time selection, `READINFODEBUG` command code |
| `Application.h` | `DebugPrint` macro routing (`DebugPrint2USB` vs `CyU3PDebugPrint`), `TRACESERIAL` define |
| `RunApplication.c` | `ApplicationThread` main loop, `MsgParsing()` event dispatch, `ParseCommand()` console handler, GPIF watchdog recovery with recovery cap |
| `tests/fx3_cmd.c` | Host-side vendor command tool, interactive debug console, soak test harness |
| `tests/fw_test.sh` | Automated TAP test suite (36 tests + 3 streaming) |

---

## 9. Known Limitations

| Area | Description |
|------|-------------|
| UART output | Unreachable when `_DEBUG_USB_` is defined (current default).  All `DebugPrint()` output goes to the USB buffer; UART input still works but output is invisible on the serial port. |
| `TESTFX3` | Debug enable/disable is a side effect of the device-info query.  Sending `TESTFX3` with `wValue=0` to read hwconfig will disable debug output.  There is no way to query device info without affecting the debug state. |
| `Seconds` counter | Dead code in USB-debug mode.  `RunApplication.c` increments a `Seconds` counter inside `#ifndef _DEBUG_USB_`, so there is no periodic throughput indication on the console when USB debug is active. |
| `EventName[]` | No bounds check.  `MsgParsing` indexes `EventName[]` (23 entries) by `(uint8_t)qevent`.  In practice SDK USB events map to 0--22, but there is no guard for out-of-range values. |
