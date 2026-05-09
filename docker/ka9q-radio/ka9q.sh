#!/bin/bash
#
# ka9q.sh — helper for the ka9q-radio Docker image.
#
# Subcommands:
#   start                  Launch the container detached.
#   console                Drop into a bash shell inside the running container.
#   monitor [stream-name]  Run ka9q's `monitor` inside the container with
#                          host-side ALSA playback.  Default: wwv-pcm.local
#   listen  [stream-name]  Fallback: play with cvlc on the host (often fails
#                          because ka9q uses dynamic RTP payload types VLC
#                          can't decode without an SDP file).  Default:
#                          wwv-pcm.local
#   stop                   Stop the container.
#   help                   Show this usage.
#
# Typical workflow:
#   ./ka9q.sh start                    # terminal A
#   ./ka9q.sh monitor                  # terminal B  (audio out via ka9q monitor)
#   ./ka9q.sh console                  # terminal C
#     # inside the container:
#     control hf.local                 # curses tuner UI
#

set -euo pipefail

CONTAINER_NAME="ka9q-radio"
IMAGE_NAME="ka9q-radio"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# This script lives in docker/ka9q-radio/; project root is two levels up.
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

usage() {
    sed -n '3,/^$/s/^# \?//p' "$0"
}

container_running() {
    docker ps --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"
}

cmd_start() {
    if container_running; then
        echo "Container '$CONTAINER_NAME' is already running."
        return 0
    fi
    if [ ! -f "$PROJECT_ROOT/SDDC_FX3/SDDC_FX3.img" ]; then
        echo "WARNING: $PROJECT_ROOT/SDDC_FX3/SDDC_FX3.img not found."
        echo "         Firmware upload will fail unless the device is already loaded."
    fi
    mkdir -p "$PROJECT_ROOT/wisdom"
    # /dev/snd + audio group give the in-container `monitor` access to host
    # ALSA so audio actually plays.  Harmless on hosts without sound — the
    # device simply isn't bound and `monitor` falls back to silent operation.
    local snd_args=()
    if [ -e /dev/snd ]; then
        snd_args+=(--device /dev/snd --group-add audio)
    fi
    docker run --rm -d --name "$CONTAINER_NAME" --privileged \
        -v /dev/bus/usb:/dev/bus/usb \
        -v /run/udev:/run/udev:ro \
        -v "$PROJECT_ROOT/SDDC_FX3:/firmware" \
        -v "$PROJECT_ROOT/wisdom:/var/lib/ka9q-radio" \
        "${snd_args[@]}" \
        --network host \
        -e FFTW_RIGOR="${FFTW_RIGOR:-measure}" \
        "$IMAGE_NAME" >/dev/null
    echo "Container '$CONTAINER_NAME' started."
    echo "Follow logs:  docker logs -f $CONTAINER_NAME"
}

cmd_console() {
    if ! container_running; then
        echo "Container '$CONTAINER_NAME' is not running.  Start it with: $0 start" >&2
        exit 1
    fi
    exec docker exec -it "$CONTAINER_NAME" bash
}

cmd_monitor() {
    local stream="${1:-wwv-pcm.local}"
    if ! container_running; then
        echo "Container '$CONTAINER_NAME' is not running.  Start it with: $0 start" >&2
        exit 1
    fi
    echo "Running ka9q's monitor inside the container.  Ctrl-C to stop."
    echo "If you hear nothing, check that the container was started with"
    echo "/dev/snd available on the host (re-run '$0 start' on a host with sound)."
    exec docker exec -it "$CONTAINER_NAME" monitor "$stream"
}

cmd_listen() {
    local stream="${1:-wwv-pcm.local}"
    if ! command -v avahi-resolve >/dev/null 2>&1; then
        echo "avahi-resolve not found.  Install avahi-utils on the host" >&2
        echo "  (e.g. sudo apt install avahi-utils)" >&2
        exit 1
    fi
    if ! command -v cvlc >/dev/null 2>&1; then
        echo "cvlc not found.  Install VLC on the host" >&2
        echo "  (e.g. sudo apt install vlc)" >&2
        exit 1
    fi
    local addr
    addr=$(avahi-resolve -n "$stream" 2>/dev/null | awk '{print $2}')
    if [ -z "$addr" ]; then
        echo "Could not resolve '$stream' via avahi-daemon." >&2
        echo "  - Is the container running?  ($0 start)" >&2
        echo "  - Is avahi-daemon running on the host?" >&2
        echo "  - Is the multicast stream actually being published?" >&2
        echo "    (check 'docker logs $CONTAINER_NAME' for 'Established under name')" >&2
        exit 1
    fi
    echo "Resolved $stream -> $addr"
    echo "Playing with cvlc (--network-caching=200).  Ctrl-C to stop."
    echo "If audio doesn't come through, try the GUI: vlc rtp://@$addr:5004"
    exec cvlc --network-caching=200 "rtp://@$addr:5004"
}

cmd_stop() {
    if ! container_running; then
        echo "Container '$CONTAINER_NAME' is not running."
        return 0
    fi
    docker stop "$CONTAINER_NAME" >/dev/null
    echo "Container '$CONTAINER_NAME' stopped."
}

case "${1:-help}" in
    start)          shift; cmd_start "$@" ;;
    console)        shift; cmd_console "$@" ;;
    monitor)        shift; cmd_monitor "$@" ;;
    listen)         shift; cmd_listen "$@" ;;
    stop)           shift; cmd_stop "$@" ;;
    help|-h|--help) usage ;;
    *) usage; exit 2 ;;
esac
