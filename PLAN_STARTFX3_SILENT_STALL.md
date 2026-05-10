# PLAN: STARTFX3 silent-stall on preflight rejection

## Problem

`STARTFX3` (vendor request `0xAA`) is supposed to refuse to start the GPIF
state machine if the Si5351 CLK0 output is powered down or PLL A is
unlocked.  The check is implemented in `GpifPreflightCheck()`
(`SDDC_FX3/StartStopApplication.c:99`) and wired into the handler at
`SDDC_FX3/USBHandler.c:336`.  The check works — but the host's
`libusb_control_transfer` sees the request as **successful** even when
preflight refuses to start GPIF.  The host therefore continues to bulk
read, gets zero bytes (the SM was never loaded), and only notices the
problem indirectly (timeouts, watchdog activity, "PASS start" followed by
no streaming).

## Evidence

Sequence (with FX3 closed and `_DEBUG_USB_` debug console attached via
`./fx3_cmd debug`):

```text
fx3> adc 64000000
PASS adc 64000000 Hz

fx3> i2cw 0xc0 0x10 0x80          # force CLK0 power-down
PASS i2cw addr=0xC0 reg=0x10 len=1

fx3> stats                        # confirm chip is in PD
PASS stats: ... clk0_reg16=0x80 clk0_result=0

fx3> start
PASS start                        # host sees success
Preflight FAIL: ADC clock not enabled    # firmware printed rejection
                                  # no "GO s=… r=…" trace — handler broke out

fx3> stats                        # byte-identical to pre-start:
PASS stats: ... gpif=255 ... clk0_reg16=0x80 clk0_result=0
```

`gpif=255` after start ⇒ `CyU3PGpifGetSMState` returned its 0xFF default,
i.e. the GPIF SM was never loaded.  All other counters unchanged across
start ⇒ no DMA, no PIB, no I2C error, no watchdog.  This is exactly the
state produced by hitting `CyU3PUsbStall(0, ...); break;` at
`USBHandler.c:339-341` — the firmware did reject the request.

## Root cause

The STARTFX3 case body in `USBHandler.c` (lines 325-342) is structured:

```c
case STARTFX3:
    CyU3PUsbLPMDisable();
    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);   /* (1) */
    if (!GpifPreflightCheck()) {
        CyU3PUsbStall(0, CyTrue, CyFalse);            /* (2) */
        isHandled = CyTrue;
        break;
    }
    ...
```

`CyU3PUsbGetEP0Data` (1) completes both the OUT data phase and the IN
status phase of the control transfer.  By the time the FX3 calls
`CyU3PUsbStall` at (2), the host's `libusb_control_transfer` has already
returned 0.  The stall applies to subsequent EP0 traffic (effectively a
no-op until the next transfer naturally clears it), not to the request
we wanted to reject.

This is a documented FX3 EP0 quirk; the existing test
`do_test_pll_preflight` (`tests/fx3_cmd.c:1772`) already accommodates it
by treating "succeeded with no bulk data" as rejection.  The plain
`do_start` (`tests/fx3_cmd.c:402-410`) does not, and neither would any
host-side application using the vendor protocol.

## Fix

Reorder the STARTFX3 handler: run `GpifPreflightCheck()` **before**
consuming the EP0 data phase, and on failure return without calling
`GetEP0Data` and without setting `isHandled = CyTrue`.  When the vendor
handler returns with `isHandled == CyFalse`, the FX3 USB driver
auto-stalls the entire transfer, which propagates to libusb as
`LIBUSB_ERROR_PIPE`.

The replacement body (excluding the existing comment block, which moves
above the new preflight call):

```c
case STARTFX3:
    if (!GpifPreflightCheck()) {
        DebugPrint(4, "\r\nSTARTFX3 rejected by preflight");
        /* Leave isHandled = CyFalse so FX3 auto-stalls the whole
         * transfer — host sees LIBUSB_ERROR_PIPE.  Do NOT call
         * GetEP0Data here; the data phase must be left unacked. */
        break;
    }
    CyU3PUsbLPMDisable();
    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
    /* (existing teardown + StartGPIF body, unchanged) */
```

### Why not the alternative "stall before GetEP0Data" pattern

`CyU3PUsbStall(0, CyTrue, CyFalse)` followed by `break` with
`isHandled = CyTrue` would also reject visibly, but mixes two
rejection idioms in one switch.  Letting the FX3 driver auto-stall on
`isHandled = CyFalse` is the same pattern used by other defensive
returns in this handler (e.g. the oversize-EP0 guard at line 181-184
uses an explicit `CyU3PUsbStall` because it must return CyTrue early;
in our case we don't need to).  Pick one; the unhandled-return path is
the smaller diff.

### Risks / side effects considered

| Concern | Resolution |
|---|---|
| `GpifPreflightCheck` writing chip state | It only does I²C reads — no writes (`Si5351.c:190-206`, `Si5351.c:148-158`). Reordering it earlier is safe. |
| Added latency on STARTFX3 happy path | Two I²C reads (~600 µs total) move earlier in the case body. Net delta: zero. |
| Re-entrancy / interrupt context | STARTFX3 dispatch is on the USB callback context; single-threaded for a given request. No change. |
| `_DEBUG_USB_` trace order | `TraceSerial` at `USBHandler.c:62` prints "STARTFX3" *before* the switch dispatches. Trace ordering unchanged. |
| Watchdog interaction | Watchdog still calls `si5351_clk0_enabled()` on stall recovery; preflight reordering is independent. |

## Files changed

- `SDDC_FX3/USBHandler.c` — STARTFX3 case body reorder (~10 lines moved).

No other file changes:

- `StartStopApplication.c` (`GpifPreflightCheck`) — unchanged.
- `Si5351.c` / `Si5351.h` — unchanged.
- `protocol.h` — unchanged (no new vendor codes, GETSTATS layout unchanged).
- `tests/fx3_cmd.c` — unchanged.  `do_test_pll_preflight` already handles
  both stall and silent-fail paths; after the fix the stall path will be
  the primary outcome.  The silent-fail fallback becomes dead code but
  harmless; can be cleaned up in a separate doc commit.

## Validation test (post-build)

Repeat the exact debug-console sequence that originally exposed the bug:

```text
./fx3_cmd debug
fx3> adc 64000000
fx3> i2cw 0xc0 0x10 0x80
fx3> stats           # expect: clk0_reg16=0x80 clk0_result=0 (pre)
fx3> start           # expect: FAIL start: Broken pipe   ← was "PASS start"
fx3> stats           # expect: unchanged from previous stats
fx3> stop
fx3> adc 64000000
Ctrl-C
```

Pass criteria:
1. `start` prints `FAIL start: …` containing "Broken pipe" / "PIPE".
2. The firmware debug console still shows `Preflight FAIL: ADC clock not enabled`.
3. `stats` after the failed start is byte-identical to `stats` before
   (no SM load, no fault tick, no PIB/i2c change).

## Regression test

1. **Happy-path start**:
   ```text
   ./fx3_cmd adc 64000000 && ./fx3_cmd start && ./fx3_cmd stop && ./fx3_cmd adc 64000000
   ```
   Expected: all PASS.  Confirms preflight returns CyTrue on a healthy
   chip and the handler proceeds normally.

2. **Existing preflight test**:
   ```text
   ./fx3_cmd pll_preflight
   ```
   Expected: `PASS pll_preflight: STARTFX3 correctly rejected with PLL unlocked`.
   The primary path (`r == LIBUSB_ERROR_PIPE`) fires; the silent-fail
   fallback is no longer needed but doesn't trigger.

3. **Soak**:
   ```text
   ./fx3_cmd soak                # whatever the project's standard soak entry point is
   ```
   Expected: no regressions in `dma_completions`, `pib_errors`, or
   `fault_count` over a 5-minute run.  No change in watchdog behavior.

4. **GPIF wedge test** (per `docs/debugging.md` test catalog):
   ```text
   ./fx3_cmd gpif_wedge          # or equivalent
   ```
   Expected: unchanged behavior.  The preflight is unrelated to wedge
   recovery; this verifies we didn't accidentally couple them.

## Out of scope

- Updating `do_test_pll_preflight` to drop the silent-fail fallback.  The
  fallback is harmless; removing it is a doc / cleanup commit that can
  ride on a later PR with a clear "no-functional-change" message.
- Auditing other vendor requests for the same EP0 stall pattern.  A
  quick scan suggests STARTFX3 is the only handler that calls
  `GetEP0Data` and then conditionally stalls.  Worth a follow-up audit
  but not blocking this fix.
- Removing the `glLastClk0Reg16` / `glLastClk0Result` diagnostic bytes.
  They earned their keep this session; leave in place for future
  CLK0-related debugging.  Can be revisited if the GETSTATS payload
  ever needs the byte budget.

## Approval gate

Per `CLAUDE.md`:
- Awaiting user approval of this plan before implementation.
- After implementation, will provide a copy-pastable change-doc block
  (detailed description, build instructions, validation test,
  regression test) for review before any commit/push.
