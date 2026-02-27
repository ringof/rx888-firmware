# Plan: Fix dma_count_reset (100% fail) and sustained_stream (early single fail)

## Problem Statement

Soak run (seed 20, 390 cycles) shows two distinct test failures:

| Scenario         | Runs | Fail | Rate  | Nature                         |
|------------------|------|------|-------|--------------------------------|
| dma_count_reset  |    8 |    8 | 100%  | Intrinsic bug, predecessor-independent |
| sustained_stream |    4 |    1 |  25%  | Predecessor-dependent (pib_overflow)   |

All other 37 scenarios pass at 100%.

---

## Bug 1: dma_count_reset — STOPFX3 zeros glDMACount before GETSTATS reads it

### Root Cause

The test sequence is:

```
primed_start_and_read_retry()  →  data received (got1 >= 0)
cmd_u32(STOPFX3)               →  firmware sets glDMACount = 0 (USBHandler.c:386)
read_stats(GETSTATS)           →  reads glDMACount = 0
if (count1 == 0) → FAIL        →  "first session dma_count=0"
```

The stream *did* produce data (the primed read returned bytes), but STOPFX3
zeroes `glDMACount` before the test can observe it.  The
`glDMACount = 0` in the STOPFX3 handler was added to prevent watchdog
false-positives on stale counts.  STARTFX3 (USBHandler.c:355) also
resets `glDMACount = 0`, so the STOPFX3 reset is redundant for
watchdog safety — STARTFX3 always zeroes it before the next session
begins.

Evidence: `dma_count_monotonic` passes because it reads GETSTATS during
streaming (before STOP); `dma_count_reset` reads after STOP — always 0.

### Fix

**Test-side change only** — reorder GETSTATS before STOPFX3 in both
sessions of `do_test_dma_count_reset()` (`fx3_cmd.c:3250`).

Specifically, for each session:

1. `primed_start_and_read[_retry]()` — stream data
2. `read_stats()` — capture `dma_count` **while stream is live**
3. `cmd_u32(STOPFX3)` — stop streaming

This matches the approach used by `dma_count_monotonic` which reads
GETSTATS mid-stream and passes reliably.

No firmware change required.  The `glDMACount = 0` in STOPFX3 is
arguably redundant (STARTFX3 already resets it), but removing it is a
separate concern and not needed for this fix.

### Files Changed

- `tests/fx3_cmd.c`: `do_test_dma_count_reset()` (lines 3273–3293
  and 3314–3325 — swap GETSTATS and STOPFX3 ordering)

---

## Bug 2: sustained_stream — dirty xHCI endpoint after pib_overflow

### Root Cause

`sustained_stream` uses `cmd_u32_retry(STARTFX3)` + synchronous
`libusb_bulk_transfer()` (`fx3_cmd.c:2180–2202`).  Every other soak
streaming scenario uses `primed_start_and_read[_retry]()`, which:

- Queues an async bulk TD *before* STARTFX3 (avoids PIB race)
- On failure, does STOPFX3 → clear_halt(EP1) → retry (recovers dirty
  xHCI endpoint state)

After `pib_overflow` intentionally floods 1532+ PIB errors, the host-side
xHCI endpoint ring is left dirty.  Without the retry+clear_halt pattern,
`sustained_stream`'s first bulk transfer immediately fails with
`LIBUSB_ERROR_IO` at 0 bytes.

Evidence: sustained_stream passes 3/4 times; the single failure is
exclusively after `prev=pib_overflow`.  `stop_start_cycle` (which uses
`primed_start_and_read_retry`) runs immediately after the failure and
passes.

### Fix

Replace the plain `cmd_u32_retry(STARTFX3)` + first bulk read with
`primed_start_and_read_retry()`.  After the primed start succeeds
(GPIF running, first chunk received), continue with the existing
synchronous bulk loop for the remaining 30 seconds.

### Files Changed

- `tests/fx3_cmd.c`: `do_test_sustained_stream()` (lines 2180–2184 —
  replace STARTFX3 + error check with primed start; adjust
  `total_bytes` initialiser to count primed bytes)

---

## Validation

### Build

Standard build — `make` in `tests/`.  No firmware rebuild needed (no
firmware changes).

### Test: dma_count_reset

```bash
# Quick: run the single test several times
for i in $(seq 1 5); do ./fx3_cmd dma_count_reset; done

# Should see 5× PASS (currently 0/8 in soak)
```

### Test: sustained_stream after pib_overflow

```bash
# Reproduce the failing predecessor sequence
./fx3_cmd pib_overflow && sleep 0.1 && ./fx3_cmd sustained_stream

# Should PASS (currently fails when pib_overflow is predecessor)
```

### Regression: soak

```bash
./fx3_cmd -F ../SDDC_FX3/SDDC_FX3.img soak --cycles 20

# Target: 0 failures across all scenarios
# dma_count_reset should go from 0% → 100% pass
# sustained_stream should lose the early-after-pib_overflow failure
```

### Regression: fw_test.sh

```bash
./fw_test.sh --firmware ../SDDC_FX3/SDDC_FX3.img

# All tests should pass (dma_count_reset is not in fw_test.sh,
# but sustained_stream equivalent — the streaming tests — should
# remain clean)
```

---

## Scope

- **Two function edits** in `tests/fx3_cmd.c`, no new files
- **No firmware changes** — both bugs are test-side
- **No new dependencies**
