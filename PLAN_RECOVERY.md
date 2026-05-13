# Plan: Recovery Architecture via `health.c`

## 1. Context and motivation

The 1-hour soak on `main` produced one transient `rapid_start_stop` failure
(0.05% per-cycle, self-recovered) and zero device wedges.  The same scenario
on PR #93's branch produced an **unrecoverable** firmware lockup requiring
manual USB power-cycle.  Investigation showed:

- The lockup is in the STOPFX3 vendor handler (EP0 thread deadlocked inside
  an SDK call).
- The existing streaming watchdog in `SDDC_FX3/RunApplication.c` cannot
  fire — its gating conditions are scoped to GPIF state {5,7,8,9} + DMA
  non-advance.
- The README documents a "layered recovery architecture" that does not
  exist as a coherent system in the code.  Recovery logic is scattered
  across `RunApplication.c` (streaming watchdog), `USBHandler.c` (inline
  soft-stop in STOPFX3), `StartStopApplication.c` (StopApplication
  cleanup), and the soak harness's host-side retry shim.
- Issues #104, #105, #106, and #107 document the gap between README
  claims and observed behavior.

A targeted fix (per-callback timer, EP0 heartbeat, etc.) would close
issue #105 but add to the scatter — another bespoke recovery mechanism
in another file, with no shared contract.  After three or four such
patches no future developer can reconstruct "where does recovery live?"
from a single read.

## 2. Architectural goal

Build a single recovery module — `SDDC_FX3/health.c` and
`SDDC_FX3/health.h` — that owns every recovery decision.  The module is
the only place future recovery additions are made.  Existing scattered
recovery code is left in place initially and migrated over time as
bandwidth allows.

The win is comprehensibility, not novelty: a future developer reads one
file and understands the recovery system.  The README's "layered
recovery" section becomes documentation of code that actually exists.

## 3. Interface contract — three functions

Every recovery-related contribution goes through this interface.  No
parallel mechanisms; no inline cleanup in handlers.

```
/* Recorded by callers when an observable liveness event occurs.
 * Cheap; safe to call from any thread including USB callbacks. */
void health_record_event(health_event_t event);

/* Called periodically from the main loop.  Pure function — examines
 * accumulated state and returns a structured status describing whether
 * the device is healthy and, if not, why.  Does NOT take recovery
 * action. */
health_status_t health_evaluate(void);

/* Called by the main loop when health_evaluate() reports unhealthy.
 * Selects and applies an appropriate remedy from the cascade based on
 * the failure reason.  May call CyU3PDeviceReset() in the worst case. */
void health_recover(health_status_t status);
```

The discipline rule is mechanical: **new recovery code must extend one
of these three functions.**  Reviewers can apply this in 30 seconds.
Future liveness signals are new event types; future remedies are new
branches inside `health_recover()`.

## 4. Recovery cascade

| Level | Remedy | Appropriate for | Latency |
|---|---|---|---|
| 1 | Soft-stop FW_TRG + `DmaMultiChannelReset` + `GpifSMStart` | Streaming wedge (today's behavior) | ~300 ms |
| 2 | EP0 stall+unstall + `FlushEp` on EP1 | EP0 stuck state | ~10 ms |
| 3 | `StopApplication` + `StartApplication` | Application-level state corruption | ~100 ms |
| 4 | `CyU3PDeviceReset(CyFalse)` | Anything else / catastrophic | full re-enumeration |
| 5 | FX3 hardware WDT (`CyU3PSysWatchDogConfigure` + periodic pet) | Levels 1–4 themselves wedged | configured period |

Today's code implements Level 1 only.  Level 5 (HWDT) is a one-call
setup and a single pet call — added once, in `health.c`, as a separate
concern from any specific failure mode.  Levels 2–4 are added as
specific failure modes are addressed.

## 5. PR sequencing

**PR 1 — Skeleton.**  `health.h` + `health.c` with the three-function
interface, a stub `health_status_t` enum (initial values: `HEALTHY`,
`WEDGED_UNKNOWN`), and an empty `health_recover()` switch.  No
integration points; no behavior change.  Update `README.md` "Firmware
Robustness" section to describe the contract.  Reviewable in 20 minutes;
trivially correct because nothing calls it.

**PR 2 — First real use: EP0 heartbeat + Level 4 backstop.**  Wires
`health_record_event(EVENT_EP0_HANDLER_EXIT)` into the USB vendor
request callback.  Adds `WEDGED_EP0` to the status enum.
`health_evaluate()` checks "last EP0 exit timestamp" vs main loop's
current time.  `health_recover(WEDGED_EP0)` calls
`CyU3PDeviceReset(CyFalse)`.  Fixes the issue #104 lockup directly.
Validation: the four reproduction tests on `main` (merged from
`claude/fw-test-restore`) should run cleanly on the new firmware.

**PR 3 — Level 5 (HWDT) backstop.**  Adds `CyU3PSysWatchDogConfigure`
call in `health_init()`, pet from main loop.  Belt-and-braces against
catastrophic wedges even if Level 4 fails.  Independent of any specific
failure mode; pure defense-in-depth.

**PR 4+ — Specific liveness signals and remedies as needed.**  Each PR
adds one event type and/or one cascade branch.  Issue-driven (one PR
per filed issue when it surfaces).

**PR N (deferred) — Migration of existing streaming watchdog.**
Bit-identical refactor.  The streaming-wedge detection logic moves into
`health_evaluate()` as another event signal; the cleanup actions move
into `health_recover(WEDGED_STREAMING)`.  Done when bandwidth allows.
No urgency once the new framework is carrying real load.

## 6. Validation strategy

- **Soak harness is the primary gate.**  Each PR after PR 1 must pass a
  clean 1-hour soak before merge.
- **Issue #104 reproduction tests** (now on `main`:
  `rapid_stress_with_concurrent_stats`, `rapid_start_stop_extreme`,
  `stop_during_starting`, `health_latency_after_stress`) gate PR 2.
  They must reproduce the lockup on pre-PR-2 firmware AND pass on
  post-PR-2 firmware.
- **`fw_test.sh`** continues as the smoke test; all 41 tests must pass.
- **README robustness section is updated incrementally and only with
  evidence.**  Each claim added must point to a test or observation
  that backs it.  Avoid the original failure mode (overclaim).

## 7. Scope boundaries

This plan deliberately does NOT:

- Rewrite the existing streaming watchdog up front (deferred to PR N).
- Add every conceivable liveness signal (signals are added as failure
  modes warrant).
- Change the FX3 SDK boundary or replace SDK calls.
- Touch host-side recovery (libusb retries, soak harness shim) except
  where they currently mask firmware bugs that should be the
  framework's responsibility.
- Address `gpif_soft_stop` intermittent (#106) — that's a 1 ms timing
  issue independent of the recovery architecture.  Tracked separately.

## 8. Open questions for confirmation

1. **HWDT timing.**  PR 3 (own PR) vs folding into PR 1 (skeleton +
   HWDT setup since HWDT is independent of any specific failure mode).
   Slight preference for own PR — keeps PR 1 zero-behavior-change.
2. **README update cadence.**  Update in PR 1 (describe the contract),
   or wait until PR 2 lands a real fix (so the doc describes working
   code, not aspirational contract)?  Slight preference for PR 1 — the
   contract is the deliverable of PR 1.
3. **Existing `CyU3PUsbStall` / `FlushEp` logic in
   `USBHandler.c:166-175` CLEAR_FEATURE handler** — when this migrates,
   does it become a Level 2 remedy in `health_recover()`, or does it
   stay in the EP0 callback for protocol-correctness reasons?  Defer
   the decision until migration PR.

## 9. References

- Issues #104, #105, #106, #107 — documented gaps that motivate this
  plan.
- PR #93 — on hold; the original failure surface that surfaced these.
- Branch `main` — now contains the four #104 reproduction tests plus
  the dispatcher restore for `gpif_soft_stop` and
  `stop_under_backpressure` (merged via PR #108).
- README.md §"Firmware Robustness" — currently aspirational; aligned
  with code progressively through PR sequencing.
