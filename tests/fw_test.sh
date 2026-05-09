#!/usr/bin/env bash
#
# fw_test.sh — Automated firmware test for SDDC_FX3 on RX888mk2 hardware.
#
# Requires:
#   - fx3_cmd      (built from tests/Makefile)
#   - rx888_stream (from https://github.com/ringof/rx888_tools)
#   - RX888mk2 connected via USB
#
# rx888_stream handles firmware upload: if the device is in bootloader mode
# (PID 0x00F3), it loads the .img and waits for re-enumeration before
# proceeding.  This script uses a short rx888_stream run to upload firmware
# and confirm basic streaming, then uses fx3_cmd for individual command tests.
#
# Output: TAP (Test Anything Protocol)
#
# Usage:
#   ./fw_test.sh --firmware path/to/SDDC_FX3.img [options]
#
# Options:
#   --firmware PATH        Firmware .img to upload (required)
#   --stream-seconds N     Streaming capture duration (default: 5)
#   --rx888-stream PATH    Path to rx888_stream binary (default: search PATH)
#   --skip-stream          Skip streaming tests
#   --sample-rate HZ       ADC sample rate (default: 64000000)
#

set -euo pipefail

# ---- Configuration ----

FIRMWARE=""
STREAM_SECONDS=5
RX888_STREAM=""
SKIP_STREAM=0
SAMPLE_RATE=32000000
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FX3_CMD="$SCRIPT_DIR/fx3_cmd"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --firmware)        FIRMWARE="$2"; shift 2 ;;
        --stream-seconds)  STREAM_SECONDS="$2"; shift 2 ;;
        --rx888-stream)    RX888_STREAM="$2"; shift 2 ;;
        --skip-stream)     SKIP_STREAM=1; shift ;;
        --sample-rate)     SAMPLE_RATE="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,/^$/s/^# \?//p' "$0"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

# ---- Locate rx888_stream ----

find_rx888_stream() {
    # Explicit path from --rx888-stream
    if [[ -n "$RX888_STREAM" ]]; then
        if [[ -x "$RX888_STREAM" ]]; then
            return 0
        fi
        echo "Bail out! --rx888-stream '$RX888_STREAM' not found or not executable"
        exit 1
    fi

    # Check submodule build (symlink created by tests/Makefile)
    local submod="$SCRIPT_DIR/rx888_stream"
    if [[ -x "$submod" ]]; then
        RX888_STREAM="$submod"
        return 0
    fi

    # Check submodule source directory directly
    local submod_bin="$SCRIPT_DIR/rx888_tools/rx888_stream"
    if [[ -x "$submod_bin" ]]; then
        RX888_STREAM="$submod_bin"
        return 0
    fi

    # Check PATH
    if command -v rx888_stream &>/dev/null; then
        RX888_STREAM="$(command -v rx888_stream)"
        return 0
    fi

    return 1
}

# ---- Helpers ----

TEST_NUM=0
PASS_COUNT=0
FAIL_COUNT=0
TMPDIR=""

cleanup() {
    if [[ -n "$TMPDIR" && -d "$TMPDIR" ]]; then
        rm -rf "$TMPDIR"
    fi
}
trap cleanup EXIT

tap_ok() {
    TEST_NUM=$((TEST_NUM + 1))
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok $TEST_NUM - $1"
}

tap_fail() {
    TEST_NUM=$((TEST_NUM + 1))
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "not ok $TEST_NUM - $1"
    if [[ -n "${2:-}" ]]; then
        echo "#   $2"
    fi
}

tap_skip() {
    TEST_NUM=$((TEST_NUM + 1))
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok $TEST_NUM - $1 # SKIP $2"
}

# Run fx3_cmd, capture output.  Returns 0 if output starts with "PASS".
run_cmd() {
    local output last_line
    output=$("$FX3_CMD" "$@" 2>&1) || true
    echo "$output"
    last_line="${output##*$'\n'}"
    [[ "$last_line" == PASS* ]]
}

# Ensure device is in a known-clean state before streaming tests.
# Mirrors the soak runner's inter-scenario cleanup (soak_main lines 3794-3820):
#   1. Unconditional STOPFX3 (may already be stopped — that's fine)
#   2. 100 ms quiesce for GPIF/DMA to settle
#   3. Health check via TESTFX3; retry once after 2 s on failure
device_quiesce() {
    "$FX3_CMD" stop >/dev/null 2>&1 || true
    "$FX3_CMD" gpio 0x0800 >/dev/null 2>&1 || true   # LED_BLUE — clear SHDWN
    sleep 0.1
    if "$FX3_CMD" test >/dev/null 2>&1; then
        return 0
    fi
    # Device unhealthy — give watchdog time to finish, retry once
    sleep 2
    if "$FX3_CMD" test >/dev/null 2>&1; then
        return 0
    fi
    echo "# WARNING: device_quiesce: health check failed after retry"
    return 1
}

# ---- Preflight checks ----

if [[ ! -x "$FX3_CMD" ]]; then
    echo "Bail out! fx3_cmd not found at $FX3_CMD — run 'make' in tests/ first"
    exit 1
fi

if ! find_rx888_stream; then
    echo "Bail out! rx888_stream not found. Build it:"
    echo "#   git submodule update --init && cd tests && make"
    echo "#   or pass --rx888-stream /path/to/rx888_stream"
    exit 1
fi

if [[ -z "$FIRMWARE" ]]; then
    # Default: look for the build output
    DEFAULT_FW="$SCRIPT_DIR/../SDDC_FX3/SDDC_FX3.img"
    if [[ -f "$DEFAULT_FW" ]]; then
        FIRMWARE="$(cd "$(dirname "$DEFAULT_FW")" && pwd)/$(basename "$DEFAULT_FW")"
        echo "# Using firmware: $FIRMWARE"
    else
        echo "Bail out! No firmware specified. Use --firmware path/to/SDDC_FX3.img"
        echo "#   or build it first: cd SDDC_FX3 && make"
        exit 1
    fi
fi

if [[ ! -f "$FIRMWARE" ]]; then
    echo "Bail out! Firmware file not found: $FIRMWARE"
    exit 1
fi

TMPDIR=$(mktemp -d /tmp/fw_test.XXXXXX)

echo "# rx888_stream: $RX888_STREAM"
echo "# firmware:     $FIRMWARE"
echo "# sample rate:  $SAMPLE_RATE Hz"

# ---- Test Plan ----

# Tests: 1 upload + 1 probe + 1 gpio + 1 adc + 2 att + 2 vga + 1 stop
#      + 3 stale commands + 1 i2c_nack + 1 adc_off
#      + 1 ep0_overflow + 5 debug/OOB tests + 1 stack check
#      + 2 GETSTATS (readout + I2C) + 1 GETSTATS PLL
#      + 5 new coverage-gap tests (vendor_rqt_wrap, stale_vendor_codes,
#        setarg_gap_index, gpio_extremes, i2c_write_bad_addr)
#      + 1 hw_smoke (ADC alive after GPIO extremes)
#      + 4 GPIF wedge/recovery tests
#      + 1 PIB overflow + 1 GETSTATS PIB
#      + optional streaming (3 checks)
# NOTE: pib_overflow wedges DMA/GPIF — run all clean-state tests first
PLANNED=36
if [[ $SKIP_STREAM -eq 0 ]]; then
    PLANNED=$((PLANNED + 3))
fi
echo "1..$PLANNED"

# ==================================================================
# 1. Firmware upload via rx888_stream
# ==================================================================
# Run rx888_stream briefly with the firmware file.  It will:
#   - Find the device in bootloader mode (PID 0x00F3)
#   - Upload the .img file
#   - Wait for re-enumeration at app PID (0x00F1)
#   - Start streaming (which we immediately kill)
#
# If the device already has firmware loaded, rx888_stream opens it
# directly and this still exercises the basic startup path.

UPLOAD_LOG="$TMPDIR/upload.log"

timeout 15 "$RX888_STREAM" -f "$FIRMWARE" -s "$SAMPLE_RATE" \
    > /dev/null 2>"$UPLOAD_LOG" &
UPLOAD_PID=$!

# Give it time to upload firmware and start streaming
sleep 4
kill "$UPLOAD_PID" 2>/dev/null || true
wait "$UPLOAD_PID" 2>/dev/null || true

# Allow device to settle after rx888_stream exits
sleep 2

# Check if the device is now at the app PID
if lsusb -d 04b4:00f1 &>/dev/null; then
    tap_ok "firmware upload: device running at PID 0x00F1"
else
    tap_fail "firmware upload: device not found at PID 0x00F1 after upload" \
             "$(cat "$UPLOAD_LOG" 2>/dev/null || echo '(no log)')"
    echo "Bail out! Cannot continue without firmware loaded"
    exit 1
fi

# ==================================================================
# 2. Device probe
# ==================================================================

output=$(run_cmd test) && {
    hwconfig=$(echo "$output" | grep -oP 'hwconfig=0x\K[0-9A-Fa-f]+' || echo "??")
    if [[ "$hwconfig" == "04" ]]; then
        tap_ok "device probe: RX888r2 detected (hwconfig=0x04)"
    else
        tap_fail "device probe: unexpected hwconfig=0x$hwconfig (expected 0x04)" "$output"
    fi
} || {
    tap_fail "device probe: fx3_cmd test failed" "$output"
}

# ==================================================================
# 3. GPIO test
# ==================================================================

# Set LED on, dither off, randomizer off
output=$(run_cmd gpio 0x0800) && {
    tap_ok "gpio: set LED_BLUE"
} || {
    tap_fail "gpio: write failed" "$output"
}

# ==================================================================
# 4. ADC clock
# ==================================================================

output=$(run_cmd adc "$SAMPLE_RATE") && {
    tap_ok "adc: set clock to $SAMPLE_RATE Hz"
} || {
    tap_fail "adc: set clock failed" "$output"
}

# ==================================================================
# 5. Attenuator spot-check (min and max)
# ==================================================================

output=$(run_cmd att 0) && {
    tap_ok "att: set 0 (minimum)"
} || {
    tap_fail "att: set 0 failed" "$output"
}

output=$(run_cmd att 63) && {
    tap_ok "att: set 63 (maximum)"
} || {
    tap_fail "att: set 63 failed" "$output"
}

# ==================================================================
# 6. VGA spot-check (min and max)
# ==================================================================

output=$(run_cmd vga 0) && {
    tap_ok "vga: set 0 (minimum)"
} || {
    tap_fail "vga: set 0 failed" "$output"
}

output=$(run_cmd vga 255) && {
    tap_ok "vga: set 255 (maximum)"
} || {
    tap_fail "vga: set 255 failed" "$output"
}

# ==================================================================
# 7. Stop (ensure clean state)
# ==================================================================

output=$(run_cmd stop) && {
    tap_ok "stop: clean state"
} || {
    tap_fail "stop: failed" "$output"
}

# ==================================================================
# 8. Stale tuner command tests (post-R82xx removal)
# ==================================================================
# These should STALL.  The "raw" subcommand treats STALL as PASS.
# After each, verify the device is still responsive.

output=$("$FX3_CMD" raw 0xB4 2>&1) || true
if [[ "$output" == *STALL* ]]; then
    tap_ok "stale TUNERINIT (0xB4): got STALL as expected"
else
    tap_fail "stale TUNERINIT (0xB4): command accepted (not removed?)" "$output"
fi

output=$("$FX3_CMD" raw 0xB5 2>&1) || true
if [[ "$output" == *STALL* ]]; then
    tap_ok "stale TUNERTUNE (0xB5): got STALL as expected"
else
    tap_fail "stale TUNERTUNE (0xB5): command accepted (not removed?)" "$output"
fi

output=$("$FX3_CMD" raw 0xB8 2>&1) || true
if [[ "$output" == *STALL* ]]; then
    tap_ok "stale TUNERSTDBY (0xB8): got STALL as expected"
else
    tap_fail "stale TUNERSTDBY (0xB8): command accepted (not removed?)" "$output"
fi

# ==================================================================
# 8a. I2C NACK on non-existent address
# ==================================================================
# Read from I2C address 0xC2 (no device at this address) to confirm
# the firmware propagates I2C NACK errors as EP0 STALLs.

output=$("$FX3_CMD" i2cr 0xC2 0 1 2>&1) || true
if [[ "$output" == *FAIL* ]]; then
    tap_ok "i2c_nack: read from absent address 0xC2 correctly failed"
else
    tap_fail "i2c_nack: read from absent address 0xC2 unexpectedly succeeded" "$output"
fi

# ==================================================================
# 8b. ADC clock-off (freq=0) — exercises I2cTransferW1 error path
# ==================================================================
# Send STARTADC with freq=0 to exercise the Si5351 clock-off path
# through the error-checked I2cTransferW1 code.  On a working device
# this should succeed (the Si5351 accepts the write to disable CLK0).

output=$(run_cmd adc 0) && {
    tap_ok "adc_off: clock-off via STARTADC freq=0"
} || {
    tap_fail "adc_off: clock-off failed (I2cTransferW1 error?)" "$output"
}

# ==================================================================
# 9. EP0 wLength overflow check (issue #6)
# ==================================================================
# Send a vendor request with wLength > 64 — firmware must STALL.

output=$(run_cmd ep0_overflow) && {
    tap_ok "ep0_overflow: STALL on oversized wLength"
} || {
    tap_fail "ep0_overflow: accepted oversized wLength (buffer overflow risk)" "$output"
}

# ==================================================================
# 10. TraceSerial OOB bRequest (issue #21)
# ==================================================================
# Send vendor request 0xCC (outside FX3CommandName[0xAA..0xBA]).
# Before the fix, this would index FX3CommandName[0xCC-0xAA] = [34],
# reading past the 17-element array.

output=$(run_cmd oob_brequest) && {
    tap_ok "oob_brequest: survived bRequest=0xCC (issue #21)"
} || {
    tap_fail "oob_brequest: device crashed on OOB bRequest" "$output"
}

# ==================================================================
# 11. TraceSerial OOB SETARGFX3 wIndex (issue #20)
# ==================================================================
# Send SETARGFX3 with wIndex=0xFFFF (outside SETARGFX3List[0..13]).

output=$(run_cmd oob_setarg) && {
    tap_ok "oob_setarg: survived SETARGFX3 wIndex=0xFFFF (issue #20)"
} || {
    tap_fail "oob_setarg: device crashed on OOB wIndex" "$output"
}

# ==================================================================
# 12. Console input buffer fill (issue #13)
# ==================================================================
# Send 35 chars (exceeds 32-byte ConsoleInBuffer).  Before the fix,
# the off-by-one allowed writing past the buffer.

output=$(run_cmd console_fill) && {
    tap_ok "console_fill: survived 35-char input (issue #13)"
} || {
    tap_fail "console_fill: device crashed on buffer fill" "$output"
}

# ==================================================================
# 13. Debug buffer race stress test (issue #8)
# ==================================================================
# Rapidly interleave SETARGFX3 (generates debug output via TraceSerial)
# with READINFODEBUG polls (reads debug buffer).

output=$(run_cmd debug_race) && {
    tap_ok "debug_race: survived 50 rapid poll cycles (issue #8)"
} || {
    tap_fail "debug_race: device crashed or corrupted under race" "$output"
}

# ==================================================================
# 14. Debug console over USB (issue #26)
# ==================================================================
# Send "?" + CR, collect debug output, verify we get help text back.

output=$(run_cmd debug_poll) && {
    tap_ok "debug_poll: got debug output over USB (issue #26)"
} || {
    tap_fail "debug_poll: no debug output received" "$output"
}

# ==================================================================
# 15. Stack watermark check (issue #12)
# ==================================================================
# Query the "stack" debug command and verify adequate headroom
# after the thread has run through initialization.

output=$(run_cmd stack_check) && {
    tap_ok "stack_check: adequate stack headroom at 2KB (issue #12)"
} || {
    tap_fail "stack_check: insufficient stack headroom" "$output"
}

# ==================================================================
# 16. GETSTATS readout
# ==================================================================

output=$(run_cmd stats) && {
    tap_ok "stats: GETSTATS readout"
} || {
    tap_fail "stats: GETSTATS readout failed" "$output"
}

# ==================================================================
# 17. GETSTATS I2C failure counter
# ==================================================================

output=$(run_cmd stats_i2c) && {
    tap_ok "stats_i2c: I2C failure counter incremented on NACK"
} || {
    tap_fail "stats_i2c: I2C failure counter did not increment" "$output"
}

# ==================================================================
# 18. GETSTATS PLL lock status
# ==================================================================
# After test 4 set the ADC clock, the Si5351 PLL should be locked.
# Verify SYS_INIT is clear and PLL A is locked via GETSTATS byte [19].

output=$(run_cmd stats_pll) && {
    tap_ok "stats_pll: Si5351 PLL locked, SYS_INIT clear"
} || {
    tap_fail "stats_pll: PLL not locked or SYS_INIT set" "$output"
}

# ==================================================================
# 19. Vendor request counter wraparound
# ==================================================================

output=$(run_cmd vendor_rqt_wrap) && {
    tap_ok "vendor_rqt_wrap: counter wraps at 256"
} || {
    tap_fail "vendor_rqt_wrap: counter did not wrap" "$output"
}

# ==================================================================
# 20. Stale vendor codes (dead-zone bRequest values)
# ==================================================================

output=$(run_cmd stale_vendor_codes) && {
    tap_ok "stale_vendor_codes: dead-zone bRequest values STALL"
} || {
    tap_fail "stale_vendor_codes: dead-zone codes not handled" "$output"
}

# ==================================================================
# 21. SETARGFX3 near-miss wIndex values
# ==================================================================

output=$(run_cmd setarg_gap_index) && {
    tap_ok "setarg_gap_index: near-miss wIndex values survived"
} || {
    tap_fail "setarg_gap_index: device crashed on gap wIndex" "$output"
}

# ==================================================================
# 22. GPIO extreme patterns
# ==================================================================

output=$(run_cmd gpio_extremes) && {
    tap_ok "gpio_extremes: all-zeros/all-ones GPIO survived"
} || {
    tap_fail "gpio_extremes: device crashed on extreme pattern" "$output"
}

# ==================================================================
# 23. I2C write NACK counter
# ==================================================================

output=$(run_cmd i2c_write_bad_addr) && {
    tap_ok "i2c_write_bad_addr: write NACK path survived"
} || {
    tap_fail "i2c_write_bad_addr: device crashed on write NACK" "$output"
}

# ==================================================================
# 24. Hardware smoke test — ADC alive after GPIO extremes
# ==================================================================
# Verifies the device can still stream data after gpio_extremes.
# Catches the case where SHDWN was left set (ADC asleep).

device_quiesce

output=$(run_cmd hw_smoke) && {
    tap_ok "hw_smoke: ADC alive — data flows after GPIO extremes"
} || {
    tap_fail "hw_smoke: no data — ADC may be in shutdown" "$output"
}

# ==================================================================
# 25. GPIF stop state verification
# ==================================================================
# After STOPFX3, verify the GPIF SM is actually stopped (state 0 or 1).
# Before the fix: SM stays running or stuck in BUSY/WAIT.

device_quiesce

output=$(run_cmd stop_gpif_state) && {
    tap_ok "stop_gpif_state: GPIF SM properly stopped after STOPFX3"
} || {
    tap_fail "stop_gpif_state: GPIF SM still running after STOPFX3" "$output"
}

# ==================================================================
# 26. Stop/start cycle test
# ==================================================================
# Cycle STOP+START 5 times, verifying data flows each cycle.
# Before the fix: wedges on 2nd or 3rd cycle.

device_quiesce

output=$(run_cmd stop_start_cycle) && {
    tap_ok "stop_start_cycle: 5 stop/start cycles completed"
} || {
    tap_fail "stop_start_cycle: stream failed during cycling" "$output"
}

# ==================================================================
# 27. PLL pre-flight check
# ==================================================================
# Verify STARTFX3 is rejected when ADC clock is off.
# Before the fix: START succeeds and GPIF reads garbage.

device_quiesce

output=$(run_cmd pll_preflight) && {
    tap_ok "pll_preflight: STARTFX3 rejected without ADC clock"
} || {
    tap_fail "pll_preflight: STARTFX3 accepted without ADC clock" "$output"
}

# ==================================================================
# 28. Wedge recovery test
# ==================================================================
# Provoke DMA backpressure (start streaming, don't read EP1),
# then STOP+START and verify recovery.
# Before the fix: device is wedged, no recovery.

device_quiesce

output=$(run_cmd wedge_recovery) && {
    tap_ok "wedge_recovery: recovered from DMA backpressure wedge"
} || {
    tap_fail "wedge_recovery: device wedged after backpressure" "$output"
}

# ==================================================================
# 29. GPIF soft-stop verification (requires new waveform)
# ==================================================================
# Cycle start/stop 10 times, verify SM lands in IDLE (state 1) each time.
# State 1 = soft-stop via !FW_TRG transitions.  State 255 = force-stop
# fallback (old waveform).  REQUIRES updated SDDC_GPIF.h.

device_quiesce

output=$(run_cmd gpif_soft_stop) && {
    tap_ok "gpif_soft_stop: SM consistently reaches IDLE via soft-stop"
} || {
    tap_fail "gpif_soft_stop: SM did not reach IDLE — new waveform needed" "$output"
}

# ==================================================================
# 30. Stop under DMA backpressure (requires new waveform)
# ==================================================================
# Start at 64 MS/s without reading EP1, then STOP.  Before the fix:
# SM is stuck in WAIT, force-stop leaves debris.  After: !FW_TRG
# exits WAIT cleanly.  REQUIRES updated SDDC_GPIF.h.

device_quiesce

output=$(run_cmd stop_under_backpressure) && {
    tap_ok "stop_under_backpressure: clean stop from WAIT state + recovery"
} || {
    tap_fail "stop_under_backpressure: stop or recovery failed" "$output"
}

# ==================================================================
# 31. PIB error detection (issue #10)
# ==================================================================
# Start streaming at 64 MS/s without reading EP1 — GPIF overflows.
# Verify the debug console reports "PIB error".
# NOTE: This test wedges the DMA/GPIF path.  All tests that need
# clean device state must run BEFORE this point.

device_quiesce

output=$(run_cmd pib_overflow) && {
    tap_ok "pib_overflow: PIB error detected in debug output (issue #10)"
} || {
    tap_fail "pib_overflow: no PIB error reported" "$output"
}

# ==================================================================
# 32. GETSTATS PIB error counter
# ==================================================================
# Runs after pib_overflow; counter should already be > 0.

output=$(run_cmd stats_pib) && {
    tap_ok "stats_pib: PIB error counter incremented on overflow"
} || {
    tap_fail "stats_pib: PIB error counter did not increment" "$output"
}

# ==================================================================
# 33. Streaming test via rx888_stream
# ==================================================================

if [[ $SKIP_STREAM -eq 1 ]]; then
    tap_skip "stream: data capture" "streaming tests skipped"
    tap_skip "stream: byte count" "streaming tests skipped"
    tap_skip "stream: non-zero data" "streaming tests skipped"
else
    CAPTURE="$TMPDIR/capture.raw"

    # rx888_stream writes samples to stdout.  Capture N seconds' worth.
    timeout $((STREAM_SECONDS + 10)) \
        "$RX888_STREAM" -f "$FIRMWARE" -s "$SAMPLE_RATE" \
        2>"$TMPDIR/stream.log" \
        | head -c $((SAMPLE_RATE * 2 * STREAM_SECONDS)) \
        > "$CAPTURE" &
    STREAM_PID=$!

    sleep "$STREAM_SECONDS"
    kill "$STREAM_PID" 2>/dev/null || true
    wait "$STREAM_PID" 2>/dev/null || true

    BYTE_COUNT=$(stat -c%s "$CAPTURE" 2>/dev/null || echo 0)

    # 9a: did we get any data?
    if [[ "$BYTE_COUNT" -gt 0 ]]; then
        tap_ok "stream: data capture ($BYTE_COUNT bytes in ${STREAM_SECONDS}s)"
    else
        tap_fail "stream: data capture (0 bytes received)" \
                 "$(tail -5 "$TMPDIR/stream.log" 2>/dev/null || echo '(no log)')"
    fi

    # 9b: byte count in the right ballpark?
    EXPECTED=$((SAMPLE_RATE * 2 * STREAM_SECONDS))
    LOW=$((EXPECTED * 50 / 100))
    if [[ "$BYTE_COUNT" -ge "$LOW" ]]; then
        PERCENT=$((BYTE_COUNT * 100 / EXPECTED))
        tap_ok "stream: byte count ${PERCENT}% of expected ($BYTE_COUNT / $EXPECTED)"
    else
        tap_fail "stream: byte count too low ($BYTE_COUNT < $LOW expected)"
    fi

    # 9c: data is not all zeros
    NONZERO=$(od -An -tx1 -N 4096 "$CAPTURE" | tr -d ' \n0' | wc -c)
    if [[ "$NONZERO" -gt 0 ]]; then
        tap_ok "stream: non-zero data present"
    else
        tap_fail "stream: first 4096 bytes are all zero (ADC not sampling?)"
    fi
fi

# ---- Summary ----

echo "#"
echo "# $PASS_COUNT passed, $FAIL_COUNT failed out of $TEST_NUM tests"

if [[ $FAIL_COUNT -gt 0 ]]; then
    exit 1
fi
exit 0
