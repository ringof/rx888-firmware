# PLAN — Synthetic-PPS short-transfer marker (issue #125)

**Branch**: `claude/synth-pps-i125`
**Issue**: #125 — *Synthetic-PPS short-transfer marker (software-only) for sample-drop detection*
**Companion HW work**: separate issue, blocked on this one
**Base**: `main` @ 3f6a6bf (post-v0.1.0 merge)

---

## Objective

Add a firmware mechanism that, on a software-driven schedule, forces a partial DMA commit on the active streaming buffer. The resulting short USB bulk transfer is detected by a libusb host tool and used as an in-band marker for sample-count integrity verification.

End state for this branch:
- Firmware exports `SYNTH_PPS = 0xB4` vendor command, with a periodic timer mode.
- Each commit increments `glPpsCount`; failures increment `glPpsCommitFailCount`. Both readable via `GETSTATS`.
- Host tool `tests/rx888_tools/pps_probe.c` detects short URBs, prints PPS events, runs integrity histogram and phase-sweep tests from the issue's acceptance criteria.
- No regression in throughput, watchdog behavior, or existing test suite.

---

## Configuration locked

| Parameter | Value | Source |
|---|---|---|
| Sample rate | 64 MSPS | Issue #125 |
| Sample width | 16-bit signed | Issue #125 |
| Stream rate | 128 MB/s | derived |
| DMA buffer size | 16384 B (16 KB) | `Application.h:43` |
| Samples per buffer | 8192 | derived |
| Buffer-time | ~128 µs | derived |
| Expected samples per PPS (1 Hz) | 64,000,000 | derived |
| Endpoint | EP1 IN (0x81), USB 3.0 bulk, 16×1024 burst | `Application.h:36`, `StartStopApplication.c:144-146` |

---

## A1 audit findings (already complete — informs everything below)

1. **DMA channel type** is `CY_U3P_DMA_TYPE_AUTO_MANY_TO_ONE` (`StartStopApplication.c:172`). AUTO mode keeps the ARM core out of the steady-state path — correct for our high-rate streaming, and `SetWrapUp` works on AUTO channels without modification.
2. **EP burst mode is not enabled** (no `CyU3PUsbEPSetBurstMode` call). Per SDK doc, the FX3 then sends one DMA buffer per USB burst transaction — DMA buffer boundaries map cleanly to USB bursts. We must not enable burst mode, or our marker scheme breaks.
3. **Canonical wrap-up API** is `CyU3PDmaMultiChannelSetWrapUp` (`cyu3dma.h:1726-1754`). This is the same idiom UVC uses for end-of-frame in AN75779 §5.7 — well-trodden code path. No consumer-suspend dance required.
4. **EP packet mode (`CyU3PUsbSetEpPktMode`) is OUT-only** per `cyu3usb.h:989` — not applicable to our IN endpoint.
5. **Active-socket tracking** is needed to know which of the two PIB producer sockets to wrap up. The existing `DmaCallback` (`StartStopApplication.c:61`) fires on `CY_U3P_DMA_CB_PROD_EVENT` for every committed buffer — natural place to track the last-produced socket. The next wrap-up target is the *other* socket (currently filling).
6. **Buffer alignment constraint**: any DMA buffer size must be a multiple of 16 bytes (AN75779 §5.6). Our 16384 B buffer is fine; any future buffer-size rework must preserve this.
7. **Exact-multiple-of-1024 partial commit behavior** is not explicitly documented in the SDK or AN75779. UVC's success with arbitrary-aligned partial commits is suggestive but not definitive. **Phase B3 phase-sweep test is the empirical check.**

---

## Phase A — Firmware

### A2. `SYNTH_PPS = 0xB4` vendor command

**Files**: `SDDC_FX3/protocol.h`, `SDDC_FX3/USBHandler.c`.

- Add enum value `SYNTH_PPS = 0xB4` to `FX3Command` in `protocol.h:14-41`, alongside `HANGFX3` / `HANGMAIN`. Document in the same "TEST + DIAGNOSTIC, safe in production" comment style — this is a permanent diagnostic, not test-only.
- `wValue` semantics:
  - `0` — stop synthetic-PPS timer.
  - `1` — start synthetic-PPS timer at 1 Hz (default).
  - `2` — fire a single immediate commit (one-shot, for bring-up).
- `wIndex` — period in milliseconds when starting. `0` or unspecified = 1000 ms. Range-check: reject if `wIndex < 10` (would saturate the timer ISR at >100 Hz) or `wIndex > 60000` (no value in supporting >60 s intervals).
- Handler skeleton in `USBHandler.c` alongside the other vendor handlers. Calls `synth_pps_start(period_ms)`, `synth_pps_stop()`, or `synth_pps_oneshot()` from the new module.
- Update `protocol.h:96` `FX3_CMD_COUNT` from 17 to 18 and add `"SYNTH_PPS"` to the debug command-name lookup table.

### A3. Synthetic-PPS timer + wrap-up

**Files**: new `SDDC_FX3/synth_pps.c`, `SDDC_FX3/synth_pps.h`, integration in `SDDC_FX3/RunApplication.c`.

- New module `synth_pps.{c,h}` with:
  - `void synth_pps_init(void)` — called once from app init; creates the CyU3P timer.
  - `void synth_pps_start(uint32_t period_ms)` — arm the timer.
  - `void synth_pps_stop(void)` — disarm.
  - `void synth_pps_oneshot(void)` — fire one commit immediately (calls the wrap-up path directly, not via timer).
  - Internal `static void SynthPpsTick(uint32_t param)` — timer callback; calls `CyU3PDmaMultiChannelSetWrapUp(&glMultiChHandleSlFifoPtoU, target_socket)` where `target_socket` is the socket *not* most-recently completed.
- Active-socket tracking: extend `DmaCallback` (`StartStopApplication.c:61`) to record which socket index produced the just-completed buffer. Single `uint8_t glLastProducedSocket` global, written from DMA callback context (single-writer, ARM-context read in timer callback — no lock needed on 32-bit ARM with aligned access).
- Wrap-up guard: `synth_pps` does nothing if `glIsApplnActive == CyFalse`. The timer can stay armed across stop/start without misbehaving — the tick becomes a no-op.
- Increment `glPpsCount` on each successful `SetWrapUp`. Increment `glPpsCommitFailCount` if it returns non-success. Both `uint32_t`, declared in `synth_pps.c`, extern'd via `synth_pps.h`.

### A4. `GETSTATS` exposure

**Files**: `SDDC_FX3/USBHandler.c` (GETSTATS handler), `docs/diagnostics_side_channel.md`, `docs/api.md`.

- Extend GETSTATS reply from 26 → 34 bytes:
  - Bytes 26-29: `glPpsCount` (LE uint32)
  - Bytes 30-33: `glPpsCommitFailCount` (LE uint32)
- Hosts that request `wLength=26` continue to work (firmware returns the requested prefix; behavior is consistent with how the 24→26 transition was handled in #122).
- Update `docs/diagnostics_side_channel.md` and `docs/api.md` GETSTATS layout tables to add offsets 26-33 with the new fields. Follow the working rules from PR #123: no file:line callouts.

### A5. Watchdog compatibility check

**Files**: `SDDC_FX3/RunApplication.c:248-345`.

- Verify that synthetic commits don't trip the GPIF stall watchdog. The watchdog checks `glDMACount == prevDMACount`; synthetic commits *increment* `glDMACount` (because `SetWrapUp` triggers the produce-event path that hits `DmaCallback`). So if anything, synthetic commits make stalls *less* visible. Confirm via a run with synth-PPS active and no real streaming load.
- State-number magic numbers `5/7/8/9` are unchanged (no GPIF SM change in this branch).
- No code changes expected; this is verification only.

---

## Phase B — Host tooling

### B1. `pps_probe` tool

**Files**: new `tests/rx888_tools/pps_probe.c`, `tests/rx888_tools/Makefile` (extend existing).

- libusb-only, no SDK dependency. Build target alongside existing tools.
- Behavior:
  1. Open device by VID/PID, claim interface 0.
  2. Send `STARTADC` (0xB2) at 64 MSPS.
  3. Send `STARTFX3` (0xAA).
  4. Queue 8 async bulk IN transfers of 16384 bytes each on EP1 IN.
  5. Send `SYNTH_PPS` (0xB4, wValue=1, wIndex=1000) to start 1 Hz commits.
  6. Completion callback:
     ```c
     uint64_t pps_sample = total_samples + (xfer->actual_length / 2);
     total_samples += xfer->actual_length / 2;
     if (xfer->actual_length < xfer->length) {
         record_pps_event(pps_sample, get_wallclock_ns(), xfer->actual_length);
     }
     libusb_submit_transfer(xfer);
     ```
  7. On SIGINT: stop synth-PPS, drain pending URBs, dump event log + integrity stats.
- Total-samples is `uint64_t`. No rollover concern within experiment lifetimes at 64 MSPS.
- Sample-width division is by 2 (16-bit signed samples).

### B2. Integrity histogram analysis

**Files**: in `pps_probe.c` or companion Python script `tests/rx888_tools/pps_analyze.py`.

- For each pair of consecutive PPS events, compute `delta = pps_sample[N+1] - pps_sample[N]`.
- Expected nominal: 64,000,000 at 1 Hz.
- Plot residuals (`delta - expected`) as a histogram.
- Acceptance: residuals within ±1 sample, zero outliers, FW `glPpsCount` matches host's short-URB count exactly.

### B3. Phase-sweep test

**Goal**: validate that partial commits at every byte offset within a 16384-byte buffer produce a host-visible short URB. This is the empirical answer to the exact-multiple-of-1024 ZLP question.

- Run `pps_probe` with `wIndex=50` (20 Hz synthetic PPS) for 50+ seconds to collect 1000+ events.
- For each event, record `actual_length` (the buffer phase where the commit landed).
- Bin into 64 bins (256-byte resolution).
- Acceptance: every bin from byte 1 to byte 16383 has at least one hit; no event maps to two PPS pulses (no merge); FW count = host count.

### B4. Spectral integrity check

- Capture 60 s of samples with synth-PPS off and 60 s with it on at 1 Hz.
- FFT both. Acceptance: no spectral artifact at the synth-PPS rate or harmonics in the "on" capture.

---

## Phase C — Decision gate

Synthesized from the issue's acceptance criteria:

| Result | Interpretation | Next step |
|---|---|---|
| All B tests pass | Software path complete. Issue #125 closeable. | Tag a release; consider the companion HW issue. |
| B2 residuals > ±1 sample | DMA / USB pipeline reordering or dropping bytes. | Diagnose in firmware; do not proceed to HW. |
| B3 missed bins | Exact-multiple-of-1024 case really does merge. | Fall back to `SUSP_CONS_PARTIAL_BUF` + `CommitBuffer(count)` with count rounded off the 1024-multiple. Re-run B3. |
| B4 spectral artifact | Commit path corrupts samples (padding, duplication, reordering). | Investigate immediately; this is a streaming-correctness bug. |
| FW count ≠ host count | URBs lost on host or commits lost in firmware. | Check libusb errors, EP_UNDERRUN counter, kernel logs. |

---

## Execution order (commit-per-step)

Each step below maps to one commit. Each commit's *Change Documentation* block per CLAUDE.md will be provided in chat before requesting commit approval.

1. **A2** — `SYNTH_PPS` vendor command + protocol.h enum, no behavior yet (handler is a stub returning success). Verify the vendor-command transport works.
2. **A3** — `synth_pps.{c,h}` module with timer + wrap-up, active-socket tracking in `DmaCallback`. Firmware now actually fires commits.
3. **A4** — `GETSTATS` extension to 34 bytes, doc updates.
4. **B1** — `pps_probe.c` minimal viable detector.
5. **A5 / B2 / B3 / B4** — verification commits, mostly doc / test-data captures.

Throughout: 1 hour soak + ka9q-radio docker compatibility run before each merge to main, per the working discipline established for v0.1.0. Spectre best-effort once available.

---

## Out of scope for this branch

- GPIF state machine changes (HW PPS path) — separate issue, separate branch.
- Pin repurposing (BIAS_HF / BIAS_VHF release from simple GPIO) — separate issue.
- Spectre #239 integration handoff — happens after this branch ships, in the form of a release-note pointer.
- Phase calibration, UTC anchoring, multi-receiver alignment — downstream consumer concerns, not firmware-side.

---

## References

In-conversation:
- `PPSTimingArch1.pdf` — timing architecture context, defines the in-band-marker concept
- `WB6CXC-Xenia2026.pdf` — TAPRX-888 / TIS-TS1 / AC0G timing taxonomy context

External (now in conversation context):
- **AN75779** §4 + §5.7 — canonical UVC partial-commit / forced wrap-up pattern; confirms `SetWrapUp` is the right API
- **AN84868** §3.5 — I/O matrix reconfiguration (relevant to the HW path, not this branch)

In-repo (FX3 SDK):
- `SDK/fw_lib/1_3_4/inc/cyu3dma.h` — `CyU3PDmaMultiChannelSetWrapUp` (lines 1726-1754), channel types, suspend options
- `SDK/fw_lib/1_3_4/inc/cyu3usb.h` — EP config, burst mode, packet mode
- `SDK/fw_lib/1_3_4/inc/cyu3os.h` — CyU3P timer API

In-repo (firmware):
- `SDDC_FX3/StartStopApplication.c:61, 141-176, 154` — DmaCallback, EP config, multi-channel create
- `SDDC_FX3/Application.h:33-44` — buffer/burst constants
- `SDDC_FX3/USBHandler.c` — vendor-command dispatch
- `SDDC_FX3/protocol.h:14-41` — command enum + permanent-diagnostic pattern (HANGFX3/HANGMAIN)

In-repo (docs to update):
- `docs/api.md` — vendor command surface
- `docs/diagnostics_side_channel.md` — GETSTATS wire layout
- `docs/architecture.md` — pipeline section, in-band marker mention
- `docs/vendor-protocol-plan.md` — planning ledger
- `SDDC_FX3/docs/debugging.md` — optional debug trace

---

## Per-CLAUDE.md compliance

- All work on the currently-checked-out branch `claude/synth-pps-i125`. No further branches created without explicit instruction.
- No commits without explicit user approval. Each commit will be preceded by a copy-pastable change documentation block (description, build instructions, validation, regression).
- No emoji unless requested.
- No file:line callouts in committed docs (PR #123 working rule).
- 1-hour soak + ka9q-radio docker run before each merge to main.
