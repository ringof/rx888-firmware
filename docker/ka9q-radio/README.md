# ka9q-radio Docker Test Environment

Docker container for testing SDDC_FX3 firmware compatibility with
[ka9q-radio](https://github.com/ka9q/ka9q-radio)'s `radiod` and the
`rx888.so` driver plugin.

## Build

```
docker build -t ka9q-radio docker/ka9q-radio/
```

## Find your USB device

`/dev/bus/usb` is the Linux usbfs tree. To confirm the RX888mk2 is
visible to the host before mounting it into the container:

```
# List all USB devices; look for Cypress FX3
lsusb

# Filter to Cypress IDs:
#   04b4:00f3  -> FX3 boot loader (no firmware loaded)
#   04b4:00f1  -> SDDC firmware running
lsusb -d 04b4:
```

Example output:

```
Bus 002 Device 014: ID 04b4:00f3 Cypress Semiconductor Corp. CYUSB3013/CYUSB3014
```

The corresponding device node is `/dev/bus/usb/<bus>/<device>` —
`/dev/bus/usb/002/014` in the example. Mounting all of `/dev/bus/usb`
into the container (as the `docker run` examples below do) lets the
container see the device even when its bus/device path changes after
firmware upload (PID flips `0x00f3` → `0x00f1`, which usually
re-enumerates).

If `lsusb` shows nothing for `04b4:`, the host isn't seeing the
device — fix that first (cable, USB 3.0 port, power) before bothering
with Docker. macOS and Windows do not expose `/dev/bus/usb`; this
container is Linux-only.

## Run

### With firmware already loaded on the device

```
docker run --rm -it --privileged \
  -v /dev/bus/usb:/dev/bus/usb \
  -v /run/udev:/run/udev:ro \
  --network host \
  ka9q-radio
```

### With firmware upload (radiod handles it)

```
docker run --rm -it --privileged \
  -v /dev/bus/usb:/dev/bus/usb \
  -v /run/udev:/run/udev:ro \
  -v $(pwd)/SDDC_FX3:/firmware \
  -v $(pwd)/wisdom:/var/lib/ka9q-radio \
  --network host \
  ka9q-radio
```

The `/run/udev` bind mount is **required** when radiod uploads the
firmware: after the FX3 re-enumerates from `04b4:00f3` (DFU) to
`04b4:00f1` (loaded), libusb inside the container needs to see the
new device.  libusb's hotplug listener subscribes to systemd-udevd
events; udevd does not run inside the container, so without this
mount libusb returns a stale device list forever and radiod exits
with "Error or device could not be found".  The mount is harmless
(read-only) for the firmware-already-loaded case, so it is shown in
both examples.

The `wisdom` bind mount persists FFTW wisdom across runs.  On first
run the entrypoint generates wisdom for the host CPU (FFTW wisdom is
CPU-specific — it cannot be baked into a portable image).  Subsequent
runs reuse the saved file.

Planning rigor is controlled by the `FFTW_RIGOR` environment variable:

| Value        | First-run time           | Runtime FFT performance |
|--------------|--------------------------|-------------------------|
| `estimate`   | instant                  | slowest                 |
| `measure`    | minutes (default)        | near-optimal            |
| `patient`    | hours (1.62M-point FFT)  | optimal                 |
| `exhaustive` | many hours to days       | marginally > patient    |

Pass it with `-e FFTW_RIGOR=<value>` on `docker run`, e.g.
`-e FFTW_RIGOR=estimate` for a quick firmware-validation session, or
`-e FFTW_RIGOR=patient` if you intend to operate the radio long-term
and want the most efficient FFT plans.

### Interactive shell (for debugging)

```
docker run --rm -it --privileged \
  -v /dev/bus/usb:/dev/bus/usb \
  -v /run/udev:/run/udev:ro \
  -v $(pwd)/SDDC_FX3:/firmware \
  --network host \
  ka9q-radio bash
```

Then inside the container:

```
# Check USB device
lsusb -d 04b4:

# Run radiod manually
radiod /etc/radio/radiod@rx888-test.conf

# In another terminal (docker exec):
# Tune to a frequency
tune hf.local 14.074m

# Monitor status
monitor hf.local
```

## What this tests

1. **Firmware upload** — ka9q-radio uses its own `ezusb.c` loader
   (same protocol as `rx888_stream -f`)
2. **Si5351 clock programming** — ka9q programs the Si5351 directly
   via `I2CWFX3`, then sends `STARTADC` (via container patch 03) so
   the SDDC firmware's `glAdcClockEnabled` flag is set and
   `STARTFX3`'s preflight check passes.  Without patch 03, STARTFX3
   stalls and the GPIF state machine never starts.
3. **GPIF streaming** — `STARTFX3` + async bulk transfers at 64.8 MSPS
4. **GPIO control** — `GPIOFX3` for dither, randomizer, HF/VHF select
5. **Attenuator/VGA** — `SETARGFX3` with DAT31_ATT and AD8340_VGA
6. **Stop/restart** — `STOPFX3` + `STARTFX3` cycling

## Known compatibility notes

See the analysis in the parent repository for details on:

- `TUNERSTDBY` (0xB8) calls that produce harmless STALLs (ka9q-side fix)
- Missing `STARTADC` before `STARTFX3` — SDDC requires it for GPIF
  preflight; ka9q omits it (ka9q-side fix in patch 03)
- GPIO LED bit mapping differences (cosmetic)
- Missing `libusb_clear_halt()` in ka9q (xHCI fix needed upstream)

## Requirements

- Docker with `--privileged` support
- RX888mk2 connected via USB 3.0
- Host kernel with xHCI/USB 3.0 support (any modern Linux)
