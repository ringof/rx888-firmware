# ka9q-radio patches applied in this container

These patches are applied during the Docker build (see Dockerfile)
against the pinned `KA9Q_RADIO_SHA`.  Each patch is a deliberate ask
of the upstream maintainer; the set is kept minimal so that asks are
focused on real, irreducible incompatibilities — not noise.

| File | What | Why |
|------|------|-----|
| `03-startadc-before-startfx3.patch` | Insert `command_send(...,STARTADC,samprate)` before `STARTFX3` in `rx888_start_rx`. | ka9q programs the Si5351 directly via `I2CWFX3` and never sends `STARTADC`.  SDDC firmware's `STARTFX3` runs `GpifPreflightCheck()` which requires `glAdcClockEnabled = CyTrue`, a flag only set by the `STARTADC` handler.  Without this patch, `STARTFX3` stalls EP0, the GPIF state machine never starts, and radiod times out with "No rx888 data for 5 seconds".  No workaround at the host or container level. |

When upstream ka9q-radio merges this fix, bump `KA9Q_RADIO_SHA` in
the Dockerfile and remove this patch.

## Findings that did *not* become patches

The audit (`docs/ka9q-compat-audit.md`) identified two further
ka9q-side behaviors that are observable but not blocking:

- **`sleep(1)` after firmware upload** (`rx888.c:700`).  Upstream's
  fixed 1 s wait is fragile in principle, but it is sufficient on
  every host we have tested when the container is run with
  `-v /run/udev:/run/udev:ro` (so libusb's hotplug listener sees
  fresh device events).  A polling loop would be more robust on
  pathologically slow hosts but is not required.  Documented in
  audit §1.

- **`TUNERSTDBY` (0xB8) on the HF path** (`rx888.c:943`,
  `rx888.c:1003`).  Returns a clean USB STALL from SDDC firmware's
  default handler; ka9q's `command_send` ignores the return value,
  so streaming is unaffected.  Cosmetic only — the bus is noisier
  than it needs to be.  Documented in audit §2.
