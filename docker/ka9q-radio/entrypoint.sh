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

exec "$@"
