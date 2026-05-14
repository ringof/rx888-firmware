---
title: Vendor protocol plan
nav_order: 9
permalink: /vendor-protocol-plan/
---

# Vendor Protocol Evolution Plan

**Status:** approved for execution
**Branch:** TBD (likely a successor to `claude/fix-docker-build-path-qLngV` once #91 is merged)
**Trigger:** ka9q-radio Docker test harness surfaced a hidden coupling between `STARTADC` and `STARTFX3` (audit §8).  Fixing only the bug misses the larger lesson; this plan addresses both.

## For the executing Claude instance

You are picking up this plan from a previous session.  Constraints:

1. **`CLAUDE.md` applies in full.**  Plan first; ask before committing each commit; provide the standard change-doc block (detailed description / build instructions / validation test / regression test) before any commit; commit only with explicit user OK.
2. **Execute commits in order: 1 → 2 → 3 → 4.**  Each is independently mergeable; do not bundle.
3. **Before editing each file, `Read` the path to confirm line numbers haven't shifted** — the repository may have evolved since this plan was written.  Line numbers in this plan are accurate as of PR #91 merge.
4. **Hardware validation is the user's responsibility.**  You can build, lint, run static patch-apply checks, and write tests.  End-to-end streaming validation requires the user's RX888mk2.
5. **Soak/fuzz tests are mandatory for commits 1 and 3** (per Principle 2 below).  If you cannot run them yourself, at minimum write the script, commit it, and document expected output.
6. **If a sub-decision in this plan looks wrong**, STOP and ask the user.  Do not silently improvise.
7. **Page 3 of this document (below "Approval gate") contains the implementation specifics — file paths, line numbers, code skeletons, validation commands.  Read it before opening any source file.**

## Context

The vendor command set was drafted ~2018 around the original BBRF/SDR-Console reference code.  Reviewing it cold against three current consumers (ExtIO on Windows, `rx888_stream`, ka9q-radio), three structural issues stand out:

1. **State-changing and configuration commands are mixed.**  `STARTADC` reads as a state transition ("start the ADC") but is actually Si5351 configuration.  `STARTFX3` is the real state transition, with hidden preconditions.
2. **Implicit ordering is nowhere documented.**  The recommended sequence (`GPIOFX3` → `STARTADC` → gains → `STARTFX3`) is discovered by reading existing host code.  When a host like ka9q-radio takes a different but technically valid path (program Si5351 directly via `I2CWFX3`, skip `STARTADC`), the firmware silently refuses.
3. **No discovery or capability negotiation.**  Hosts cannot ask "what does this firmware support?" or "what is the current state?" — they probe by sending commands and watching for STALLs.

The wire protocol cannot break: ExtIO, `rx888_stream`, ka9q-radio, and ka9q-web are all deployed.  But the API can be improved by **additive** changes plus a hardware-grounded fix.

## Guiding principles

### Principle 1 — Separation of concerns

Move toward an API where commands fall into three orthogonal categories:

| Category | Property | Today | Goal |
|---|---|---|---|
| **Configuration** | Idempotent; order-free; no effect on data flow | `STARTADC`, `GPIOFX3`, `SETARGFX3` | unchanged on the wire; clearer in docs |
| **Control** | Explicit state transitions; may fail with a reason | `STARTFX3`, `STOPFX3`, `RESETFX3` | preflights query the chip, not flags |
| **Query** | Read-only; returns state or capabilities | `GETSTATS` (partial) | add `GETCAPABILITIES`, `GETSTATE` |

### Principle 2 — Robustness / Recoverability invariant *(new)*

**No vendor command, regardless of arguments, current state, or sequence, may leave the RX888 in a state requiring a power cycle to recover.**  A misbehaving host should always be able to reach a clean baseline by issuing well-known commands or by host-side USB reset.

Concretely, the firmware must guarantee:

- Any vendor request (valid or not) returns to the EP0 idle state within a bounded time — no infinite loops, no busy-waits without timeout.
- Any sequence of valid commands in any order is recoverable: e.g. `STARTFX3` before `STARTADC`, `STOPFX3` when already stopped, repeated `STARTFX3` without intervening `STOPFX3`, etc.  Each is either a no-op or returns a clear failure (STALL or response code) — neither wedges the GPIF, the DMA, nor the I2C bus.
- Malformed payloads (wrong length, out-of-range values) are rejected at the EP0 boundary; they do not corrupt internal state.
- The escape hatches `RESETFX3` and `STOPFX3` always work, including from a wedged GPIF state.
- Host-side `libusb_reset_device()` — and ultimately a USB unplug/replug — always brings the device back to bootloader (`04b4:00f3`) or to the loaded baseline (`04b4:00f1` with idle GPIF).

This principle binds **every** firmware change in this plan.  Each commit must include a soak/fuzz test (or pointer to one) demonstrating that the change does not create a new wedge mode.

## Plan: four independent commits

Each commit is independently mergeable.  Order matters only for documentation cross-references.

### Commit 1 — `fix(Si5351): query CLK0_PDN directly instead of trusting STARTADC flag`

**Scope:** `SDDC_FX3/driver/Si5351.c` only (~20 lines edited / removed).

**Change:**
- Replace `si5351_clk0_enabled()` to read CLK0_CONTROL register (16) bit 7 via I2C.  Bit 7 = 0 means powered up.  Symmetric with the existing `si5351_pll_locked()` which already reads register 0 bit 5.
- Remove the `glAdcClockEnabled` global flag and the two write sites in `si5351aSetFrequencyA()`.

**Why this is correct:** the hardware register is authoritative.  Any path that programs the Si5351 — `STARTADC` (firmware path) or `I2CWFX3` (host path) — leaves register 16 in the same state, so `GpifPreflightCheck()` passes for both.

**Recoverability check:** if the I2C read fails (bus glitch), the function returns `CyFalse` — `STARTFX3` refuses safely.  Recovery: retry, or `RESETFX3`.  No new wedge surface.

**Compatibility:** `rx888_stream`, ExtIO, and ka9q-radio (with no host-side patches) all work after this change.  No wire-protocol change.

**Validation:** existing `tests/fw_test.sh` covers the `STARTADC`-then-`STARTFX3` path; the Docker container without patch 03 covers the `I2CWFX3`-direct path.  Both must pass.

**Risk:** low.  `I2cTransfer()` is a proven path (used by `si5351_pll_locked`).  Reading register 16 is non-destructive.

### Commit 2 — `refactor(docker): drop ka9q patch 03; firmware fix supersedes it`

**Scope:** test-harness side only.

- Remove `docker/ka9q-radio/patches/03-startadc-before-startfx3.patch`.
- Update `docs/ka9q-compat-audit.md` §8: reframe as "fixed firmware-side; no ka9q-radio patches required."
- Update `docker/ka9q-radio/patches/README.md` and the docker README to reflect "zero ka9q patches needed."  Patches/ directory is now empty.
- The `git apply` loop in the Dockerfile should still work (no-op when patches/ is empty).

**Validation:** rebuild the image with `--no-cache`; rerun the helper-script workflow; confirm streaming still works against firmware from commit 1.

**Risk:** zero — this is a downgrade-friction reduction, not a behavior change.

### Commit 3 — `feat(USBHandler): add GETCAPABILITIES and GETSTATE vendor commands`

**Scope:** `SDDC_FX3/protocol.h`, `SDDC_FX3/USBHandler.c`, plus a documentation block in `docs/architecture.md`.

**New commands** at vendor request bytes **0xBB** and **0xBC** (confirmed unused):

- `GETCAPABILITIES` (0xBB) — returns a JSON object describing firmware version, build SHA, supported-commands list, sample-rate min/max, GPIO bit map (so the LED ambiguity in audit §4 becomes queryable), ADC bit width.  JSON keeps the schema flexible — fields can be added without breaking parsers.  Example:

  ```json
  {"version":"0.0.1","build":"f78cff9","commands":["STARTFX3","STOPFX3","STARTADC","GPIOFX3","I2CWFX3","I2CRFX3","SETARGFX3","RESETFX3","READINFODEBUG","GETSTATS","GETCAPABILITIES","GETSTATE"],"sample_rate_hz":{"min":2000000,"max":129600000},"gpio_bits":{"DITHER":0,"RANDOM":1,"VHF_EN":2,"LED_BLUE":11},"adc_bits":16}
  ```

  Returned over EP0 in chunks if it exceeds `CYFX_SDRAPP_MAX_EP0LEN`; host issues repeat reads until done.  Length capped (e.g. 1024 bytes) so this never wedges.

- `GETSTATE` (0xBC) — returns a smaller JSON object with current SM state, current Si5351 frequency (read from chip), current GPIO bits, last vendor-command error code:

  ```json
  {"sm_state":"IDLE","si5351_clk0_hz":64800000,"gpio":3,"last_error":0,"streaming":false}
  ```

**Why JSON over fixed binary:** schema flexibility (new fields don't break old hosts), introspection by hand (`curl`-style debugging via `usbcontrol` or similar), easier integration with web-based tooling (ka9q-web, future telemetry).  Cost is parser size — small in C with a string-matching scanner, and the firmware only needs to *emit*, not parse.

**Recoverability check:** both commands are read-only.  Truncated reads or aborted transfers leave the firmware in EP0 idle.  Bounded length cap means no buffer-overflow surface.

**Validation:**
- Issue `GETCAPABILITIES`, parse JSON, verify version field matches built-in `git describe` output.
- Issue each command listed in the response and confirm none STALL.
- Issue `GETSTATE` before and after `STARTFX3` / `STOPFX3` and confirm `sm_state` and `streaming` change.
- **Recoverability soak:** issue these commands at high rate concurrently with `STARTFX3` / `STOPFX3` cycles; confirm no resource leak, no GPIF wedge.

**Risk:** low-medium.  Pure additions but introduces the firmware's first JSON serialization — keep it simple (single static buffer + `snprintf`).

### Commit 4 — `docs(architecture): document recommended vendor command sequence and recoverability guarantees`

**Scope:** `docs/architecture.md` only.

**Content:**
1. **Recommended initialization sequence** for HF receive on a fresh device: `GPIOFX3` (mode bits) → set Si5351 (via `STARTADC` *or* direct `I2CWFX3` — both are valid) → `SETARGFX3 DAT31_ATT` → `SETARGFX3 AD8340_VGA` → `STARTFX3` → bulk read → `STOPFX3`.
2. **Hardware preconditions** that `STARTFX3` actually checks (CLK0 powered up via register-16 query, PLL A locked) — the *truth*, not the historical proxy.
3. **Capability discovery flow** for new hosts: `GETCAPABILITIES` first, branch on `commands` list, then issue commands.
4. **Recoverability contract** (Principle 2 above) — guarantees the firmware makes to host authors.  Includes the recovery escape sequence: `STOPFX3` → `RESETFX3` → `libusb_reset_device()` → unplug/replug → re-upload firmware.
5. **Reference to deprecated commands** (`TUNERINIT`, `TUNERTUNE`, `TUNERSTDBY`) so a new developer who finds them in `protocol.h` knows they STALL on this firmware.

**Risk:** zero (docs only).

## Out of scope (deferred)

- VHF support / R82xx tuner.  Audit §3 — non-blocking for HF.
- LED bit-mapping convergence with ka9q.  Audit §4 — cosmetic; once `GETCAPABILITIES` exposes the GPIO bit map, hosts can adapt programmatically.
- ka9q-radio's missing `libusb_clear_halt()`.  Audit-mentioned but is upstream's call.
- A general fuzz harness for vendor commands (worth doing as a separate effort, separate from this plan).

## Open questions — resolved

1. **Vendor request bytes for `GETCAPABILITIES` / `GETSTATE`.**  → Resolved: **0xBB and 0xBC**.
2. **Capability struct format.**  → Resolved: **JSON text** (schema flexibility outweighs parser cost).
3. **Patch 03 / audit cleanup placement.**  → Resolved: **separate commit (commit 2)**, immediately after the firmware fix.
4. **Single PR vs. split.**  → Resolved: **split across multiple commits** as designed; merge order 1 → 2 → 3 → 4.

## Approval gate

This plan is ready to execute pending OK on this document.  Once approved, work proceeds commit-by-commit with the standard CLAUDE.md change-doc block before each commit.

---

# Page 3 — Implementation specifics

## Repository state at plan-approval time

- **Current `main`:** includes PR #91 merge (`f9077f7` and predecessors).  All work below targets a fresh branch off `main`.
- **Suggested branch name:** `claude/vendor-protocol-v1` (or similar; the executing instance picks).
- **Build the firmware:** `cd SDDC_FX3 && make clean && make all` → produces `SDDC_FX3.img`.
- **Build the test-harness image:** `docker build -t ka9q-radio docker/ka9q-radio/`.
- **End-to-end validation requires real RX888mk2 hardware.**  An instance running headlessly can build, syntax-check, run static patch-apply, and run unit-style tests, but cannot exercise the streaming path.  When end-to-end testing is required, ask the user to run the helper script.

## Files in scope per commit

### Commit 1 — `fix(Si5351): query CLK0_PDN directly`

**Primary file:** `SDDC_FX3/driver/Si5351.c`

Locations as of PR #91 merge (re-verify with `Read` before editing):

- Line ~55: `static CyBool_t glAdcClockEnabled = CyFalse;` — **REMOVE**
- Lines ~140–163: existing `si5351_pll_locked()` — model for the new function
- Lines ~165–175: current `si5351_clk0_enabled()` (returns the bare flag) — **REPLACE**
- Line ~192: `glAdcClockEnabled = CyFalse;` inside `si5351aSetFrequencyA(freq=0)` — **REMOVE**
- Line ~250: `glAdcClockEnabled = CyTrue;` inside the success path of `si5351aSetFrequencyA(freq>0)` — **REMOVE**

**Proposed replacement** (drop in at the old `si5351_clk0_enabled` location):

```c
CyBool_t si5351_clk0_enabled(void)
{
    uint8_t reg16 = 0xFF;  /* default = "powered down" if I2C fails */
    CyU3PReturnStatus_t rc;

    /* Read CLK0_CONTROL register (16).  Bit 7 = CLK0_PDN:
     *   0 = clock powered up and running
     *   1 = powered down (chip default at power-on)
     * Authoritative — reflects actual chip state regardless of
     * which path programmed it (STARTADC vs direct I2CWFX3).
     */
    rc = I2cTransfer(SI_CLK0_CONTROL, SI5351_ADDR, 1, &reg16, CyTrue);
    if (rc != CY_U3P_SUCCESS)
        return CyFalse;        /* I2C failed — refuse start */
    return (reg16 & 0x80) == 0;
}
```

- `I2cTransfer` signature is in `SDDC_FX3/driver/I2cControl.h`; the existing `si5351_pll_locked` (already in this file) shows the calling pattern.
- `SI_CLK0_CONTROL` already exists as a constant in `SDDC_FX3/driver/Si5351.h` (referenced at `Si5351.c:193` in the `freq=0` path).  Reuse it.

**Validation:**
- Build: `cd SDDC_FX3 && make clean && make all` — must produce `SDDC_FX3.img` with no warnings.
- Hardware (STARTADC path, host-driven): `./tests/fw_test.sh --firmware SDDC_FX3/SDDC_FX3.img` — must pass.
- Hardware (I2CWFX3-direct path, after commit 2): `./docker/ka9q-radio/ka9q.sh start && ./docker/ka9q-radio/ka9q.sh monitor` — must produce audio.

**Soak test** (Commit 1 must ship this script): see template at the end of Page 3.

### Commit 2 — `refactor(docker): drop ka9q patch 03`

**Files:**

- `docker/ka9q-radio/patches/03-startadc-before-startfx3.patch` — **DELETE** (`git rm`).
- `docker/ka9q-radio/patches/README.md` — table loses its only row.  Add note: *"All previously-required ka9q-radio source patches are now resolved firmware-side; the patches/ directory is empty.  Container patches are reserved for future incompatibilities that have no firmware-side or container-side fix."*
- `docs/ka9q-compat-audit.md` §8 — reframe.  Replace the "Fix" subsection with: *"Resolution: fixed firmware-side in `<commit-1-sha>` of this repo (`driver/Si5351.c`).  `si5351_clk0_enabled()` now queries CLK0_CONTROL register 16 bit 7 directly.  ka9q-radio against the current firmware requires no source modifications."*
- `docker/ka9q-radio/README.md`:
  - "What this tests" bullet 2 — revert to: *"Si5351 clock programming via `I2CWFX3` (host-side direct programming, validated by SDDC firmware's chip-level preflight check)."*
  - "Known compatibility notes" — remove the "Missing STARTADC" bullet entirely.

**Validation:** rebuild image with `docker build --no-cache -t ka9q-radio docker/ka9q-radio/`; re-run the three-terminal flow; confirm `rx888 running` and audible output via `./ka9q.sh monitor`.

### Commit 3 — `feat(USBHandler): add GETCAPABILITIES and GETSTATE`

**Files:**

- `SDDC_FX3/protocol.h` — add to the command enum:
  ```c
  GETCAPABILITIES = 0xBB,  /* Return JSON describing firmware capabilities */
  GETSTATE        = 0xBC,  /* Return JSON describing current runtime state */
  ```
- `SDDC_FX3/USBHandler.c` — add two new `case` blocks in the EP0 vendor-request switch.  Model on the existing `GETSTATS` handler at line ~246–266; same `glEp0Buffer` + `CyU3PUsbSendEP0Data` + `isHandled = CyTrue` pattern.
- `docs/architecture.md` — document the wire format (`bmRequestType=0xC0`, `bRequest=0xBB`/`0xBC`, response in `glEp0Buffer`, JSON UTF-8) and schema (full sample JSON of each).

**JSON emission pattern** (place adjacent to the new case statements in `USBHandler.c`):

```c
static int build_capabilities_json(char *buf, int max)
{
    return snprintf(buf, max,
        "{"
          "\"version\":\"%s\","
          "\"build\":\"%s\","
          "\"commands\":[\"STARTFX3\",\"STOPFX3\",\"STARTADC\",\"GPIOFX3\","
                        "\"I2CWFX3\",\"I2CRFX3\",\"SETARGFX3\",\"RESETFX3\","
                        "\"READINFODEBUG\",\"GETSTATS\","
                        "\"GETCAPABILITIES\",\"GETSTATE\"],"
          "\"sample_rate_hz\":{\"min\":2000000,\"max\":129600000},"
          "\"gpio_bits\":{\"DITHER\":0,\"RANDOM\":1,\"VHF_EN\":2,\"LED_BLUE\":11},"
          "\"adc_bits\":16"
        "}",
        SDDC_FW_VERSION, SDDC_BUILD_HASH);
}
```

`SDDC_FW_VERSION` and `SDDC_BUILD_HASH` should be passed via `-D` flags from `make`.  If the Makefile does not already inject these (check first), add:

```make
GIT_VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "unknown")
GIT_HASH    := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
CFLAGS      += -DSDDC_FW_VERSION=\"$(GIT_VERSION)\" -DSDDC_BUILD_HASH=\"$(GIT_HASH)\"
```

**Sub-decision resolutions:**
- **GPIO bit map:** SDDC-canonical, sourced from `docs/architecture.md` §"GPIOFX3 bitmask protocol".  NOT ka9q's mapping (ka9q's differs in bits 11–12; see audit §4).
- **Sample-rate min/max:** `min` = 2 000 000 Hz (lowest reliable Si5351 output for direct sampling), `max` = 129 600 000 Hz (per existing `rx888-test.conf` and standard ka9q config).  Re-verify by reading `Si5351.c` for any hardcoded clamps before finalizing.
- **ADC bit width:** 16 (LTC2208 in RX888mk2 hardware).  Constant.

**EP0 chunking:** if the JSON exceeds `CYFX_SDRAPP_MAX_EP0LEN`, chunk via repeated `CyU3PUsbSendEP0Data` calls — the existing `READINFODEBUG` handler is the model.  In practice the capabilities JSON above is ~250 bytes, well under the buffer size, so chunking is unlikely to trigger.

**Validation script** (commit alongside, at `tests/test_capabilities.sh`):

```bash
#!/bin/bash
# Issue GETCAPABILITIES and GETSTATE; validate JSON.
set -euo pipefail
JQ_PATH="${JQ_PATH:-jq}"
"$JQ_PATH" --version >/dev/null  # require jq
./tests/fx3_cmd --vendor 0xBB --read 1024 > /tmp/caps.json
"$JQ_PATH" -e '.version, .commands, .sample_rate_hz.min, .adc_bits' /tmp/caps.json
./tests/fx3_cmd --vendor 0xBC --read 1024 > /tmp/state.json
"$JQ_PATH" -e '.sm_state, .si5351_clk0_hz, .streaming' /tmp/state.json
echo "OK"
```

(`tests/fx3_cmd` is the existing helper from `tests/Makefile`.  If it does not currently support `--read N`, extend it as part of this commit.)

### Commit 4 — `docs(architecture): recommended sequence + recoverability contract`

**File:** `docs/architecture.md` only.

Add new top-level sections (place them after the existing "Vendor command table" section, in this order):

1. **"Recommended initialization sequence"** — paste the sequence from Principle 1 of this plan, with rationale: *"Any sequence that leaves CLK0 enabled and PLL A locked at the time `STARTFX3` is sent will succeed; the recommended sequence is the easy path, not the only path."*
2. **"Hardware preconditions for STARTFX3"** — quote the actual checks (post-commit-1): CLK0 power-up via register-16 query, PLL A lock via register-0 LOL_A bit.
3. **"Capability discovery"** — pointer to `GETCAPABILITIES`/`GETSTATE` (commit 3); recommended first request from new hosts.
4. **"Recoverability contract"** — Principle 2's four invariants verbatim plus the recovery escape ladder: `STOPFX3` → `RESETFX3` → host `libusb_reset_device()` → unplug/replug → re-upload firmware via `ezusb_load_ram` or `fxload`.
5. **"Deprecated commands"** — `TUNERINIT` (0xB4), `TUNERTUNE` (0xB5), `TUNERSTDBY` (0xB8) STALL on this firmware (R82xx removed, see `docs/LICENSE_ANALYSIS.md`).

## Soak/fuzz test template

Each firmware-touching commit (1 and 3) must ship a script in `tests/` that proves no command sequence can wedge the device.  Template:

```bash
#!/bin/bash
# tests/soak_<topic>.sh
#
# Goal: prove that no sequence of valid + malformed vendor commands
# wedges the device.  Recovery is checked by sending GETSTATE
# (commit 3+) or by RESETFX3 then a known-good probe.
#
# Usage:  ITERATIONS=1000 DEVICE=/dev/bus/usb/<bus>/<dev> ./tests/soak_<topic>.sh
#
# Exits 0 on success; nonzero with diagnostic on first wedge.

set -euo pipefail
ITERATIONS="${ITERATIONS:-1000}"
DEVICE="${DEVICE:-}"
[ -n "$DEVICE" ] || { echo "Set DEVICE=/dev/bus/usb/..." >&2; exit 2; }

cmds=(0xAA 0xAB 0xB2 0xAD 0xAE 0xAF 0xB6 0xB1 0xBA 0xB3 0xBB 0xBC 0xFF)
# 0xFF is "unknown" — must STALL cleanly, never wedge.

for i in $(seq 1 "$ITERATIONS"); do
    cmd="${cmds[$RANDOM % ${#cmds[@]}]}"
    arg=$RANDOM
    # Don't propagate STALLs; that's the test target.
    ./tests/fx3_cmd --device "$DEVICE" --vendor "$cmd" --arg "$arg" || true

    # Recovery probe after EVERY iteration:
    ./tests/fx3_cmd --device "$DEVICE" --vendor 0xB1 || true   # RESETFX3
    ./tests/fx3_cmd --device "$DEVICE" --vendor 0xBC --read 1024 > /dev/null \
        || { echo "WEDGED at iter $i after vendor=$cmd arg=$arg" >&2; exit 1; }
done
echo "$ITERATIONS iterations: no wedges."
```

- For commit 1 (`tests/soak_si5351.sh`), focus the random selection on `STARTADC` / `STARTFX3` / `STOPFX3` / `I2CWFX3` to exercise the chip-query path.
- For commit 3 (`tests/soak_capabilities.sh`), include `GETCAPABILITIES` / `GETSTATE` and `0xFF` (unknown) heavily.
- Expected runtime: ~2 minutes for 1000 iterations on a typical host.
- The recovery probe in commit 1 (where `GETSTATE` doesn't exist yet) should use a cheap known-good command instead — recommend `READINFODEBUG` (0xBA).
