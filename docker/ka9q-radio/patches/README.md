# ka9q-radio patches applied in this container

These patches are applied during the Docker build (see Dockerfile)
against the pinned `KA9Q_RADIO_SHA`.  Each patch is a deliberate ask
of the upstream maintainer; the set is kept minimal so that asks are
focused on real, irreducible incompatibilities — not noise.

Currently no `*.patch` files are active.  The Dockerfile's
`COPY patches/*.patch` step is guarded against an empty match so the
build succeeds with zero patches applied.

## Disabled / historical

| File | What it was | Why it's disabled |
|------|-------------|-------------------|
| `03-startadc-before-startfx3.patch.disabled` | Inserted `command_send(...,STARTADC,samprate)` before `STARTFX3` in `rx888_start_rx` to wake the firmware's `glAdcClockEnabled` host-cache flag. | Superseded by SDDC_FX3 commit `f3835a5` on branch `claude/si5351-chip-query`: `si5351_clk0_enabled()` now reads CLK0_CONTROL reg 16 bit 7 from the Si5351 directly, so `GpifPreflightCheck()` sees the chip's real state when ka9q programs Si5351 via `I2CWFX3`.  No host-side workaround needed.  File kept in-tree with `.disabled` suffix (skipped by the `*.patch` glob) for archaeology. |

## Findings that did *not* become patches

The audit (`docs/ka9q-compat-audit.md`) identified two further
ka9q-side behaviors that are observable but not blocking:

- **`sleep(1)` after firmware upload** in ka9q's `rx888_usb_init()`.
  Upstream's fixed 1 s wait is fragile in principle, but it is
  sufficient on every host we have tested when the container is run
  with `-v /run/udev:/run/udev:ro` (so libusb's hotplug listener sees
  fresh device events).  A polling loop would be more robust on
  pathologically slow hosts but is not required.  Documented in
  audit §1.

- **`TUNERSTDBY` (0xB8) on the HF path** in ka9q's
  `rx888_set_hf_mode()` and `rx888_start_rx()`.  Returns a clean USB
  STALL from SDDC firmware's default handler; ka9q's `command_send`
  ignores the return value, so streaming is unaffected.  Cosmetic
  only — the bus is noisier than it needs to be.  Documented in
  audit §2.
