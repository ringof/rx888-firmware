# Changelog

All notable changes to this firmware are documented here.

The format is loosely based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
uses [semantic-ish versioning](https://semver.org/) — see the
[releases page](https://github.com/ringof/rx888-firmware/releases) for
the canonical list of artifacts.

Firmware-reported version (`FIRMWARE_VER_MAJOR.FIRMWARE_VER_MINOR` in
`SDDC_FX3/protocol.h`, queryable via the `TESTFX3` vendor command) is
tracked separately from the repo release tag and is noted per-release
below where it changes.

## [Unreleased]

_No changes yet._

## [0.1.0] — 2026-05-14

First named release.  Firmware version **2.3** (`FIRMWARE_VER_MAJOR=2`,
`FIRMWARE_VER_MINOR=3`).

### Highlights

- **ka9q-radio runs without host-side patches.**  Firmware now reports
  Si5351 CLK0 state truthfully, so ka9q-radio's direct Si5351
  programming path passes the `STARTFX3` preflight without needing a
  redundant `STARTADC`.  The container's patch 03 has been retired.
- **5-level recovery cascade** for streaming wedges, EP0 stalls, and
  main-thread freezes — including FX3 hardware-watchdog fallback for
  the worst-case lockup.  Validated under multi-hour soak.
- **GitHub Pages developer site** at
  [ringof.github.io/rx888-firmware](https://ringof.github.io/rx888-firmware/)
  with a complete USB API reference verified against the C source
  line-by-line.

### Added

- Live Si5351 CLK0_CONTROL register read inside
  `si5351_clk0_enabled()`; removed the `glAdcClockEnabled`
  host-cache flag.  (`SDDC_FX3/driver/Si5351.c`, PR #122.)
- `GETSTATS` reply extended from 24 to 26 bytes — new offsets:
  [24] raw CLK0_CONTROL register byte, [25] boolean chip-query
  result.  Backwards compatible: hosts that still request `wLength=24`
  continue to work.  (`SDDC_FX3/USBHandler.c`.)
- `clk0_chip_query` soak scenario that exercises the live I2C
  preflight check.  (`tests/fx3_cmd.c`.)
- `soak --weight NAME=N` / `-w NAME=N` flag to bias scenario rotation
  for context-dependent failure hunts.  (`tests/fx3_cmd.c`.)
- GitHub Pages site: `docs/_config.yml`, `docs/index.md`, `docs/api.md`
  (USB vendor-command reference), `docs/building.md`,
  `docs/compatibility.md`, plus Jekyll front-matter on existing docs.
  Workflow `.github/workflows/pages.yml`.  (PR #121.)
- `CHANGELOG.md` (this file).

### Changed

- `docker/ka9q-radio/patches/03-startadc-before-startfx3.patch` renamed
  to `…patch.disabled`; kept in-tree for archaeology.  Dockerfile loop
  hardened with a POSIX empty-glob guard so a zero-active-patch build
  succeeds cleanly.  (PR #122.)
- `docs/ka9q-compat-audit.md` §8 reframed: original failure mode
  preserved for context, but marked resolved firmware-side.
- `docs/vendor-protocol-plan.md` commits 1 and 2 marked DONE; commits
  3 (`GETCAPABILITIES`, `GETSTATE`) and 4 (architecture docs) remain
  open.

### Fixed

- `STARTFX3` preflight no longer rejects ka9q-radio's direct Si5351
  programming path with `LIBUSB_ERROR_PIPE` / EP0 STALL → "No rx888
  data for 5 seconds, quitting".

### Known issues

- Cold-start `startadc_mid_stream` flake at the very first scenario
  after firmware boot (≈ once per multi-thousand-cycle soak).
  Tracked in [#119](https://github.com/ringof/rx888-firmware/issues/119).
- Intermittent failures in `setarg_gap_index`
  ([#111](https://github.com/ringof/rx888-firmware/issues/111)) and
  `test_health_recovery`
  ([#113](https://github.com/ringof/rx888-firmware/issues/113)).
- Broader doc audit pending
  ([#114](https://github.com/ringof/rx888-firmware/issues/114)).
