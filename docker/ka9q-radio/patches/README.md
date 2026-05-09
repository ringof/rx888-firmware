# ka9q-radio patches applied in this container

These patches are applied during the Docker build (see Dockerfile)
against the pinned `KA9Q_RADIO_SHA`.  They are derived from the audit
in `docs/ka9q-compat-audit.md`.

| File | What | Why |
|------|------|-----|
| `01-poll-reenumeration.patch` | Replace post-upload `sleep(1)` with a 200 ms × 50 polling loop, then a 1 s settle delay. | Author's own comment admits the 1 s value is a guess; FX3 boot + udev settle on Linux can exceed 1 s, more so under Docker, causing `rx888_usb_init()` to fail with "Error or device could not be found".  The trailing 1 s settle covers kernel SuperSpeed LPM setup before the upstream rescan runs. |
| `02-drop-tunerstdby-hf.patch` | Remove `TUNERSTDBY` (0xB8) calls in `rx888_set_hf_mode` and `rx888_start_rx`. | SDDC firmware removed all R82xx tuner commands; 0xB8 STALLs.  HF receive does not need this command. |
| `03-startadc-before-startfx3.patch` | Insert `command_send(...,STARTADC,samprate)` before `STARTFX3` in `rx888_start_rx`. | ka9q programs the Si5351 directly via `I2CWFX3` and never sends `STARTADC`.  SDDC firmware's `STARTFX3` runs `GpifPreflightCheck()` which requires `glAdcClockEnabled = CyTrue`, a flag only set by the `STARTADC` handler.  Without this patch, `STARTFX3` stalls EP0, the GPIF state machine never starts, and radiod times out with "No rx888 data for 5 seconds". |

When upstream ka9q-radio merges equivalent fixes, bump
`KA9Q_RADIO_SHA` in the Dockerfile and remove the corresponding
patch.
