# ka9q-radio test environment for RX888mk2 firmware validation
#
# Build:   docker build -t ka9q-radio docker/ka9q-radio/
# Run:     docker run --rm -it --privileged -v /dev/bus/usb:/dev/bus/usb \
#            --network host ka9q-radio
#
# The --privileged flag is needed because the FX3 changes USB PID during
# firmware upload (0x00F3 -> 0x00F1), which changes the device path.
# --network host is needed for ka9q-radio's multicast UDP output.

FROM debian:bookworm-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    git \
    ca-certificates \
    libavahi-client-dev \
    libbsd-dev \
    libfftw3-dev \
    libiniparser-dev \
    libncurses-dev \
    libopus-dev \
    libusb-1.0-0-dev \
    libusb-dev \
    uuid-dev \
    libogg-dev \
    libsamplerate-dev \
    portaudio19-dev \
    && rm -rf /var/lib/apt/lists/*

# Clone ka9q-radio — only need radiod + rx888 plugin.
# Pin to a specific commit so audit results are reproducible. Bump
# this SHA when intentionally retesting against newer ka9q-radio.
ARG KA9Q_RADIO_SHA=f78cff9cc8c7fa33ea49aac9176d9263c535a332
WORKDIR /build
RUN git clone https://github.com/ka9q/ka9q-radio.git \
    && git -C ka9q-radio checkout "${KA9Q_RADIO_SHA}"

WORKDIR /build/ka9q-radio

# Apply local compatibility patches against the pinned SHA.
# See docker/ka9q-radio/patches/README.md and docs/ka9q-compat-audit.md.
COPY patches/*.patch /tmp/patches/
RUN for p in /tmp/patches/*.patch; do \
        echo "Applying $p"; \
        git apply --whitespace=nowarn "$p"; \
    done

# Build only what we need: radiod core + rx888.so plugin + control utility
# Disable all other SDR hardware to avoid pulling in unnecessary -dev packages
RUN make -C src -j$(nproc) \
    ENABLE_AIRSPY=0 \
    ENABLE_AIRSPYHF=0 \
    ENABLE_BLADERF=0 \
    ENABLE_FOBOS=0 \
    ENABLE_FUNCUBE=0 \
    ENABLE_HACKRF=0 \
    ENABLE_HYDRASDR=0 \
    ENABLE_RTLSDR=0 \
    ENABLE_RX888=1 \
    ENABLE_SDRPLAY=0 \
    ENABLE_SIG_GEN=1 \
    ARCHOPTS="" \
    radiod rx888.so sig_gen.so control tune monitor

# ---- Runtime image ----
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libavahi-client3 \
    libbsd0 \
    libfftw3-single3 \
    libiniparser1 \
    libncursesw6 \
    libopus0 \
    libusb-1.0-0 \
    libogg0 \
    libsamplerate0 \
    libportaudio2 \
    libfftw3-bin \
    avahi-daemon \
    avahi-utils \
    dbus \
    procps \
    usbutils \
    && rm -rf /var/lib/apt/lists/*

# Copy built binaries
COPY --from=builder /build/ka9q-radio/src/radiod /usr/local/bin/
COPY --from=builder /build/ka9q-radio/src/control /usr/local/bin/
COPY --from=builder /build/ka9q-radio/src/tune /usr/local/bin/
COPY --from=builder /build/ka9q-radio/src/monitor /usr/local/bin/
COPY --from=builder /build/ka9q-radio/src/rx888.so /usr/local/lib/ka9q-radio/
COPY --from=builder /build/ka9q-radio/src/sig_gen.so /usr/local/lib/ka9q-radio/

# Create plugin directory and config directory
RUN mkdir -p /usr/local/lib/ka9q-radio /etc/radio /var/lib/ka9q-radio /usr/local/share/ka9q-radio

# Install ka9q-radio share files (presets.conf, bandplan.txt, help.txt, etc.)
# radiod loads /usr/local/share/ka9q-radio/presets.conf at startup.
COPY --from=builder /build/ka9q-radio/share/ /usr/local/share/ka9q-radio/

# Install minimal rx888 config for HF direct-sampling test
COPY rx888-test.conf /etc/radio/radiod@rx888-test.conf

# Copy entrypoint
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
CMD ["radiod", "/etc/radio/radiod@rx888-test.conf"]
