# SDDC_FX3 Test Tools

Host-side test tools for validating the SDDC_FX3 firmware on RX888mk2
hardware.

## Prerequisites

```
sudo apt install libusb-1.0-0-dev
git submodule update --init       # pulls rx888_tools for firmware upload
```

## Building

```
cd tests
make            # builds fx3_cmd and rx888_stream
make fx3_cmd    # builds just the vendor command exerciser
make clean      # removes build artifacts
```

## fx3_cmd -- Vendor Command Exerciser

Sends individual USB vendor requests to an RX888mk2 and reports
PASS/FAIL.

### Firmware upload

If the device is in bootloader mode (PID 0x00F3), use `-F` to upload
firmware automatically before running a command:

```
./fx3_cmd -F ../SDDC_FX3/SDDC_FX3.img test     # upload + probe
./fx3_cmd -F ../SDDC_FX3/SDDC_FX3.img soak 8   # upload + 8h soak
./fx3_cmd load ../SDDC_FX3/SDDC_FX3.img         # upload only
```

The `-F` flag auto-detects: if firmware is already loaded (PID 0x00F1),
the upload step is skipped.  The `load` command always uploads (resetting
to bootloader first if needed).  Both use `rx888_stream` from the
`rx888_tools` submodule.

### Basic commands

```
./fx3_cmd test                          # probe device info
./fx3_cmd gpio 0x800                    # set GPIO word (LED_BLUE on)
./fx3_cmd adc 64000000                  # set ADC clock to 64 MHz
./fx3_cmd att 15                        # set attenuator (0-63)
./fx3_cmd vga 128                       # set VGA gain (0-255)
./fx3_cmd wdg_max 5                     # set watchdog max recovery count (0=unlimited)
./fx3_cmd start                         # start GPIF streaming
./fx3_cmd stop                          # stop GPIF streaming
./fx3_cmd i2cr 0xC0 0 1                 # I2C read (Si5351 status)
./fx3_cmd i2cw 0xC0 3 0xFF              # I2C write
./fx3_cmd raw 0xCC                      # send arbitrary vendor request
./fx3_cmd stats                         # read GETSTATS counters
./fx3_cmd reset                         # reboot FX3 to bootloader
```

### Automated test commands

Each prints `PASS`/`FAIL` and exits 0/1.  Scenarios cluster into a
few categories — a representative sample below; the canonical full
list lives in the `cmd_table[]` at the top of
`tests/fx3_cmd.c`, or run `./fx3_cmd debug` and type `!help` for
the same list interactively.

**EP0 / vendor-command boundaries:**

```
./fx3_cmd ep0_overflow                  # wLength > 64 must STALL
./fx3_cmd oob_brequest                  # unknown bRequest STALLs
./fx3_cmd oob_setarg                    # SETARGFX3 wIndex=0xFFFF
./fx3_cmd ep0_stall_recovery            # EP0 stall then immediate use
./fx3_cmd vendor_rqt_wrap               # counter wraparound at 256
./fx3_cmd stale_vendor_codes            # dead-zone bRequest values
./fx3_cmd setarg_gap_index              # near-miss SETARGFX3 wIndex
./fx3_cmd ep0_hammer                    # 500 rapid EP0s during stream
```

**GPIF / streaming:**

```
./fx3_cmd stop_gpif_state               # GPIF SM lands in IDLE
./fx3_cmd stop_start_cycle              # cycle STOP+START with data verify
./fx3_cmd gpif_soft_stop                # soft-stop reaches IDLE (needs new waveform)
./fx3_cmd stop_under_backpressure       # STOP while DMA buffers full
./fx3_cmd sustained_stream              # continuous streaming check
./fx3_cmd dma_count_monotonic           # DMA counter monotonic during stream
./fx3_cmd dma_count_reset               # DMA counter reset on STARTFX3
./fx3_cmd data_sanity                   # bulk-data corruption check
```

**Recovery validation:**

```
./fx3_cmd wedge_recovery                # DMA wedge + STOP+START recovery
./fx3_cmd watchdog_cap_observe          # observe watchdog fault plateau
./fx3_cmd watchdog_cap_restart          # restart after watchdog cap
./fx3_cmd clock_pull                    # pull clock mid-stream, verify recovery
./fx3_cmd test_health_recovery          # HANGFX3 + Level-4 EP0 reset round-trip
./fx3_cmd test_main_recovery            # HANGMAIN + Level-5 HWDT round-trip
./fx3_cmd abandoned_stream              # simulate host crash (no STOPFX3)
```

**Si5351 / ADC clock:**

```
./fx3_cmd pll_preflight                 # STARTFX3 rejected without PLL lock
./fx3_cmd clk0_chip_query               # STARTFX3 STALLs after CLK0 power-down via I2CWFX3
./fx3_cmd freq_hop                      # rapid ADC frequency hopping
./fx3_cmd adc_freq_extremes             # edge ADC frequencies
```

**Diagnostics counters:**

```
./fx3_cmd stats                         # snapshot full GETSTATS
./fx3_cmd stats_i2c                     # I2C failure counter
./fx3_cmd stats_pib                     # PIB error counter
./fx3_cmd stats_pll                     # Si5351 PLL lock status
./fx3_cmd stack_check                   # stack watermark > 25%
```

**Debug / I/O paths:**

```
./fx3_cmd console_fill                  # console-buffer fill behavior
./fx3_cmd debug_race                    # rapid debug I/O cycles
./fx3_cmd debug_poll                    # debug console "?" command
./fx3_cmd readinfodebug_flood           # debug buffer flood without drain
./fx3_cmd debug_cmd_while_stream        # debug command during streaming
./fx3_cmd i2c_under_load                # I2C read while streaming
./fx3_cmd i2c_write_bad_addr            # I2C write NACK counter
./fx3_cmd i2c_multibyte                 # multi-byte I2C round-trip
```

### Consumer-failure tests

The `abandoned_stream` test (and future variants) exercises the most
common real-world failure mode: **the host application dies without
sending STOPFX3**.

When an SDR application crashes, is `kill -9`'d, or loses USB
connectivity mid-stream, the FX3 is left streaming into the void.  DMA
buffers fill, the GPIF stalls, and the watchdog fires.  But recovery is
futile — there is no consumer, so the freshly-recovered DMA channel
stalls again within milliseconds.  Without a recovery cap, the firmware
loops forever: detect stall → recover → stall → recover → …

This is not an edge case.  SDR applications are complex, host-side
software, and unclean exits are routine:

- User hits Ctrl-C on a streaming application
- Application segfaults during DSP processing
- USB hub glitch drops the device momentarily
- System suspend/resume while streaming
- OOM killer takes the application

The `abandoned_stream` test simulates this directly:

1. Starts ADC + GPIF streaming at 64 MHz
2. **Does not read EP1.  Does not send STOPFX3.**
3. Waits 10 seconds, taking GETSTATS snapshots at t=0, t=5s, t=10s
4. Checks whether `streaming_faults` plateaued (recovery capped) or is
   still climbing (unbounded loop)
5. Sends STOPFX3 to clean up, verifies EP0 still responds

**Before `WDG_MAX_RECOV` fix**: FAIL — faults grow at ~1/sec in both
windows.
**After fix**: PASS — faults plateau once the cap is reached.

The soak harness assigns this scenario a high weight (15) — tied for
second-highest — reflecting the likelihood of consumer-side failures in
the field.

Future consumer-failure variants to consider:

- **Partial-read abandon**: read some EP1 data, then stop reading (simulates
  application hang rather than crash)
- **Rapid reconnect**: abandon stream, close/reopen USB handle, restart
  (simulates application restart without device reset)
- **Suspend/resume**: abandon stream, issue USB reset, verify clean restart

All follow the same pattern: start streaming → simulate failure → observe
via GETSTATS → clean up → verify EP0 alive.

### Interactive debug console

```
./fx3_cmd debug
```

Opens a live debug console over USB:

- Enables debug mode via `TESTFX3 wValue=1`
- Puts stdin in raw mode (character-at-a-time, no echo)
- Polls `READINFODEBUG` every 50 ms for firmware output
- Typed characters are sent to the firmware; Enter sends CR
- Ctrl-C to quit

#### Local command escape (`!`)

Type `!` to switch to local command mode.  This lets you run any
`fx3_cmd` subcommand without leaving the debug session:

```
!adc 64000000        → runs fx3_cmd adc 64000000
!start               → runs fx3_cmd start
!stop_gpif_state     → runs the GPIF state test
!help                → lists all available local commands
```

Debug output polling continues between commands.  Some test commands
take several seconds; output is paused during execution.

### Soak test (multi-hour stress)

```
./fx3_cmd soak [hours] [seed]
```

Randomly cycles through all test scenarios (weighted by severity) for
the given duration, running a health check between each cycle.
Deterministic given a seed for reproducibility.  Ctrl-C triggers early
shutdown with a full summary.

```
# Quick 1-minute sanity check
./fx3_cmd soak 0.016 42

# Default: 1 hour, random seed
./fx3_cmd soak

# 8-hour overnight run
./fx3_cmd soak 8

# Bias the rotation: surface a specific scenario faster by cranking
# its weight (--weight / -w NAME=N, repeatable, max 32 overrides).
./fx3_cmd soak 1 42 --weight test_health_recovery=20 \
                    --weight test_main_recovery=20
```

Output includes a status line every 10 cycles and a final table:

```
=== SOAK TEST SUMMARY ===
Duration: 01:00:00  Seed: 12345  Cycles: 1847

Scenario              Runs  Pass  Fail
stop_start_cycle       370   370     0
wedge_recovery         277   275     2
...
TOTAL                 1847  1843     4
```

## soak_test.sh -- Soak Test Wrapper

Handles firmware upload and device probe before invoking `fx3_cmd soak`.

```
./soak_test.sh --firmware ../SDDC_FX3/SDDC_FX3.img --hours 4 --seed 42
```

### Options

```
--firmware PATH        Firmware .img file (required)
--hours N              Soak duration in hours (default: 1, fractional OK)
--seed S               PRNG seed for reproducibility (default: time-based)
--rx888-stream PATH    Path to rx888_stream binary
--no-reset             Skip USB reset on exit (leave device as-is)
--reload-interval N    Every N scenarios, reset device to DFU, re-upload
                       firmware, and resume.  Default: 0 (disabled).
                       Each reload adds ~10s.  Tests whether a freshly
                       loaded firmware handles stress correctly.
```

Per-scenario weight overrides (`--weight NAME=N`) are not forwarded
by this wrapper — for biased rotations, invoke `./fx3_cmd soak`
directly.

## fw_test.sh -- Automated TAP Test Suite

Runs an end-to-end firmware test suite in TAP format: firmware
upload, device probe, all stale-command / OOB / debug-path / GPIF
checks, GETSTATS counter validation, watchdog and stop/start
cycles, and (optionally) a live streaming capture.  Handles
firmware upload automatically via `rx888_stream`.

```
./fw_test.sh --firmware ../SDDC_FX3/SDDC_FX3.img
```

### Options

```
--firmware PATH        Firmware .img file (required)
--stream-seconds N     Streaming capture duration (default: 5)
--rx888-stream PATH    Path to rx888_stream binary
--skip-stream          Skip streaming tests (tests 31-33)
--sample-rate HZ       ADC sample rate (default: 32000000)
```

### Requirements

- RX888mk2 connected via USB
- `fx3_cmd` and `rx888_stream` built (run `make` first)
- User must have USB device permissions (udev rule or run as root)

### Example output

```
1..N
ok 1 - firmware upload (PID 0x00F1)
ok 2 - device probe (hwconfig=0x04)
ok 3 - GPIO set LED_BLUE
...
ok N - stream: non-zero data present
#
# N passed, 0 failed out of N tests
```

`N` is set by the `PLANNED` counter at the top of `fw_test.sh` and
varies as scenarios are added.  Each block in the script prints
`ok` or `not ok` for one numbered TAP entry; the streaming test at
the end contributes a few additional sub-assertions.

## File map

| File | Purpose |
|------|---------|
| `fx3_cmd.c` | Vendor command exerciser, test harness, and soak test |
| `fw_test.sh` | TAP test suite wrapper (single-pass) |
| `soak_test.sh` | Soak test wrapper (firmware upload + `fx3_cmd soak`) |
| `Makefile` | Build system for test tools |
| `rx888_tools/` | Git submodule: firmware uploader and USB streamer |
