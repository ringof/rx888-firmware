# Plan: Spectre Docker Test Environment for RX888mk2 Firmware

A second test harness alongside `docker/ka9q-radio/`, validating
`SDDC_FX3.img` against a different host stack: SoapySDR-SDDC →
GNU Radio (via [Spectre](https://github.com/spectregrams/spectre)).
A green run transitively certifies the firmware on the
SoapySDR-SDDC seam, so any GNU Radio host using `driver=SDDC` is
covered — Spectre is the proxy test, not the only target.

## Decisions

- **Image strategy: A — upstream images unchanged.** Use
  `spectregrams/spectre-server:3.5.0-alpha` and `spectre-cli:3.5.0-alpha`
  via compose; no custom Dockerfile. Recon confirmed SoapySDR-SDDC is
  built into the image from `spectregrams/ExtIO_sddc v0.0.1`, and an
  `rx888mk2` receiver (mode `fixed_center_frequency`) is a first-class
  citizen in `backend/src/spectre_server/core/receivers/`.
- **Firmware injection: layered.** A one-shot `rx888-uploader`
  sidecar runs `rx888_stream -f /firmware/SDDC_FX3.img` before
  `spectre-server` starts (compose `service_completed_successfully`),
  AND the same `.img` is bind-mounted into `spectre-server` at
  libsddc's resolved firmware path. Either layer alone has failure
  modes the other covers (warm boot, mid-session re-upload, udev
  races, upstream path drift); combined, the harness provably tests
  *our* firmware regardless of libsddc internals.
- **Validation signal: bench sig-gen at 10 MHz, –30 dBm.** Canned
  config `rx888-siggen-10mhz.json` captures 8 MHz around 10 MHz for
  10 s; expected plot is a flat noise floor with one bright carrier
  at 10.000 MHz.

## Relationship to upstream Spectre

[spectregrams/spectre#239](https://github.com/spectregrams/spectre/issues/239)
(opened 2026-05-13) proposes Spectre adopt `ringof/rx888-firmware`
as the bundled firmware source; per author context, gated on a 1.0
release here. Implications:

- Direction reverses post-adoption — their build will fetch our
  release asset; today we override their bundled blob.
- This branch's validation PNG is the adoption evidence #239 needs;
  the reference plot should be captured against a release-candidate
  build, not a dev branch.
- The layered firmware-injection design doesn't change post-#239;
  only the source-of-truth for `SDDC_FX3.img` does (local build →
  release-asset URL on both sides).
- Defining 1.0 release criteria and filing a local tracking issue
  for #239 are out of scope for this branch.

## Deliverables

```
docker/spectre/
├── docker-compose.yml      # Pinned upstream images + USB/udev +
│                           # rx888-uploader sidecar + bind-mounts
├── uploader/
│   └── Dockerfile          # debian-slim + libusb + rx888_stream
├── spectre.sh              # Helper: up/down/reflash/status/cli/validate/clean
├── configs/
│   └── rx888-siggen-10mhz.json
├── patches/README.md       # Placeholder, mirrors ka9q-radio layout
└── README.md               # Hardware setup, run, validation, regression

docs/spectre-compat-audit.md  # Mirrors docs/ka9q-compat-audit.md style
```

## Tasks

1. **Recon (live host, not committed):** discover libsddc's
   firmware search path inside `spectre-server:3.5.0-alpha` via
   `docker exec ... strings /usr/local/lib/libsddc.so | grep -E '\.(img|fw)$'`
   and `find / -name 'SDDC_FX3*'`. Record in `docs/spectre-compat-audit.md`.
2. **`docker-compose.yml`:** pin both image tags, mount
   `/dev/bus/usb` + `/run/udev:ro`, bind-mount `SDDC_FX3/SDDC_FX3.img`
   at libsddc's path (from task 1), bind-mount `./capture/` for
   PNG/FITS outputs, wire `rx888-uploader` with
   `depends_on: { rx888-uploader: { condition: service_completed_successfully } }`.
3. **`uploader/Dockerfile`:** debian-slim + `libusb-1.0-0` + a built
   `rx888_stream` from `tests/rx888_tools`. Entrypoint detects PID
   `04b4:00f3` → uploads; `04b4:00f1` → no-op; neither → fails.
4. **`spectre.sh`:** `up`, `down`, `reflash` (re-run uploader only),
   `status` (`SoapySDRUtil --find=driver=SDDC` + `lsusb -d 04b4:`),
   `cli`, `validate`, `clean`.
5. **`configs/rx888-siggen-10mhz.json`:** receiver `rx888mk2`, mode
   `fixed_center_frequency`, center 10 MHz, rate 8 MHz, 10 s, tag
   `rx888-siggen-10mhz`. Param keys confirmed against
   `spectre create config --help` at first run.
6. **`README.md`:** mirrors `docker/ka9q-radio/README.md` —
   `lsusb -d 04b4:` walkthrough, run instructions, validation
   procedure with expected plot description, regression note
   (ka9q-radio container still works against same firmware after
   Spectre run), pointer to compat audit.
7. **`docs/spectre-compat-audit.md`:** mirrors
   `docs/ka9q-compat-audit.md`'s structure; initially holds recon
   findings (libsddc fork SHA, firmware path, receiver/mode names).
8. **Doc claim (gated on green real-hardware run):** add "Spectre"
   and "SoapySDR-SDDC + GNU Radio" sections to `docs/compatibility.md`
   and update `README.md`'s host-app list. Lands as a later commit
   on this branch, only after the validation test produces the
   expected plot.

## Validation test

Bench: signal generator → RX888mk2 SMA, single CW tone at 10.000 MHz,
–30 dBm.

1. `./spectre.sh up` — uploader runs, FX3 ends at PID `00f1`,
   spectre-server comes up.
2. `./spectre.sh status` → one `driver=SDDC` device.
3. Via `spectre-cli`: `spectre create config --receiver rx888mk2
   --mode fixed_center_frequency --tag rx888-siggen-10mhz
   --param center_freq=10000000 --param sample_rate=8000000`
   → `spectre record spectrograms --tag rx888-siggen-10mhz
   --duration 10` → `spectre create plot --tag rx888-siggen-10mhz
   --obs-date ... --start-time ... --end-time ...`.
4. `./capture/<tag>/<date>/*.png` exists, >50 kB.
5. Expected: flat noise floor across ~6–14 MHz, single bright
   horizontal line at 10.000 MHz, no aliasing wraps. Reference image
   committed to `docs/images/spectre-validation-expected.png` after
   first successful real-hardware run.

## Regression test

After a green Spectre run on a given `SDDC_FX3.img`:

1. `./spectre.sh down`.
2. `cd ../ka9q-radio && ./ka9q.sh start` — same firmware, no
   rebuild, no re-flash.
3. ka9q-radio streams normally; `./ka9q.sh stop`.

Both pipelines independently consuming the same firmware in the
same session proves the firmware isn't silently coupled to one
host stack.

## Open question

Libsddc's exact firmware search path inside
`spectre-server:3.5.0-alpha` is the only unknown remaining. It
must be resolved by task 1 on a host with docker running; until
then, the bind-mount target in task 2 is parameterized as `${SDDC_FW_PATH}`
with candidate values `/usr/local/share/sddc/SDDC_FX3.img` and the
container WORKDIR. README documents the one-liner that resolves it.
