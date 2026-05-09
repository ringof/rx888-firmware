# ka9q-radio patches applied in this container

These patches are applied during the Docker build (see Dockerfile)
against the pinned `KA9Q_RADIO_SHA`.  They are derived from the audit
in `docs/ka9q-compat-audit.md`.

| File | What | Why |
|------|------|-----|
| `01-poll-reenumeration.patch` | Replace post-upload `sleep(1)` with a 200 ms × 50 polling loop. | Author's own comment admits the 1 s value is a guess; FX3 boot + udev settle on Linux can exceed 1 s, more so under Docker, causing `rx888_usb_init()` to fail with "Error or device could not be found". |
| `02-drop-tunerstdby-hf.patch` | Remove `TUNERSTDBY` (0xB8) calls in `rx888_set_hf_mode` and `rx888_start_rx`. | SDDC firmware removed all R82xx tuner commands; 0xB8 STALLs.  HF receive does not need this command. |

When upstream ka9q-radio merges equivalent fixes, bump
`KA9Q_RADIO_SHA` in the Dockerfile and remove the corresponding
patch.
