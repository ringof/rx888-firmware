# PLAN.md — RX888 Hardware-in-the-Loop Test Harness

## 1. Goal

Catch firmware-induced regressions to the RX888 RF chain without a
live operator test on every change. The dominant failure mode of a
firmware change is "the chain still works but it works *worse*";
this harness exists to surface that class of regression. Designed as
nearly all USB-in / USB-out: one command in, one PASS line out.

## 2. Bench topology

```
   ┌──────────┐  USB2   ┌──────────────┐   USB3   ┌──────────┐
   │   QDX    │◄────────┤ host (docker)├─────────►│  RX888   │
   │  (TX)    │ CAT+aud │  ka9q-radio  │ stream   │  (RX)    │
   └────┬─────┘         │  wsprdaemon  │          └────┬─────┘
        │ RF out             HITLtest                  │ RF in
        └─────► [fixed attenuator pad] ────────────────┘
```

QDX exposes both USB-CDC serial (CAT) and USB audio. Container
generates the WSPR/FT8/etc. audio, plays into the QDX's USB sound
device, and asserts PTT/frequency via CAT. RX888 streams to
`radiod`. Open bench, no shielding — tolerance bands accept ambient
QRM drift.

## 3. Phase ladder

| Phase | Deliverable | Effort | Exit criterion |
|------:|-------------|--------|----------------|
| 1   | Container has audio + CAT plumbing; WSPR round-trip via QDX, decode/no-decode | ~3 days | Operator runs `HITLtest`, PASS on good fw, FAIL with RF unplugged |
| 1.5 | CSV log of wsprdaemon-reported SNR per PASS | ~½ day | Data accumulating from day one |
| 1.7 | CW carrier subtest (CAT-only, reads `radiod` channel power) | ~1 day | Seconds-scale subtest works |
| 1.8 | FT8 subtest (reuses Phase 1 audio plumbing, 15 s cadence) | ~½ day | FT8 round-trip works |
| 2   | Empirical SNR-delta gate vs. baseline per mode | ~1 day once data exists | `HITLtest --gate` exits non-zero on regression |
| 3   | Proper `radiod` status TLV→JSON reader + self-hosted CI runner workflow | ~1–2 weeks | Per-PR gate green on bench runner |
| 4 (deferred) | MDS sweep, IQ-domain metrics | unscoped | — |

The ordering is deliberate: ship the weakest gate fast, accumulate
data, set tolerance empirically, then automate. Once the Phase 1
audio path lands, every WSJT-family mode (WSPR/FT8/FT4/JS8/Q65/JT9)
is roughly a free addition — only the WAV and cadence change.

## 4. Phase 1 detail

### 4.1 Container additions (`docker/ka9q-radio`)

- `alsa-utils` (`aplay`) for audio out to the QDX sound device.
- A WSPR WAV generator (`wsprgen`/`WSPRcode` from the WSJT-X tree,
  or pre-baked WAVs committed to `tests/hitl/wav/`).
- `hamlib`'s `rigctl`, or a tiny direct-serial CAT helper for the
  QDX's Kenwood-style protocol.
- The `HITLtest` entrypoint script.
- Updated README covering cabling, audio-device pinning via udev,
  NTP requirement, and how to invoke.

### 4.2 Script flow

1. **Pre-flight.** Confirm RX888 enumerates, QDX serial + sound
   devices present, host clock NTP-synced to within ~100 ms,
   `radiod`/`wsprdaemon` running.
2. **Generate.** Produce the WSPR WAV for the configured
   callsign/grid/power (or use a pre-baked one).
3. **Align + arm.** Wait for the next even UTC minute. Set
   frequency via CAT, assert PTT.
4. **Transmit.** `aplay` the WAV (~110.6 s) to the QDX sound
   device. Release PTT on completion.
5. **Decode wait.** Sleep ~70 s for wsprdaemon to process the slot.
6. **Check.** Grep wsprdaemon's spot output for the expected
   callsign in that slot.
7. **Report.** Print `HITL PASS: ... SNR=<x> dB freq=<f>` or
   `HITL FAIL: no decode for <call> in slot <utc>`. Exit 0/1.

### 4.3 Success criterion

- On known-good firmware: `HITLtest` prints PASS.
- With QDX powered off (or RF cable disconnected): prints FAIL
  within the same wall-clock window.

No baseline file, no CI wiring yet.

## 5. Phases 1.5 – 4 (one paragraph each)

**1.5 — Passive SNR logging.** `HITLtest` appends one CSV row per
PASS: UTC, firmware SHA, mode, band, reported SNR, frequency
offset. No gating change. Starts variance data accumulating
immediately.

**1.7 — CW carrier subtest.** Re-uses Phase 1 CAT plumbing without
the audio path: set frequency, key down for 5 s via CAT, key up.
Receiver side reads `radiod`'s status multicast for channel power /
noise density / SNR at the carrier frequency. Seconds-scale subtest;
becomes the per-PR CI gate at Phase 3.

**1.8 — FT8 subtest.** Same audio plumbing, different WAV (FT8 is
12.6 s of audio in a 15 s slot). 8× the cycle cadence of WSPR —
much better for any iterated testing.

**2 — Baseline + gate.** Once Phase 1.5/1.7/1.8 have produced a few
weeks of data across day/night, write `tests/hitl/baseline.json` with
per-mode/per-band SNR mean and tolerance (e.g. mean − 2σ).
`HITLtest --gate` exits non-zero on regression even if decode
succeeded. `HITLtest --rebaseline` is a deliberate, human-invoked
command. Never automatic.

**3 — Status reader + CI.** Replace 1.7's ad-hoc parser with a
proper `radiod` status-multicast TLV→JSON tool (~100 lines).
Register a self-hosted runner *org-level* into a runner group
restricted to the specific repos that may use it. Workflow
`.github/workflows/hitl.yml` pinned to `runs-on: [self-hosted,
rx888-bench]`, gated by a GitHub Environment with required
reviewers, with a `flock`-on-host mutex around the test step
(GitHub `concurrency:` does not span repos). Pre-flight verifies
hardware. Ephemeral runner mode. PR-gated for firmware changes;
nightly cron runs the full WSPR path.

**4 (deferred) — Fancy.** MDS sweep via programmable attenuator,
IQ-domain metrics (image rejection, spur survey) from `radiod` raw
output. Each is its own plan.

## 6. Open items and risks

- **QDX is not a beacon.** Unlike the U3S, the QDX requires a host
  to generate the WSPR audio — hence the audio path in Phase 1.
  Callsign/grid/power live in the WAV, not in the radio's EEPROM.
- **wsprdaemon spot log path/format** — to confirm during
  bring-up; document the grep target in the script.
- **Audio-device pinning.** QDX shows up as an ALSA card; pin via
  udev rule using its USB IDs so device-node enumeration order
  doesn't matter.
- **NTP discipline.** WSPR cycles align to even UTC minute; bench
  host clock must be NTP-synced. Pre-flight enforces.
- **PTT timing.** Verify on bring-up that PTT-assert → audio-out
  has no front-end clipping.
- **Open-bench QRM drift.** Phase 2 tolerance bands must absorb
  day/night variance. If Phase 1.5 data shows variance is too wide
  to gate usefully, shielding becomes a prerequisite *before*
  Phase 3 runner work.
- **Public-repo + self-hosted runner risk.** Phase 3 requires the
  fork-PR approval gate, ephemeral runner mode, dedicated runner
  host on a segmented VLAN, and no personal credentials on the
  host. The locked-down repo settings tracked separately must
  precede the runner.
- **Single test band in Phase 1.** 20 m suggested. Multi-band is a
  Phase 2/3 extension.
