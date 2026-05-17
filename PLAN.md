# PLAN.md — RX888 Hardware-in-the-Loop Test Harness

## 1. Goal

Catch firmware-induced regressions to the RX888 RF chain without
relying on a live operator test for every change. Most users of the
RX888 run it for WSPR spotting, and the dominant failure mode of a
firmware change is "the chain still works but it works *worse*." The
harness must surface that class of regression cheaply enough to live
in CI, so that "did WSPR performance change?" stops being a manual
question.

The HITL jig is nearly all USB-in / USB-out: a QRP Labs QDX
transmits, fixed attenuators connect to the RX888 RF input, and both
devices are USB to the host running the test container. From the
operator's point of view, a successful run is one command in, one
PASS line out.

## 2. Bench topology

```
   ┌──────────┐  USB2   ┌──────────────┐   USB3   ┌──────────┐
   │   QDX    │◄────────┤ host (docker)├─────────►│  RX888   │
   │  (TX)    │  CAT    │  ka9q-radio  │ stream   │  (RX)    │
   └────┬─────┘         │  wsprdaemon  │          └────┬─────┘
        │ RF out             HITLtest                  │ RF in
        │                                              │
        └─────► [attenuator chain, fixed dB] ──────────┘
```

- **TX side:** QDX over USB-serial (CAT only — its built-in WSPR
  beacon mode is used, no audio injection in Phase 1).
- **RX side:** RX888mk2 over USB3, streaming to `radiod`.
- **RF link:** fixed attenuator pad between QDX RF out and RX888 RF
  in. Open bench, no shielding — tolerance bands must accept ambient
  QRM drift.
- **Container:** existing `docker/ka9q-radio` image, extended with a
  CAT helper and the `HITLtest` script.

## 3. Phase ladder

| Phase | Deliverable | Effort | Gates on | Exit criterion |
|-------|-------------|--------|----------|----------------|
| 1 | Operator-run `HITLtest`, decode-or-not | ~1 day script + bench bring-up | nothing yet | Operator runs `HITLtest` on the bench, sees PASS on a known-good firmware and FAIL on a deliberately broken one |
| 1.5 | CSV logger appends per-run SNR | ~½ day | nothing | Spot SNR recorded as a run artifact; weeks of data accumulate |
| 2 | SNR-delta gate vs. empirical baseline | ~1 day once Phase 1.5 data exists | PR merge (operator-driven) | Tolerance bands set from real data; harness exits non-zero on regression |
| 3 | CW tone + `radiod` status-multicast reader; self-hosted GH runner workflow | ~1–2 weeks | every PR touching firmware | Per-PR CI gate green on the bench runner |
| 4 (deferred) | FT8 path, MDS sweep, IQ-domain metrics | unscoped | tighter assertions | Out of scope for this plan |

The ordering is deliberate: ship the weak gate fast, accumulate data,
let empirical tolerance set the stronger gate, then automate. Trust
in the harness is built progressively — by the time the per-PR CI
gate lands in Phase 3, the bench operator already knows what numbers
to expect.

## 4. Phase 1 detail — operator-run `HITLtest`

### 4.1 Container additions

- Serial-port tooling (`socat`, basic `stty` already present) for
  talking to the QDX's USB-CDC node.
- A small CAT helper script (`qdx-cat.sh` or similar) wrapping the
  specific CAT sequence needed to arm the QDX's built-in WSPR
  beacon: set frequency, set callsign/grid/power if not already
  provisioned in QDX EEPROM, select WSPR mode, arm next slot.
- The `HITLtest` entrypoint script.
- Documentation in `docker/ka9q-radio/README.md` covering: cable
  layout, QDX provisioning prerequisites (callsign/grid/power
  programmed once), and how to invoke `HITLtest`.

### 4.2 Script flow

`HITLtest` performs the following inside the running container:

1. **Pre-flight.** Confirm the RX888 USB3 device enumerates and the
   QDX serial node exists. Fail fast with a clear message naming the
   missing device.
2. **Stack check.** Confirm `radiod` is running with
   `rx888-test.conf` and `wsprdaemon` is up. (Both are expected to
   be started by the container's existing entrypoint; this is a
   sanity check, not a launcher.)
3. **Arm QDX.** Send the CAT sequence to set the test frequency
   (e.g. 14.0956 MHz for 20 m WSPR), select WSPR beacon mode, and
   arm transmission on the next even UTC minute.
4. **Wait.** Sleep until the next even UTC minute, then sleep an
   additional ~180 s (110.6 s TX duration + decoder slack).
5. **Check decode.** Grep `wsprdaemon`'s spot output (path to be
   confirmed during bring-up) for the QDX's provisioned callsign in
   the expected slot.
6. **Report.** Print exactly one of:
   - `HITL PASS: TX sent, RX decoded "<call> <grid> <pwr>" SNR=<x> dB freq=<f>`
   - `HITL FAIL: no decode for <call> in slot <utc>`
7. **Exit** 0 on PASS, 1 on FAIL.

### 4.3 Phase 1 success criterion

Two test runs on the bench, by the operator:

- On a known-good firmware image, `HITLtest` prints PASS.
- With the QDX powered off (or RF cable disconnected), `HITLtest`
  prints FAIL within the same wall-clock window.

That's it. No CI wiring yet. No baseline file. No tolerance.

## 5. Phase 1.5 — passive SNR logging

Add to `HITLtest`:

- On PASS, append one CSV row to `/var/log/hitl/runs.csv` (or a
  similar path mounted as a volume) containing: UTC timestamp,
  firmware git SHA (or build ID if available from a vendor command),
  RF band, reported SNR, reported frequency offset.
- No gating change. The exit code is still binary decode/no-decode.

The point is to start accumulating real-world variance data the
moment Phase 1 lands. Without this, Phase 2's tolerance band is a
guess.

## 6. Phase 2 — SNR-delta gate

Once Phase 1.5 has produced enough rows (rough target: a few weeks of
runs across day/night, ideally ≥ 50 samples on the primary test
band):

- Compute the empirical SNR distribution on the bench.
- Write a baseline JSON file (`tests/hitl/baseline.json`) keyed by
  band, recording expected SNR mean and the chosen tolerance (e.g.
  mean − 2σ as the floor).
- `HITLtest` gains a `--gate` flag: exits non-zero if measured SNR
  falls below the floor, even if the decode succeeded.
- Re-baselining is a separate explicit command (`HITLtest
  --rebaseline`) that the operator runs deliberately. Never
  automatic.

Phase 2 is still operator-driven — the gate is "did the operator
remember to run it before tagging." That's intentional; CI
automation is Phase 3.

## 7. Phase 3 — CW tone gate + CI runner

Two deliverables, in this order:

### 7.1 `radiod` status-multicast reader

A small standalone tool that joins ka9q-radio's status multicast
group, parses the TLV stream, and emits selected fields as JSON
(noise density, baseband power, SNR, IF level, frequency offset).
Approximately ~100 lines of C or Python. This is the "minimal
commands to extract knowledge from ka9q-radio" the user identified
earlier — no new DSP, just a structured view of what `radiod`
already publishes.

### 7.2 CW tone subtest

Drive the QDX to emit a CW carrier at a known frequency and power.
Configure `radiod` with a narrow channel on that frequency. Read the
status stream for ~5 seconds. Assert that the reported channel SNR,
noise density, and IF level fall within tolerance bands derived the
same way as Phase 2 (empirical, from logged runs).

This subtest takes seconds, not minutes. It becomes the per-PR CI
gate. WSPR remains available as a slower full-protocol confirmation
that can run nightly.

### 7.3 CI wiring

- Self-hosted GitHub Actions runner physically attached to the
  bench, labelled `[self-hosted, rx888-bench]`.
- Workflow `.github/workflows/hitl.yml` triggered on PRs that touch
  `SDDC_FX3/**`, runs the container with `--device` passthrough for
  both USB nodes, executes `HITLtest --quick` (CW-only path),
  uploads the CSV row and any captured status JSON as artifacts.
- Nightly cron job on the same runner runs the full WSPR test and
  appends to the long-term log.

By the time this lands, the operator has weeks of Phase 1.5 data
and a Phase 2 baseline — the per-PR gate isn't a guess.

## 8. Phase 4 (deferred, listed only)

Out of scope for this plan; named here so contributors know where
later work fits:

- FT8 audio path (WSJT-X inside the container, USB audio + CAT PTT
  to the QDX, 15 s cycles).
- Minimum-detectable-signal sweep using a programmable attenuator.
- IQ-domain metrics (image rejection, spur survey) captured straight
  from `radiod` raw output rather than via the status stream.

Each of these is its own plan when its time comes.

## 9. Open items and risks

- **QDX beacon timing accuracy.** Phase 1 assumes the QDX's internal
  scheduler hits the even-UTC-minute slot tightly enough that the
  decoder will accept it. To verify during bring-up; if not, add a
  manual PTT trigger from the host with NTP-disciplined timing.
- **wsprdaemon spot log location/format.** The grep target in step 5
  of the script needs confirmation against the actual deployment in
  `docker/ka9q-radio`. Document the path in the script.
- **QDX provisioning.** Callsign/grid/power are programmed into the
  QDX's EEPROM once via its own configuration interface. The
  harness assumes this is done; the README will call it out.
- **Container USB passthrough for the eventual runner.** Phase 3
  needs `--device=/dev/bus/usb/...` (or `--privileged` as a
  fallback) in the workflow. Confirm udev rules on the runner host
  so the device nodes are stable across reboots.
- **Baseline drift.** Open-bench QRM will move the noise floor day
  to day. Tolerance bands in Phase 2 must be wide enough to absorb
  this; if Phase 1.5 data shows the variance is too wide for a
  useful gate, that's the signal to invest in shielding before
  Phase 3, not after.
- **Single test band.** Phase 1 picks one band (20 m suggested).
  Multi-band coverage is a Phase 2/3 extension once the single-band
  path is trusted.

## 10. What this plan does *not* commit to

- Specific tolerance numbers (Phase 2/3 — empirical).
- Runner workflow YAML (Phase 3 — written when the runner exists).
- Container image rebuild strategy for the runner (Phase 3).
- Any code beyond the Phase 1 script and its CAT helper.
- A schedule. Phase 1 is days; Phase 1.5 + 2 are weeks of data
  collection plus small code; Phase 3 is the largest single chunk.
