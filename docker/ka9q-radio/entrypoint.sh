#!/bin/bash
set -e

# Start dbus (required by avahi)
if [ ! -d /run/dbus ]; then
    mkdir -p /run/dbus
fi
dbus-daemon --system --nofork &
sleep 0.5

# Start avahi (required by ka9q-radio for multicast service discovery)
avahi-daemon --no-drop-root --daemonize 2>/dev/null || true

# Check for firmware image
if [ ! -f /firmware/SDDC_FX3.img ]; then
    echo "WARNING: /firmware/SDDC_FX3.img not found."
    echo "Mount it with: -v /path/to/SDDC_FX3:/firmware"
    echo "If the device is already loaded, this is fine."
fi

# Check for USB device
if lsusb -d 04b4:00f1 > /dev/null 2>&1; then
    echo "RX888 found (loaded, PID 0x00F1)"
elif lsusb -d 04b4:00f3 > /dev/null 2>&1; then
    echo "RX888 found (bootloader, PID 0x00F3) — radiod will upload firmware"
else
    echo "WARNING: No RX888 detected on USB bus"
    echo "Make sure to run with: --privileged -v /dev/bus/usb:/dev/bus/usb"
fi

# Pre-generate FFTW wisdom on first run.  Wisdom is CPU-specific
# (encodes which SIMD instruction set to use), so it cannot be baked
# into a portable image — it must be generated on the host CPU that
# will run the container.  Persist /var/lib/ka9q-radio across runs
# (e.g. -v $(pwd)/wisdom:/var/lib/ka9q-radio) to skip this on every
# start.  Sizes match the default rx888-test.conf (64.8 MHz, 20 ms
# blocks → rof1620000; 12 kHz output → cob240).
#
# Planning rigor is configurable via FFTW_RIGOR (estimate|measure|
# patient|exhaustive); default is "measure" (minutes, near-optimal
# runtime).  "patient" can take many hours for the 1.62M-point FFT;
# "estimate" is instant but yields slower runtime FFTs.
WISDOM_FILE="/var/lib/ka9q-radio/wisdom"
FFTW_RIGOR="${FFTW_RIGOR:-measure}"
case "$FFTW_RIGOR" in
    estimate)   WISDOM_FLAG="-e" ;;
    measure)    WISDOM_FLAG="-m" ;;
    patient)    WISDOM_FLAG="-p" ;;
    exhaustive) WISDOM_FLAG="-x" ;;
    *)
        echo "WARNING: unknown FFTW_RIGOR='$FFTW_RIGOR'; using 'measure'."
        FFTW_RIGOR="measure"
        WISDOM_FLAG="-m"
        ;;
esac
if [ ! -s "$WISDOM_FILE" ]; then
    echo "Generating FFTW wisdom (rigor=$FFTW_RIGOR) for this host..."
    echo "  (override with -e FFTW_RIGOR=estimate|measure|patient|exhaustive)"
    mkdir -p "$(dirname "$WISDOM_FILE")" /etc/fftw
    if fftwf-wisdom -v "$WISDOM_FLAG" -T "$(nproc)" -o "$WISDOM_FILE" rof1620000 cob240; then
        cp -f "$WISDOM_FILE" /etc/fftw/wisdomf 2>/dev/null || true
        echo "Wisdom saved to $WISDOM_FILE."
    else
        echo "WARNING: wisdom generation failed; radiod will fall back to FFTW_ESTIMATE."
    fi
fi

exec "$@"
