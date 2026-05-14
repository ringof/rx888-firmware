# Plan: Spectre Docker Test Environment for RX888mk2 Firmware

Mirrors the existing `docker/ka9q-radio/` test harness with a second
independent host application — [Spectre](https://github.com/spectregrams/spectre).
The goal is a one-command flow that proves a freshly built
`SDDC_FX3.img` works end-to-end with a *second*, completely different
SoapySDR-based stack (the SDDC SoapySDR plugin → Spectre's GNU
Radio-based capture pipeline → spectrogram PNG output).

This is a **test/validation** container; it is not meant to be the
recommended way to operate the radio.

## Why Spectre

| Property | Value |
|---|---|
| Stack | GNU Radio + FFTW, SoapySDR (`driver=SDDC`) |
| Detection command | `SoapySDRUtil --find="driver=SDDC"` |
| Upstream images | `spectregrams/spectre-server:3.5.0-alpha`, `spectregrams/spectre-cli:3.5.0-alpha` |
| Receiver names in CLI | upstream `--receiver rx888` (to be confirmed against installed image) |
| Output | FITS + PNG spectrogram, `.fc32` I/Q |
| RX-888 MK II status (per wiki) | Supported ✅ |

Spectre tests a different code path than ka9q-radio:

- **SoapySDR + libsddc** firmware upload path (vs. ka9q's bespoke
  `ezusb.c`)
- **GNU Radio block graph** ingest (vs. ka9q's hand-rolled multicast
  RTP fan-out)
- **FFTW** spectrogram generation (similar to ka9q's, but driven by
  GNU Radio's `logpwrfft` block — a different consumer of the same
  raw stream)

If both ka9q-radio *and* Spectre stream cleanly from the same
firmware build, the firmware's USB/GPIF behaviour is portable across
the two dominant Linux SDR ecosystems.

### Wider compatibility claim: SoapySDR-SDDC + GNU Radio

A green Spectre run is not just a "second host app works" datapoint
— it is a transitive certification of the **SoapySDR-SDDC →
GNU Radio** path for this firmware:

```
SDDC_FX3.img (this repo)
        │  libusb / GPIF
        ▼
libsddc  (spectregrams/ExtIO_sddc fork, baked into spectre-server)
        │  SoapySDR C++ API  (driver=SDDC)
        ▼
SoapySDR Source block  (gr-soapy, stock GNU Radio component)
        │  GNU Radio scheduler, complex64 stream
        ▼
*any* GNU Radio flowgraph  (Spectre is just one consumer)
```

Spectre is the proxy test, but the link being exercised
(`firmware → libsddc → Soapy → GNU Radio Source`) is exactly the
same link every other GNU-Radio-based RX888 consumer uses:
hand-rolled GRC flowgraphs, `gqrx` (when built with Soapy),
`urh`, `gr-satellites`, custom research code, etc.  None of those
need any RX888-specific code on their side — they ask SoapySDR for
`driver=SDDC` and the firmware does the rest.

**Consequence**: once this container produces the expected plot,
the project can credibly state in `README.md` / `docs/compatibility.md`
that the firmware is compatible with the **GNU Radio ecosystem via
SoapySDR-SDDC**, not merely with Spectre.  Documenting that
explicitly is itself a deliverable of this branch (see Task 5b
below).

## Deliverables

```
docker/spectre/
├── Dockerfile              # OPTIONAL — see "Image strategy" below
├── docker-compose.yml      # Pinned upstream images + USB/udev wiring
├── entrypoint.sh           # (only if we build a custom image) firmware
│                           # upload + wisdom warm-up, mirroring ka9q
├── spectre.sh              # Helper: build/up/cli/capture/plot/down
├── configs/
│   └── rx888-hf-wwv.json   # Pre-canned capture config used by validation
├── patches/                # Empty initially; matches ka9q-radio layout
│   └── README.md
└── README.md               # Setup, run, validation, regression notes
```

Plus, in `docs/`:

```
docs/spectre-compat-audit.md
```

— short compatibility-audit doc, same style as
`docs/ka9q-compat-audit.md`, capturing whatever quirks surface when
we wire it up (firmware upload latency, udev requirement, etc.).

## Image strategy — two options

**Option A (recommended): Use the upstream `spectregrams/*` images
directly via docker-compose, no custom Dockerfile.**

Pros:

- Zero maintenance burden on our side for the Spectre stack itself.
- Tracks upstream's pinned `3.5.0-alpha` tag; bumping is a one-line
  `docker-compose.yml` edit.
- The SoapySDR-SDDC plugin inside `spectre-server` already knows how
  to upload `SDDC_FX3.img` if pointed at it — we just need to expose
  our newly built image to the container at the path the plugin
  expects.

Cons:

- We're a guest in the upstream image; have to discover the firmware
  search path (`/usr/share/sddc/SDDC_FX3.img` or similar) at run time
  rather than baking it.
- We can't preload FFTW wisdom into the image.

**Option B: Build our own derived image (`FROM
spectregrams/spectre-server:3.5.0-alpha`) that overlays our firmware
and an entrypoint.**

Pros:

- Self-contained: `docker build` and the firmware is in place.
- Symmetric with `docker/ka9q-radio/` (also builds from source).

Cons:

- Couples our test to a specific upstream tag at build time, not just
  at runtime.
- More code/CI surface to maintain.

**Recommendation: start with A**, fall back to B only if the upstream
image refuses to pick up a bind-mounted firmware path. The first task
below is exactly the experiment that decides this.

## Tasks

1. **Reconnaissance against upstream image** (one-off, no commits)
    - `docker pull spectregrams/spectre-server:3.5.0-alpha`
    - `docker run --rm --entrypoint bash spectregrams/spectre-server:3.5.0-alpha
       -c 'find / -name "SDDC_FX3*" 2>/dev/null; SoapySDRUtil --info'`
    - Confirm: (a) SDDC SoapySDR module is present, (b) where it
      looks for `SDDC_FX3.img`, (c) which `spectre create config
      --receiver <name>` value maps to it.
    - Record findings in `docs/spectre-compat-audit.md`.
    - Output: decide Option A vs Option B.

2. **`docker/spectre/docker-compose.yml`**
    - Pin `spectregrams/spectre-server:3.5.0-alpha` and
      `spectregrams/spectre-cli:3.5.0-alpha` (explicit tags, no
      `latest`).
    - Bind-mount `/dev/bus/usb`, `/run/udev:ro` (same rationale as
      ka9q — libusb hotplug after PID flip).
    - Bind-mount our `SDDC_FX3/SDDC_FX3.img` to wherever step 1 says
      libsddc/SoapySDR-SDDC expects it.
    - Bind-mount a host `./capture/` directory to the server's data
      volume so PNG/FITS outputs land on the host without a final
      `spectre get files` copy step.
    - Comment every non-obvious line; specifically why
      `--privileged`-equivalent (`device: /dev/bus/usb` plus
      cap-adds, NOT `privileged: true` if avoidable) is needed.

3. **`docker/spectre/spectre.sh`** (helper, parallels `ka9q.sh`)
    - `up`                — `docker compose up -d`
    - `down`              — `docker compose down`
    - `status`            — show device detection
                            (`SoapySDRUtil --find="driver=SDDC"`)
    - `cli ...`           — `docker exec -it spectre-cli spectre $@`
    - `validate`          — runs the canned end-to-end capture
                            described in "Validation test" below and
                            returns non-zero if the resulting PNG is
                            absent or below a size threshold (cheap
                            sanity).
    - `clean`             — wipe `./capture/` and the `spectre-data-v3`
                            volume.

4. **`docker/spectre/configs/rx888-siggen-10mhz.json`** (canned config)
    - Receiver: `rx888mk2`
    - Mode: `fixed_center_frequency`
    - Centre freq: 10.000 MHz (matches a sig-gen tone on the SMA
      input)
    - Sample rate: 8 MHz (HF direct-sampling; trade-off documented
      in the README)
    - Duration: 10 s
    - FFT bin width / window: sensible defaults producing a 1024-bin
      spectrogram
    - Tag: `rx888-siggen-10mhz`

5. **`docker/spectre/README.md`** (modelled after
   `docker/ka9q-radio/README.md`)
    - Hardware setup section — exactly the same `lsusb` /
      `04b4:00f3` vs `00f1` walkthrough as the ka9q README, so the
      two are interchangeable.
    - "Build" section — note that the only build step is
      `docker compose pull` (Option A), or
      `docker build -t spectre-rx888 docker/spectre/` (Option B).
    - "Load our firmware" section — what file goes where, how to
      verify it is the firmware that booted (USB PID 00F1, plus an
      in-container `dmesg`-or-equivalent check).
    - "Validation test" section — the WWV capture procedure (see
      below), including the expected PNG appearance.
    - "Regression test" section — re-running the ka9q-radio
      container against the same firmware after the Spectre test
      should still succeed; both pipelines should produce live
      streams independently.
    - Known compatibility notes section (initially empty; fill from
      `docs/spectre-compat-audit.md`).

6. **`docs/spectre-compat-audit.md`**
    - Mirror `docs/ka9q-compat-audit.md`'s structure.
    - One-line summary per identified issue: severity, where it
      lives (upstream/libsddc/firmware), whether we patched, link
      to patch.
    - Initially: just the recon findings from task 1.

7. **Update `docs/compatibility.md` and `README.md` (Task 5b — the
   GNU Radio claim)**
    - Add a new "## Spectre" section to `docs/compatibility.md`
      paralleling the existing "## ka9q-radio" section, pointing
      at `docker/spectre/` and `docs/spectre-compat-audit.md`.
    - Add a new "## SoapySDR-SDDC + GNU Radio" section to
      `docs/compatibility.md` immediately after it, framing the
      Spectre container as the *validation harness* for a broader
      compatibility claim: any Soapy-aware GNU Radio host
      (`driver=SDDC`) can consume this firmware without
      RX888-specific code on its side.  Be precise about what is
      and is not validated — we only run one flowgraph (Spectre's
      fixed_center_frequency mode), so the claim is "validated on
      the SoapySDR-SDDC seam, not on every block downstream of it".
      Reference the diagram from PLAN_SPECTRE_DOCKER.md inline.
    - Add the same hosts to the bullet list in the top-level
      `README.md`'s "Compatible host applications" section.
    - Do NOT make this claim until the validation test in Task 5
      has actually been run on real hardware and produced the
      expected plot — these doc edits are *gated* on a green run
      and should ride a later commit on the same branch.

8. **CI integration** *(out of scope for this branch; capture as a
   follow-up issue)*
    - The ka9q docker test is host-attached (requires real hardware)
      and is not in CI. Spectre will be the same.

## Validation test (the "expected plot")

Bench setup: a signal generator driving the RX888mk2's HF SMA input
with a single CW tone at 10.000 MHz, –30 dBm (well above the
noise floor, well below saturation).

The README's validation procedure — and `spectre.sh validate` — does:

1. `./spectre.sh up`
2. Confirm device detection:
   `./spectre.sh status` → must list one `driver=SDDC` device
   (output of `SoapySDRUtil --find="driver=SDDC"`).
3. Inside `spectre-cli`:
   - `spectre create config --receiver rx888mk2 \
        --mode fixed_center_frequency \
        --tag rx888-siggen-10mhz \
        --param center_freq=10000000 \
        --param sample_rate=8000000`
     *(exact `--param` keys to be confirmed against the CLI help
     emitted by `spectre create config --receiver rx888mk2 --mode
     fixed_center_frequency --help` on first run; this is the
     working set per upstream's tutorial pattern.)*
   - `spectre record spectrograms --tag rx888-siggen-10mhz --duration 10`
   - `spectre create plot --tag rx888-siggen-10mhz \
        --obs-date $(date -u +%F) \
        --start-time <now-15s> --end-time <now>`
4. `./capture/<tag>/<date>/*.png` exists, >50 kB.
5. **Expected appearance**: a flat, dark noise-floor band across
   the full 8 MHz capture span (~6–14 MHz with centre at 10 MHz),
   with one bright, narrow horizontal line at 10.000 MHz running
   the full duration. No other strong tones; no aliasing wraps
   visible at the band edges. A reference image of this expected
   plot will live at
   `docs/images/spectre-validation-expected.png` (committed once
   the test has actually run on real hardware).
6. Optional second pass with the sig-gen retuned to 5.000 MHz and
   the centre frequency re-tuned to 5 MHz, to demonstrate retune
   works.

The PNG (a render of the FITS spectrogram) is what we ship back to
the user as "an expected plot."

## Regression test

After the Spectre validation test passes:

1. `./spectre.sh down`
2. `cd ../ka9q-radio && ./ka9q.sh start` (existing harness).
3. Confirm the ka9q-radio container streams from the same firmware
   image without rebuild or re-flash.
4. `./ka9q.sh stop`.

Both pipelines independently consuming the same firmware in the same
session demonstrates the firmware is not silently coupled to one
host stack.

## Reconnaissance findings (2026-05-14)

Docker daemon was unavailable in the planning environment, so the
recon was done against the upstream source tree on GitHub instead of
the pulled image.  Findings:

1. **Upstream supports RX-888 MK II natively**, as a first-class
   receiver alongside SDRplay/HackRF/RTL-SDR/USRP:
   - Receiver constant: `RX888MK2` →
     `backend/src/spectre_server/core/receivers/_names.py`
   - Receiver module:
     `backend/src/spectre_server/core/receivers/_rx888mk2.py`
   - CLI receiver name (used with `spectre create config
     --receiver …`): `rx888mk2` (lowercased per the convention
     visible in `_names.py`).
2. **Initial supported capture mode**: `fixed_center_frequency`
   (one mode, no sweep yet).
3. **SoapySDR-SDDC is baked into `spectre-server`** — built from
   the upstream fork `spectregrams/ExtIO_sddc` (tag `v0.0.1`) and
   installed into `/usr/local/lib`. The image at
   `spectregrams/spectre-server:3.5.0-alpha` already contains
   everything needed to talk to a connected RX888 over USB.
4. **No firmware blob (`SDDC_FX3.img`) is baked into the image.**
   libsddc is expected to upload firmware to the FX3 over USB on
   first `open()`, reading the `.img` from a path it discovers at
   runtime. The exact search path was NOT determinable from source
   inspection alone (it's resolved by libsddc's `unique_filename`
   logic).  The remaining recon — a 30-second `docker exec ...
   strings` / `find` against a running `spectre-server` — must
   happen on a host with the docker daemon up.  Until then, the
   README will list **candidate** bind-mount targets (most likely
   `/usr/local/share/sddc/SDDC_FX3.img` or `/app/.spectre-data/firmware/SDDC_FX3.img`),
   plus a one-liner the user runs to confirm.
5. **USB plumbing is identical to the ka9q-radio container.**
   Upstream `setup.sh` installs a udev rule for vendor `04b4`
   (Cypress FX3) under `spectre-group`, which is the GID passed
   in via `SPECTRE_GID` to `group_add` in compose. We piggy-back
   on that mechanism rather than reinventing it.

**Conclusion: Option A is the right starting point.** We use the
upstream compose stack unchanged; our addition is purely
configuration (a `docker-compose.override.yml` or our own
`docker-compose.yml` that copies the relevant fields from upstream's
and adds the firmware bind-mount) plus a helper script and
documentation.  Option B (custom `FROM …` image) is only justified
if step 4's firmware-path resolution turns out to require code
changes, which we do not yet have evidence for.

## Resolved choices (from user)

- **Image strategy**: decided after recon — **Option A** above.
- **Validation RF target**: bench sig-gen at 10 MHz. The canned
  config and the README's "expected plot" walk-through assume a
  signal generator on the SMA input rather than an over-the-air
  WWV capture.
- **GitHub issue script**: not generated; implement everything on
  the current branch `claude/add-spectre-docker-test-2qL6R`.

## Remaining open question (to answer at first run on a real host)

**Where does libsddc inside `spectre-server:3.5.0-alpha` expect
`SDDC_FX3.img`?** Resolution procedure, documented in the README:

```
# After `./spectre.sh up` (or `docker compose up -d`):
docker exec spectre-server bash -c \
  'strings /usr/local/lib/libsddc.so /usr/local/lib/SoapySDDC.so 2>/dev/null \
   | grep -iE "\.(img|fw)$|/share/sddc|/firmware" | sort -u'
docker exec spectre-server find / -name "SDDC_FX3*.img" 2>/dev/null
```

Whichever path libsddc references is the bind-mount target in
`docker-compose.yml`.  If libsddc looks in a writable working
directory (e.g. `$PWD`), we bind-mount our firmware into the
container's WORKDIR and add a one-line note in the README.
