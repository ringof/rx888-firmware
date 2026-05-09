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

### Tuning and listening (helper script)

`ka9q.sh` (in this directory) wraps the typical operating workflow.
The container publishes demodulated audio as RTP/multicast (per
`radiod`'s standard behavior); the helper gives you start / stop /
console / monitor / listen subcommands without having to remember
the long `docker run` invocation.

Host-side requirements:

```
sudo apt install avahi-utils alsa-utils    # for mDNS resolve + ALSA
```

The `start` subcommand auto-detects `/dev/snd` on the host and
adds `--device /dev/snd --group-add audio` to the `docker run`
invocation when present, so the in-container `monitor` can play
audio through the host's sound card.

Typical session (three terminals):

```
# Terminal A — launch the container in the background:
./ka9q.sh start

# Terminal B — listen with ka9q's own monitor (recommended):
./ka9q.sh monitor                     # default stream: wwv-pcm.local
# or:
./ka9q.sh monitor <other-stream.local>

# Terminal C — drop into the container to operate the radio:
./ka9q.sh console
# inside the container:
control hf.local                      # curses tuner UI (recommended)

# Or one-shot from the container shell (requires the channel's SSRC,
# defined in radiod config — easier to just use `control` above
# which shows you all channels):
tune -r hf.local -s <ssrc> -f 14.074m
```

`control`, `tune`, and `monitor` resolve `*.local` names via mDNS;
the runtime image installs `libnss-mdns` so glibc's `getaddrinfo`
asks the in-container avahi-daemon for `.local` lookups.  If
`Temporary failure in name resolution` appears, the image is from
before this was added — rebuild with `docker build --no-cache`.

To shut down: `./ka9q.sh stop`.

#### `monitor` keybindings (cheat sheet)

`monitor` is a curses program; press `h` inside it for the full
in-app help screen.  Most-used keys:

| Key | Action |
|---|---|
| `↑` `↓` | previous / next session |
| `PgUp` `PgDn` | page through sessions |
| `m` / `u` | mute / unmute current session (`u` also resets) |
| `M` / `U` | mute / unmute all sessions |
| `+` `-` | per-session gain ±1 dB |
| `←` `→` | per-session pan (stereo) |
| `Shift+←` / `Shift+→` | playout buffer ±1 ms |
| `n` / `f` | notch on / off (current session) |
| `s` / `t` | sort sessions by recent / total activity |
| `r` | reset playout queue (current) |
| `R` | reset all sessions |
| `d` | delete current session |
| `v` | toggle verbose display |
| `h` | help screen |
| `q` or `Q` | quit |

#### Why `monitor`, not VLC

ka9q-radio publishes RTP audio with **dynamic payload types** (PT
96+) and no SDP description that VLC will pick up.  VLC opens the
stream and receives bytes, but cannot decode them — you get
silence.  `monitor` is part of ka9q-radio itself and understands
the stream format natively.

The script does provide a `listen` subcommand that runs `cvlc` on
the host as a fallback, but it usually does not produce audio.
It is kept for users who want to use VLC's GUI for diagnostics or
who have already arranged an SDP file for VLC.  Install VLC
separately if you want it: `sudo apt install vlc`.

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

See `docs/ka9q-compat-audit.md` in the parent repository for the
full analysis.  Summary:

- **Missing `STARTADC` before `STARTFX3`** — SDDC requires it for
  the GPIF preflight check; ka9q omits it.  Fixed by container
  patch 03 (`docker/ka9q-radio/patches/03-startadc-before-startfx3.patch`).
  The single ka9q-side change required for streaming to work.
- **`/run/udev` bind-mount required** at `docker run` time — libusb
  inside the container needs host udev events to see the FX3
  re-enumerate after firmware upload.  Container-side workaround,
  no patch.
- `sleep(1)` after firmware upload (`rx888.c:700`) — fragile in
  principle, sufficient on observed hardware with the udev mount.
  Documented in audit §1, no patch.
- `TUNERSTDBY` (0xB8) calls produce harmless STALLs.  Cosmetic
  noise, no functional impact.  Documented in audit §2, no patch.
- GPIO LED bit-mapping differences (cosmetic).
- Missing `libusb_clear_halt()` in ka9q (xHCI fix needed upstream).

## Requirements

- Docker with `--privileged` support
- RX888mk2 connected via USB 3.0
- Host kernel with xHCI/USB 3.0 support (any modern Linux)
