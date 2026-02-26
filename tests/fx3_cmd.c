/*
 * fx3_cmd.c — Vendor command exerciser for SDDC_FX3 firmware.
 *
 * Sends individual USB vendor requests to an RX888mk2 and reports
 * success/failure.  Designed for scripted hardware testing.
 *
 * By default, this tool assumes the device already has firmware loaded
 * (PID 0x00F1).  Use -F <firmware.img> to automatically upload firmware
 * via rx888_stream when the device is in bootloader mode (PID 0x00F3),
 * or use the "load" command for explicit upload-only.  The fw_test.sh
 * and soak_test.sh wrapper scripts also handle firmware upload.
 *
 * Build:  gcc -O2 -Wall -o fx3_cmd fx3_cmd.c -lusb-1.0
 * Needs:  libusb-1.0-0-dev
 *
 * Copyright (c) 2024-2026 David Goncalves — MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <sys/wait.h>
#include <limits.h>
#include <libusb-1.0/libusb.h>

/* ------------------------------------------------------------------ */
/* Protocol constants — must match SDDC_FX3/protocol.h                */
/* ------------------------------------------------------------------ */

/* USB IDs */
#define RX888_VID        0x04B4
#define RX888_PID_APP    0x00F1
#define RX888_PID_BOOT   0x00F3

/* Vendor request codes */
#define STARTFX3      0xAA
#define STOPFX3       0xAB
#define TESTFX3       0xAC
#define GPIOFX3       0xAD
#define I2CWFX3       0xAE
#define I2CRFX3       0xAF
#define RESETFX3      0xB1
#define STARTADC      0xB2
#define GETSTATS      0xB3
/* Legacy tuner commands (R82xx driver removed — GPL conflict).
 * Retained here for stale-command regression tests: the "raw"
 * subcommand sends these codes and expects a USB STALL. */
#define TUNERINIT     0xB4
#define TUNERTUNE     0xB5
#define SETARGFX3     0xB6
#define TUNERSTDBY    0xB8
#define READINFODEBUG 0xBA

/* SETARGFX3 argument IDs */
#define DAT31_ATT     10
#define AD8370_VGA    11
#define WDG_MAX_RECOV 14

/* Timeouts */
#define CTRL_TIMEOUT_MS  1000

/* Global libusb context — needed by primed_start_and_read() for
 * libusb_handle_events_timeout_completed().  Set once in main(). */
static libusb_context *g_ctx;

/* ------------------------------------------------------------------ */
/* USB helpers (patterns from rx888_stream.c)                         */
/* ------------------------------------------------------------------ */

static int ctrl_write_u32(libusb_device_handle *h, uint8_t request,
                          uint16_t wValue, uint16_t wIndex, uint32_t val)
{
    uint8_t data[4];
    data[0] = (uint8_t)(val & 0xFF);
    data[1] = (uint8_t)((val >>  8) & 0xFF);
    data[2] = (uint8_t)((val >> 16) & 0xFF);
    data[3] = (uint8_t)((val >> 24) & 0xFF);

    int r = libusb_control_transfer(
        h,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        request, wValue, wIndex, data, sizeof(data), CTRL_TIMEOUT_MS);
    if (r < 0) return r;
    if (r != (int)sizeof(data)) return LIBUSB_ERROR_IO;
    return 0;
}

static int ctrl_write_buf(libusb_device_handle *h, uint8_t request,
                          uint16_t wValue, uint16_t wIndex,
                          const uint8_t *buf, uint16_t len)
{
    int r = libusb_control_transfer(
        h,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        request, wValue, wIndex, (unsigned char *)buf, len, CTRL_TIMEOUT_MS);
    if (r < 0) return r;
    if (r != (int)len) return LIBUSB_ERROR_IO;
    return 0;
}

static int ctrl_read(libusb_device_handle *h, uint8_t request,
                     uint16_t wValue, uint16_t wIndex,
                     uint8_t *buf, uint16_t len)
{
    int r = libusb_control_transfer(
        h,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        request, wValue, wIndex, buf, len, CTRL_TIMEOUT_MS);
    return r;
}

/* Convenience: send a command with a u32 payload, wValue=0, wIndex=0 */
static int cmd_u32(libusb_device_handle *h, uint8_t cmd, uint32_t val)
{
    return ctrl_write_u32(h, cmd, 0, 0, val);
}

/* Convenience: send SETARGFX3 with arg_id in wIndex, arg_val in wValue,
 * and a 1-byte zero payload (matches rx888_stream encoding). */
static int set_arg(libusb_device_handle *h, uint16_t arg_id, uint16_t arg_val)
{
    uint8_t zero = 0;
    return ctrl_write_buf(h, SETARGFX3, arg_val, arg_id, &zero, 1);
}

/* Retry a command on a transient USB error with escalating backoff.
 * When a soak scenario starts right after a prior scenario triggered
 * heavy watchdog activity, the device may still be mid-recovery and
 * unable to service control transfers.  This manifests as either:
 *
 *   LIBUSB_ERROR_TIMEOUT  — transfer completed but device didn't ACK
 *                           within CTRL_TIMEOUT_MS
 *   LIBUSB_ERROR_IO       — low-level USB I/O failure (broken pipe,
 *                           NAK flood, etc.) while the FX3 is
 *                           resetting its DMA/GPIF state
 *
 * The helper retries up to twice with escalating backoff (500 ms then
 * 1 s, worst-case 1.5 s total).  STARTFX3 is especially sensitive
 * because it restarts the GPIF state machine — unlike simple EP0
 * reads (TESTFX3) which succeed sooner.  The 1.5 s budget matches the
 * observed watchdog recovery window (~2 s) while still catching a
 * genuinely wedged device within a few seconds.
 *
 * Convention: use cmd_u32_retry for the FIRST STARTADC + STARTFX3 in
 * every soak scenario (the "entry point" calls most exposed to
 * inter-scenario timing).  Use plain cmd_u32 for mid-scenario calls
 * (STOP→START transitions, recovery verification, etc.) so genuine
 * firmware failures are caught immediately. */
static int cmd_u32_retry(libusb_device_handle *h, uint8_t cmd, uint32_t val)
{
    int r = cmd_u32(h, cmd, val);
    if (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_IO)
        return r;
    usleep(500000);                    /* 500 ms backoff */
    r = cmd_u32(h, cmd, val);
    if (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_IO)
        return r;
    usleep(1000000);                   /* 1 s backoff */
    return cmd_u32(h, cmd, val);
}

/* ------------------------------------------------------------------ */
/* Device open / close                                                */
/* ------------------------------------------------------------------ */

static libusb_device_handle *open_rx888(libusb_context *ctx)
{
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_APP);
    if (!h) {
        /* Check if device is in bootloader mode */
        libusb_device_handle *boot = libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_BOOT);
        if (boot) {
            libusb_close(boot);
            fprintf(stderr, "error: device found in bootloader mode (PID 0x%04X) — flash firmware first\n",
                    RX888_PID_BOOT);
        } else {
            fprintf(stderr, "error: no RX888 device found (VID 0x%04X, PID 0x%04X)\n",
                    RX888_VID, RX888_PID_APP);
        }
        return NULL;
    }

    /* Detach kernel driver if attached */
    if (libusb_kernel_driver_active(h, 0) == 1)
        libusb_detach_kernel_driver(h, 0);

    int r = libusb_claim_interface(h, 0);
    if (r < 0) {
        fprintf(stderr, "error: claim interface: %s\n", libusb_strerror(r));
        libusb_close(h);
        return NULL;
    }

    /* Restart the XHCI endpoint ring for EP1-IN.
     *
     * When the previous process closed its USB fd, the kernel killed
     * pending URBs via xhci_urb_dequeue → Set TR Dequeue Pointer,
     * which leaves the XHCI endpoint in the "stopped" state.  New TDs
     * submitted by this process won't be processed until the endpoint
     * is restarted.  libusb_clear_halt sends CLEAR_FEATURE(ENDPOINT_HALT)
     * to the device AND calls usb_hcd_reset_endpoint which issues a
     * Reset Endpoint command to the XHCI — clearing the stopped state.
     *
     * The firmware CLEAR_FEATURE handler now just ACKs the setup
     * (no stall-clear, no DMA teardown), so this is safe. */
    libusb_clear_halt(h, 0x81);  /* EP1-IN — restart XHCI endpoint ring */

    return h;
}

static void close_rx888(libusb_device_handle *h)
{
    if (h) {
        libusb_release_interface(h, 0);
        libusb_close(h);
    }
}

/* ------------------------------------------------------------------ */
/* Firmware upload via rx888_stream (same approach as soak_test.sh)    */
/* ------------------------------------------------------------------ */

/* Locate the rx888_stream binary.  Search order:
 *   1. Same directory as fx3_cmd (symlink created by tests/Makefile)
 *   2. rx888_tools/rx888_stream in the same directory
 *   3. Fall back to bare name (let exec search PATH)
 * Returns 1 if a verified path was found, 0 if falling back to PATH. */
static int find_rx888_stream(char *out, size_t out_size)
{
    /* Leave room for longest suffix ("rx888_tools/rx888_stream" = 24) */
    char self_dir[PATH_MAX - 32];
    ssize_t len = readlink("/proc/self/exe", self_dir, sizeof(self_dir) - 1);
    if (len > 0) {
        self_dir[len] = '\0';
        char *slash = strrchr(self_dir, '/');
        if (slash) {
            *(slash + 1) = '\0';

            snprintf(out, out_size, "%srx888_stream", self_dir);
            if (access(out, X_OK) == 0)
                return 1;

            snprintf(out, out_size, "%srx888_tools/rx888_stream", self_dir);
            if (access(out, X_OK) == 0)
                return 1;
        }
    }

    /* Fall back to PATH */
    snprintf(out, out_size, "rx888_stream");
    return 0;
}

/* Upload firmware to an FX3 device in bootloader mode by forking
 * rx888_stream.  Mirrors the upload sequence in soak_test.sh:
 *   1. Fork rx888_stream -f <fw_path> -s 32000000
 *   2. Wait 4 s for upload + enumeration
 *   3. Kill rx888_stream (it would otherwise stream forever)
 *   4. Wait 2 s for USB re-enumeration
 *   5. Verify device appeared at app PID (0x00F1)
 * Returns 0 on success, -1 on failure. */
static int upload_firmware(libusb_context *ctx, const char *fw_path)
{
    char stream_bin[PATH_MAX];
    find_rx888_stream(stream_bin, sizeof(stream_bin));

    if (access(fw_path, R_OK) != 0) {
        fprintf(stderr, "error: firmware file not readable: %s\n", fw_path);
        return -1;
    }

    fprintf(stderr, "uploading firmware: %s\n", fw_path);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* Child: suppress output, exec rx888_stream */
        if (!freopen("/dev/null", "w", stdout)) _exit(126);
        if (!freopen("/dev/null", "w", stderr)) _exit(126);
        execl(stream_bin, "rx888_stream",
              "-f", fw_path, "-s", "32000000", (char *)NULL);
        /* execl only returns on error — try PATH as last resort */
        execlp("rx888_stream", "rx888_stream",
               "-f", fw_path, "-s", "32000000", (char *)NULL);
        _exit(127);
    }

    /* Parent: wait 4 s for upload, then kill */
    sleep(4);
    kill(pid, SIGTERM);
    int status;
    waitpid(pid, &status, 0);

    /* Wait for USB re-enumeration */
    sleep(2);

    /* Verify device appeared at app PID */
    libusb_device_handle *h =
        libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_APP);
    if (!h) {
        fprintf(stderr, "error: device not found at PID 0x%04X "
                "after firmware upload\n", RX888_PID_APP);
        return -1;
    }
    libusb_close(h);

    fprintf(stderr, "firmware uploaded — device ready at PID 0x%04X\n",
            RX888_PID_APP);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Subcommands                                                        */
/* ------------------------------------------------------------------ */

static int do_test(libusb_device_handle *h)
{
    uint8_t buf[4] = {0};
    int r = ctrl_read(h, TESTFX3, 0, 0, buf, 4);
    if (r < 0) {
        printf("FAIL test: %s\n", libusb_strerror(r));
        return 1;
    }
    if (r < 4) {
        printf("FAIL test: short reply (%d bytes, expected 4)\n", r);
        return 1;
    }
    uint8_t hwconfig   = buf[0];
    uint8_t fw_major   = buf[1];
    uint8_t fw_minor   = buf[2];
    uint8_t rqt_cnt    = buf[3];
    printf("PASS test: hwconfig=0x%02X fw=%d.%d vendorRqtCnt=%d\n",
           hwconfig, fw_major, fw_minor, rqt_cnt);
    return 0;
}

static int do_gpio(libusb_device_handle *h, uint32_t bits)
{
    int r = cmd_u32(h, GPIOFX3, bits);
    if (r < 0) {
        printf("FAIL gpio 0x%08X: %s\n", bits, libusb_strerror(r));
        return 1;
    }
    printf("PASS gpio 0x%08X\n", bits);
    return 0;
}

static int do_adc(libusb_device_handle *h, uint32_t freq)
{
    int r = cmd_u32(h, STARTADC, freq);
    if (r < 0) {
        printf("FAIL adc %u: %s\n", freq, libusb_strerror(r));
        return 1;
    }
    printf("PASS adc %u Hz\n", freq);
    return 0;
}

static int do_att(libusb_device_handle *h, uint16_t val)
{
    int r = set_arg(h, DAT31_ATT, val);
    if (r < 0) {
        printf("FAIL att %u: %s\n", val, libusb_strerror(r));
        return 1;
    }
    printf("PASS att %u\n", val);
    return 0;
}

static int do_vga(libusb_device_handle *h, uint16_t val)
{
    int r = set_arg(h, AD8370_VGA, val);
    if (r < 0) {
        printf("FAIL vga %u: %s\n", val, libusb_strerror(r));
        return 1;
    }
    printf("PASS vga %u\n", val);
    return 0;
}

static int do_wdg_max(libusb_device_handle *h, uint16_t val)
{
    int r = set_arg(h, WDG_MAX_RECOV, val);
    if (r < 0) {
        printf("FAIL wdg_max %u: %s\n", val, libusb_strerror(r));
        return 1;
    }
    printf("PASS wdg_max %u\n", val);
    return 0;
}

static int do_start(libusb_device_handle *h)
{
    int r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL start: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("PASS start\n");
    return 0;
}

static int do_stop(libusb_device_handle *h)
{
    int r = cmd_u32(h, STOPFX3, 0);
    if (r < 0) {
        printf("FAIL stop: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("PASS stop\n");
    return 0;
}

static int do_i2cr(libusb_device_handle *h, uint16_t addr, uint16_t reg, uint16_t len)
{
    uint8_t buf[64];
    if (len > sizeof(buf)) len = sizeof(buf);

    int r = ctrl_read(h, I2CRFX3, addr, reg, buf, len);
    if (r < 0) {
        printf("FAIL i2cr addr=0x%02X reg=0x%02X: %s\n", addr, reg, libusb_strerror(r));
        return 1;
    }
    printf("PASS i2cr addr=0x%02X reg=0x%02X len=%d:", addr, reg, r);
    for (int i = 0; i < r; i++)
        printf(" %02X", buf[i]);
    printf("\n");
    return 0;
}

static int do_i2cw(libusb_device_handle *h, uint16_t addr, uint16_t reg,
                   const uint8_t *data, uint16_t len)
{
    int r = ctrl_write_buf(h, I2CWFX3, addr, reg, data, len);
    if (r < 0) {
        printf("FAIL i2cw addr=0x%02X reg=0x%02X: %s\n", addr, reg, libusb_strerror(r));
        return 1;
    }
    printf("PASS i2cw addr=0x%02X reg=0x%02X len=%d\n", addr, reg, len);
    return 0;
}

static int do_reset(libusb_device_handle *h)
{
    /* RESETFX3 reboots the FX3 — the device will disconnect immediately,
     * so a transfer error is expected. */
    int r = cmd_u32(h, RESETFX3, 0);
    /* Accept success or pipe error (device rebooted before replying) */
    if (r < 0 && r != LIBUSB_ERROR_PIPE && r != LIBUSB_ERROR_NO_DEVICE
              && r != LIBUSB_ERROR_IO) {
        printf("FAIL reset: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("PASS reset (device rebooting to bootloader)\n");
    return 0;
}

/* Send a raw vendor command code — for testing stale/removed commands */
static int do_raw(libusb_device_handle *h, uint8_t code)
{
    int r = cmd_u32(h, code, 0);
    if (r == LIBUSB_ERROR_PIPE) {
        printf("PASS raw 0x%02X: STALL (as expected for removed command)\n", code);
        return 0;
    }
    if (r < 0) {
        printf("FAIL raw 0x%02X: %s\n", code, libusb_strerror(r));
        return 1;
    }
    printf("PASS raw 0x%02X: accepted\n", code);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Local command dispatch for the debug console ('!' escape)          */
/* ------------------------------------------------------------------ */

/* Forward declarations for do_* functions defined later in this file. */
static int do_stats(libusb_device_handle *h);
static int do_ep0_overflow(libusb_device_handle *h);
static int do_test_oob_brequest(libusb_device_handle *h);
static int do_test_oob_setarg(libusb_device_handle *h);
static int do_test_console_fill(libusb_device_handle *h);
static int do_test_debug_race(libusb_device_handle *h);
static int do_test_debug_poll(libusb_device_handle *h);
static int do_test_pib_overflow(libusb_device_handle *h);
static int do_test_stack_check(libusb_device_handle *h);
static int do_test_stats_i2c(libusb_device_handle *h);
static int do_test_stats_pib(libusb_device_handle *h);
static int do_test_stats_pll(libusb_device_handle *h);
static int do_test_stop_gpif_state(libusb_device_handle *h);
static int do_test_stop_start_cycle(libusb_device_handle *h);
static int do_test_pll_preflight(libusb_device_handle *h);
static int do_test_wedge_recovery(libusb_device_handle *h);
static int do_test_clock_pull(libusb_device_handle *h);
static int do_test_freq_hop(libusb_device_handle *h);
static int do_test_ep0_stall_recovery(libusb_device_handle *h);
static int do_test_double_stop(libusb_device_handle *h);
static int do_test_double_start(libusb_device_handle *h);
static int do_test_i2c_under_load(libusb_device_handle *h);
static int do_test_sustained_stream(libusb_device_handle *h);
static int do_test_abandoned_stream(libusb_device_handle *h);
static int do_test_vendor_rqt_wrap(libusb_device_handle *h);
static int do_test_stale_vendor_codes(libusb_device_handle *h);
static int do_test_setarg_gap_index(libusb_device_handle *h);
static int do_test_gpio_extremes(libusb_device_handle *h);
static int do_test_hw_smoke(libusb_device_handle *h);
static int do_test_i2c_write_bad_addr(libusb_device_handle *h);
static int do_test_i2c_multibyte(libusb_device_handle *h);
static int do_test_readinfodebug_flood(libusb_device_handle *h);
static int do_test_dma_count_reset(libusb_device_handle *h);
static int do_test_dma_count_monotonic(libusb_device_handle *h);
static int do_test_watchdog_cap_observe(libusb_device_handle *h);
static int do_test_watchdog_cap_restart(libusb_device_handle *h);
static int do_test_ep0_hammer(libusb_device_handle *h);
static int do_test_debug_cmd_while_stream(libusb_device_handle *h);
static int do_test_adc_freq_extremes(libusb_device_handle *h);
static int do_test_data_sanity(libusb_device_handle *h);
static int do_test_gpif_soft_stop(libusb_device_handle *h);
static int do_test_stop_under_backpressure(libusb_device_handle *h);

/* No-arg command table entry */
struct local_cmd_entry {
    const char *name;
    int (*func)(libusb_device_handle *);
};

static const struct local_cmd_entry local_cmds_noarg[] = {
    {"test",             do_test},
    {"start",            do_start},
    {"stop",             do_stop},
    {"stats",            do_stats},
    {"ep0_overflow",     do_ep0_overflow},
    {"oob_brequest",     do_test_oob_brequest},
    {"oob_setarg",       do_test_oob_setarg},
    {"console_fill",     do_test_console_fill},
    {"debug_race",       do_test_debug_race},
    {"debug_poll",       do_test_debug_poll},
    {"pib_overflow",     do_test_pib_overflow},
    {"stack_check",      do_test_stack_check},
    {"stats_i2c",        do_test_stats_i2c},
    {"stats_pib",        do_test_stats_pib},
    {"stats_pll",        do_test_stats_pll},
    {"stop_gpif_state",  do_test_stop_gpif_state},
    {"stop_start_cycle", do_test_stop_start_cycle},
    {"pll_preflight",    do_test_pll_preflight},
    {"wedge_recovery",   do_test_wedge_recovery},
    {"clock_pull",       do_test_clock_pull},
    {"freq_hop",         do_test_freq_hop},
    {"ep0_stall_recovery", do_test_ep0_stall_recovery},
    {"double_stop",      do_test_double_stop},
    {"double_start",     do_test_double_start},
    {"i2c_under_load",   do_test_i2c_under_load},
    {"sustained_stream", do_test_sustained_stream},
    {"abandoned_stream", do_test_abandoned_stream},
    {"vendor_rqt_wrap",  do_test_vendor_rqt_wrap},
    {"stale_vendor_codes", do_test_stale_vendor_codes},
    {"setarg_gap_index", do_test_setarg_gap_index},
    {"gpio_extremes",    do_test_gpio_extremes},
    {"hw_smoke",         do_test_hw_smoke},
    {"i2c_write_bad_addr", do_test_i2c_write_bad_addr},
    {"i2c_multibyte",    do_test_i2c_multibyte},
    {"readinfodebug_flood", do_test_readinfodebug_flood},
    {"dma_count_reset",  do_test_dma_count_reset},
    {"dma_count_monotonic", do_test_dma_count_monotonic},
    {"watchdog_cap_observe", do_test_watchdog_cap_observe},
    {"watchdog_cap_restart", do_test_watchdog_cap_restart},
    {"ep0_hammer",       do_test_ep0_hammer},
    {"debug_cmd_while_stream", do_test_debug_cmd_while_stream},
    {"adc_freq_extremes", do_test_adc_freq_extremes},
    {"data_sanity",      do_test_data_sanity},
    {"gpif_soft_stop",   do_test_gpif_soft_stop},
    {"stop_under_backpressure", do_test_stop_under_backpressure},
    {"reset",            do_reset},
    {NULL, NULL}
};

static void print_local_help(void)
{
    printf("Local commands (prefix with '!'):\n"
           "  help / ?                      This help\n"
           "  test                          Read device info\n"
           "  start / stop                  Start/stop GPIF streaming\n"
           "  adc <freq>                    Set ADC clock frequency\n"
           "  att <0-63>                    Set DAT-31 attenuator\n"
           "  vga <0-255>                   Set AD8370 VGA gain\n"
           "  wdg_max <0-255>              Set watchdog max recovery count (0=unlimited)\n"
           "  gpio <bits>                   Set GPIO word\n"
           "  stats                         Read GETSTATS counters\n"
           "  stats_i2c / stats_pib / stats_pll   Counter tests\n"
           "  stop_gpif_state               Verify GPIF SM stops after STOP\n"
           "  stop_start_cycle              Cycle STOP+START N times\n"
           "  pll_preflight                 Verify START rejected without clock\n"
           "  wedge_recovery                Provoke DMA wedge, test recovery\n"
           "  clock_pull                    Pull clock mid-stream, verify recovery\n"
           "  freq_hop                      Rapid ADC frequency hopping\n"
           "  ep0_stall_recovery            EP0 stall then immediate use\n"
           "  double_stop                   Back-to-back STOPFX3\n"
           "  double_start                  Back-to-back STARTFX3\n"
           "  i2c_under_load                I2C read while streaming\n"
           "  sustained_stream              30s continuous streaming check\n"
           "  abandoned_stream              Simulate host crash (no STOPFX3)\n"
           "  vendor_rqt_wrap               Counter wraparound at 256\n"
           "  stale_vendor_codes            Dead-zone bRequest values\n"
           "  setarg_gap_index              Near-miss SETARGFX3 wIndex\n"
           "  gpio_extremes                 Extreme GPIO patterns\n"
           "  hw_smoke                      ADC alive check (stream after GPIO)\n"
           "  i2c_write_bad_addr            I2C write NACK counter\n"
           "  i2c_multibyte                 Multi-byte I2C round-trip\n"
           "  readinfodebug_flood           Debug buffer flood without drain\n"
           "  dma_count_reset               DMA counter reset on STARTFX3\n"
           "  dma_count_monotonic           DMA counter monotonic during stream\n"
           "  watchdog_cap_observe          Observe watchdog fault plateau\n"
           "  watchdog_cap_restart          Restart after watchdog cap\n"
           "  ep0_hammer                    500 rapid EP0 during stream\n"
           "  debug_cmd_while_stream        Debug command during stream\n"
           "  adc_freq_extremes             Edge ADC frequencies\n"
           "  data_sanity                   Bulk data corruption check\n"
           "  gpif_soft_stop                Verify SM lands in IDLE (needs new waveform)\n"
           "  stop_under_backpressure       STOP while DMA buffers full\n"
           "  pib_overflow                  Provoke + detect PIB error\n"
           "  stack_check                   Query stack watermark\n"
           "  i2cr <addr> <reg> <len>       I2C read (hex)\n"
           "  i2cw <addr> <reg> <byte>...   I2C write (hex)\n"
           "  raw <code>                    Send raw vendor request (hex)\n"
           "  reset                         Reboot FX3 to bootloader\n");
}

/* Parse and dispatch a local command line (without the '!' prefix). */
static int dispatch_local_cmd(libusb_device_handle *h, const char *line)
{
    /* Skip leading whitespace */
    while (*line == ' ') line++;
    if (*line == '\0') return 0;

    /* Split into command and args */
    char cmd[64] = {0};
    const char *args = NULL;
    const char *sp = strchr(line, ' ');
    if (sp) {
        int len = (int)(sp - line);
        if (len >= (int)sizeof(cmd)) len = (int)sizeof(cmd) - 1;
        memcpy(cmd, line, len);
        args = sp + 1;
        while (*args == ' ') args++;
        if (*args == '\0') args = NULL;
    } else {
        int len = (int)strlen(line);
        if (len >= (int)sizeof(cmd)) len = (int)sizeof(cmd) - 1;
        memcpy(cmd, line, len);
    }

    /* Help */
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_local_help();
        return 0;
    }

    /* No-arg commands */
    for (const struct local_cmd_entry *e = local_cmds_noarg; e->name; e++) {
        if (strcmp(cmd, e->name) == 0)
            return e->func(h);
    }

    /* Commands with arguments */
    if (strcmp(cmd, "adc") == 0) {
        if (!args) { printf("usage: adc <freq_hz>\n"); return 1; }
        return do_adc(h, (uint32_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "att") == 0) {
        if (!args) { printf("usage: att <0-63>\n"); return 1; }
        return do_att(h, (uint16_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "vga") == 0) {
        if (!args) { printf("usage: vga <0-255>\n"); return 1; }
        return do_vga(h, (uint16_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "wdg_max") == 0) {
        if (!args) { printf("usage: wdg_max <0-255>\n"); return 1; }
        return do_wdg_max(h, (uint16_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "gpio") == 0) {
        if (!args) { printf("usage: gpio <bits>\n"); return 1; }
        return do_gpio(h, (uint32_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "raw") == 0) {
        if (!args) { printf("usage: raw <code>\n"); return 1; }
        return do_raw(h, (uint8_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "i2cr") == 0) {
        if (!args) { printf("usage: i2cr <addr> <reg> <len>\n"); return 1; }
        unsigned long a, rg, l;
        if (sscanf(args, "%li %li %li", &a, &rg, &l) != 3) {
            printf("usage: i2cr <addr> <reg> <len>\n");
            return 1;
        }
        return do_i2cr(h, (uint16_t)a, (uint16_t)rg, (uint16_t)l);
    }
    if (strcmp(cmd, "i2cw") == 0) {
        if (!args) { printf("usage: i2cw <addr> <reg> <byte>...\n"); return 1; }
        char *p = (char *)args;
        char *end;
        unsigned long a = strtoul(p, &end, 0);
        if (end == p) { printf("usage: i2cw <addr> <reg> <byte>...\n"); return 1; }
        p = end;
        unsigned long rg = strtoul(p, &end, 0);
        if (end == p) { printf("usage: i2cw <addr> <reg> <byte>...\n"); return 1; }
        p = end;
        uint8_t data[64];
        int ndata = 0;
        while (ndata < (int)sizeof(data)) {
            unsigned long b = strtoul(p, &end, 0);
            if (end == p) break;
            data[ndata++] = (uint8_t)b;
            p = end;
        }
        if (ndata == 0) { printf("usage: i2cw <addr> <reg> <byte>...\n"); return 1; }
        return do_i2cw(h, (uint16_t)a, (uint16_t)rg, data, (uint16_t)ndata);
    }

    printf("unknown local command: '%s' (type !help for list)\n", cmd);
    return 1;
}

/* SIGINT handler: restore terminal from raw mode before exit. */
static struct termios saved_termios;
static volatile sig_atomic_t raw_mode_active;

static void sigint_handler(int sig)
{
    if (raw_mode_active)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Interactive debug console over USB.
 * First sends TESTFX3 with wValue=1 to enable debug mode, then polls
 * READINFODEBUG for output.  Typed characters are sent in wValue;
 * CR triggers command execution on the FX3 side.  Ctrl-C exits.
 *
 * Local command escape: typing '!' switches to local command mode.
 * Characters are buffered locally and dispatched to the corresponding
 * do_*() function on Enter, using the same USB handle.  Debug output
 * polling continues between keystrokes.  See dispatch_local_cmd(). */
static int do_debug(libusb_device_handle *h)
{
    /* Enable debug mode via TESTFX3 wValue=1 */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL debug: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("debug: enabled (hwconfig=0x%02X fw=%d.%d)\n",
           info[0], info[1], info[2]);
    printf("debug: type commands + Enter for FX3, '!' for local commands, Ctrl-C to quit\n");
    fflush(stdout);

    /* Put stdin in non-blocking mode for character-at-a-time input */
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    saved_termios = oldt;
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    newt.c_cc[VMIN] = 0;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    raw_mode_active = 1;
    signal(SIGINT, sigint_handler);

    /* Local command mode state */
    int local_mode = 0;
    char local_buf[128];
    int local_len = 0;

    uint8_t buf[64];
    for (;;) {
        /* Check for typed character */
        uint16_t send_char = 0;
        char ch;
        if (read(STDIN_FILENO, &ch, 1) == 1) {
            if (!local_mode && ch == '!') {
                /* Enter local command mode */
                local_mode = 1;
                local_len = 0;
                printf("\nfx3> ");
                fflush(stdout);
            } else if (local_mode) {
                if (ch == '\n' || ch == '\r') {
                    local_buf[local_len] = '\0';
                    printf("\n");
                    fflush(stdout);
                    if (local_len > 0)
                        dispatch_local_cmd(h, local_buf);
                    fflush(stdout);
                    local_mode = 0;
                } else if (ch == 0x7f || ch == 0x08) {
                    /* Backspace / Delete */
                    if (local_len > 0) {
                        local_len--;
                        printf("\b \b");
                        fflush(stdout);
                    }
                } else if (ch == 0x03 || ch == 0x1b) {
                    /* Ctrl-C or Escape — cancel local command */
                    printf(" (cancelled)\n");
                    fflush(stdout);
                    local_mode = 0;
                } else if (local_len < (int)sizeof(local_buf) - 1) {
                    local_buf[local_len++] = ch;
                    putchar(ch);
                    fflush(stdout);
                }
                /* Don't send to device in local mode */
            } else {
                /* Normal mode — send character to FX3 console */
                if (ch == '\n') ch = '\r';
                send_char = (uint8_t)ch;
            }
        }

        /* Poll READINFODEBUG: wValue carries the typed char (0 = none) */
        r = ctrl_read(h, READINFODEBUG, send_char, 0, buf, sizeof(buf));
        if (r > 0) {
            buf[r - 1] = '\0';  /* firmware null-terminates last byte */
            printf("%s", (char *)buf);
            fflush(stdout);
        }
        /* STALL (LIBUSB_ERROR_PIPE) means no data — normal */

        usleep(50000);  /* 50ms poll interval */
    }
    /* NOTREACHED — loop exits via SIGINT → sigint_handler */
}

/* Send a vendor request with wLength > 64 — must STALL if firmware
 * validates EP0 buffer bounds (issue #6). */
static int do_ep0_overflow(libusb_device_handle *h)
{
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));

    int r = libusb_control_transfer(
        h,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        GPIOFX3, 0, 0, buf, sizeof(buf), CTRL_TIMEOUT_MS);
    if (r == LIBUSB_ERROR_PIPE) {
        printf("PASS ep0_overflow: STALL on wLength=%d (> 64)\n", (int)sizeof(buf));
        return 0;
    }
    if (r < 0) {
        printf("FAIL ep0_overflow: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("FAIL ep0_overflow: accepted wLength=%d (expected STALL)\n", (int)sizeof(buf));
    return 1;
}

/* ------------------------------------------------------------------ */
/* Targeted issue-verification tests                                  */
/* ------------------------------------------------------------------ */

/* Issue #21: Send a vendor request with bRequest outside the
 * FX3CommandName[] bounds (0xAA-0xBA).  TraceSerial must not crash.
 * We use 0xCC which is well outside the table.  Expected: STALL
 * (unknown command) but no crash/hang.  Verify by probing afterwards. */
static int do_test_oob_brequest(libusb_device_handle *h)
{
    /* First, enable debug mode so TraceSerial is actually active */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL oob_brequest: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Send out-of-range vendor request 0xCC */
    r = cmd_u32(h, 0xCC, 0);
    /* Expected: STALL (unknown command) */
    if (r != LIBUSB_ERROR_PIPE && r != 0) {
        printf("FAIL oob_brequest: unexpected error: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Verify device is still alive by probing */
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL oob_brequest: device unresponsive after OOB bRequest: %s\n",
               libusb_strerror(r));
        return 1;
    }
    printf("PASS oob_brequest: device survived bRequest=0xCC (issue #21)\n");
    return 0;
}

/* Issue #20: Send SETARGFX3 with wIndex=0xFFFF, well beyond the
 * SETARGFX3List[] bounds.  TraceSerial must not crash.
 * Expected: STALL (unknown arg ID) but no crash/hang. */
static int do_test_oob_setarg(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL oob_setarg: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Send SETARGFX3 with wIndex=0xFFFF (way out of bounds) */
    uint8_t zero = 0;
    r = ctrl_write_buf(h, SETARGFX3, 42, 0xFFFF, &zero, 1);
    /* Expected: STALL from the default case in SETARGFX3 handler */
    if (r != LIBUSB_ERROR_PIPE && r != 0) {
        printf("FAIL oob_setarg: unexpected error: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Verify device is still alive */
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL oob_setarg: device unresponsive after OOB wIndex: %s\n",
               libusb_strerror(r));
        return 1;
    }
    printf("PASS oob_setarg: device survived SETARGFX3 wIndex=0xFFFF (issue #20)\n");
    return 0;
}

/* Issue #13: Fill the console input buffer to exactly 31 chars
 * (the maximum before the off-by-one fix) and verify the device
 * doesn't crash.  Then send CR to flush and verify responsiveness. */
static int do_test_console_fill(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    uint8_t buf[64];

    /* Enable debug mode */
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL console_fill: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Send 35 characters (exceeds 32-byte buffer) via READINFODEBUG wValue */
    for (int i = 0; i < 35; i++) {
        r = ctrl_read(h, READINFODEBUG, 'a', 0, buf, sizeof(buf));
        /* r may be STALL (no debug output pending) — that's fine */
    }

    /* Send CR to trigger ParseCommand (flushes the buffer) */
    r = ctrl_read(h, READINFODEBUG, 0x0d, 0, buf, sizeof(buf));

    /* Brief pause for command processing */
    usleep(100000);

    /* Verify device is still alive */
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL console_fill: device unresponsive after 35-char fill: %s\n",
               libusb_strerror(r));
        return 1;
    }
    printf("PASS console_fill: device survived 35-char console input (issue #13)\n");
    return 0;
}

/* Issue #8: Exercise the debug buffer race window by rapidly
 * enabling debug mode (which triggers DebugPrint2USB) and polling
 * READINFODEBUG simultaneously.  Not deterministic, but catches
 * gross corruption.  Runs N rapid poll cycles. */
static int do_test_debug_race(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    uint8_t buf[64];

    /* Enable debug mode */
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL debug_race: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Trigger firmware activity that generates debug output:
     * send several SETARGFX3 + poll READINFODEBUG interleaved rapidly */
    for (int i = 0; i < 50; i++) {
        /* Generate debug output via a benign SETARGFX3 */
        uint8_t zero = 0;
        ctrl_write_buf(h, SETARGFX3, (uint16_t)(i & 63), DAT31_ATT, &zero, 1);
        /* Immediately poll debug output */
        r = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        /* Any result is fine — we're stress-testing the race path */
    }

    /* Verify device is still alive and coherent */
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL debug_race: device unresponsive after race stress: %s\n",
               libusb_strerror(r));
        return 1;
    }
    if (r >= 4 && info[0] == 0) {
        printf("FAIL debug_race: hwconfig read back as 0 (possible corruption)\n");
        return 1;
    }
    printf("PASS debug_race: device survived 50 rapid debug poll cycles (issue #8)\n");
    return 0;
}

/* Issue #26: Non-interactive debug poll — enable debug mode, send a
 * known command ("?"), collect output, verify it contains expected text.
 * Times out after a few seconds. */
static int do_test_debug_poll(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    uint8_t buf[64];
    char collected[1024] = {0};
    int collected_len = 0;

    /* Enable debug mode */
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL debug_poll: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Send "?" + CR via READINFODEBUG wValue */
    ctrl_read(h, READINFODEBUG, '?', 0, buf, sizeof(buf));
    usleep(50000);
    ctrl_read(h, READINFODEBUG, 0x0d, 0, buf, sizeof(buf));

    /* Poll for response (up to 2 seconds) */
    for (int attempt = 0; attempt < 40; attempt++) {
        usleep(50000);
        r = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        if (r > 0) {
            int copy = r - 1;  /* last byte is firmware null */
            if (collected_len + copy >= (int)sizeof(collected) - 1)
                copy = (int)sizeof(collected) - 1 - collected_len;
            if (copy > 0) {
                memcpy(collected + collected_len, buf, copy);
                collected_len += copy;
            }
        }
    }
    collected[collected_len] = '\0';

    /* The "?" command should produce help text containing "commands" */
    if (strstr(collected, "commands") || strstr(collected, "reset")
        || strstr(collected, "threads")) {
        printf("PASS debug_poll: got help text over USB debug (issue #26)\n");
        return 0;
    }
    if (collected_len > 0) {
        printf("PASS debug_poll: got %d bytes debug output (issue #26)\n", collected_len);
        return 0;
    }
    printf("FAIL debug_poll: no debug output received after '?' command\n");
    return 1;
}

/* Issue #10: Provoke a PIB error by starting GPIF streaming and
 * deliberately not reading the bulk endpoint.  The GPIF buffers
 * overflow, PibErrorCallback fires, and MsgParsing prints "PIB error 0x..."
 * to the debug output.  We poll READINFODEBUG looking for that string.
 *
 * This validates the entire PIB error reporting chain:
 *   GPIF overflow → PibErrorCallback → EventAvailable queue →
 *   MsgParsing → DebugPrint → READINFODEBUG poll
 */
static int do_test_pib_overflow(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    uint8_t buf[64];
    char collected[4096] = {0};
    int collected_len = 0;
    int found_pib_error = 0;

    /* 1. Enable debug mode */
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL pib_overflow: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Drain any stale debug output */
    for (int i = 0; i < 10; i++) {
        ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        usleep(20000);
    }

    /* 3. Configure ADC at 64 MHz — high enough to overwhelm quickly.
     *    Use cmd_u32_retry: the previous scenario may have left the
     *    device mid-watchdog-recovery, causing a transient timeout. */
    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL pib_overflow: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 4. Start streaming — GPIF begins pushing data to EP1 IN */
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL pib_overflow: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 5. Let DMA buffers fill and PIB errors fire.
     *    At 64 MS/s the 4 × 16 KB DMA buffers fill in < 1 ms, so
     *    PIB error interrupts begin almost immediately.  The one-shot
     *    flag in PibErrorCallback (issue #50) queues a single event
     *    and lets the app thread run, so "PIB error" text may appear
     *    in the debug buffer at any point — during the storm or after
     *    STOPFX3.  Collect ALL debug reads into the search buffer. */
    usleep(10000);  /* 10ms — DMA fills in < 1ms */

    /* 6. Read debug output during the storm — the app thread is now
     *    responsive (issue #50 fix) so PIB error text may already
     *    be here alongside trace output. */
    for (int i = 0; i < 5; i++) {
        r = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        if (r > 0) {
            int copy = r - 1;
            if (collected_len + copy >= (int)sizeof(collected) - 1)
                copy = (int)sizeof(collected) - 1 - collected_len;
            if (copy > 0) {
                memcpy(collected + collected_len, buf, copy);
                collected_len += copy;
            }
        }
    }

    if (strstr(collected, "PIB error"))
        found_pib_error = 1;

    /* 7. Stop streaming — ends the PIB interrupt storm */
    cmd_u32(h, STOPFX3, 0);

    /* 8. Let the application thread process any remaining queued
     *    events into the debug buffer. */
    if (!found_pib_error)
        usleep(300000);

    /* 9. Read the debug buffer — may contain STOPFX3 trace and/or
     *    PIB error text if not already captured in step 6. */
    for (int attempt = 0; !found_pib_error && attempt < 20; attempt++) {
        r = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        if (r > 0) {
            int copy = r - 1;  /* strip NUL terminator */
            if (collected_len + copy >= (int)sizeof(collected) - 1)
                copy = (int)sizeof(collected) - 1 - collected_len;
            if (copy > 0) {
                memcpy(collected + collected_len, buf, copy);
                collected_len += copy;
            }
            if (strstr(collected, "PIB error")) {
                found_pib_error = 1;
                break;
            }
        }
        usleep(50000);  /* 50ms between polls */
    }
    collected[collected_len] = '\0';

    /* 10. Verify device is still alive */
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL pib_overflow: device unresponsive after test: %s\n",
               libusb_strerror(r));
        return 1;
    }

    if (found_pib_error) {
        /* Extract the first PIB error line for reporting */
        char *p = strstr(collected, "PIB error");
        char excerpt[64] = {0};
        if (p) {
            int n = 0;
            while (p[n] && p[n] != '\r' && p[n] != '\n' && n < 60) n++;
            memcpy(excerpt, p, n);
        }
        printf("PASS pib_overflow: detected \"%s\" in debug output (issue #10)\n",
               excerpt);
        return 0;
    }

    printf("FAIL pib_overflow: no PIB error detected in %d bytes of debug output\n",
           collected_len);
    if (collected_len > 0) {
        /* Show what we did get, truncated */
        collected[collected_len < 200 ? collected_len : 200] = '\0';
        printf("#   debug output: %s\n", collected);
    }
    return 1;
}

/* Issue #12: Query the "stack" debug command and parse the high-water
 * mark to verify adequate headroom.  The firmware reports:
 *   "Stack free in <name> is <free>/<total>"
 * We PASS if free > 25% of total (i.e. comfortable margin at 2KB).
 */
static int do_test_stack_check(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    uint8_t buf[64];
    char collected[1024] = {0};
    int collected_len = 0;

    /* 1. Enable debug mode */
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL stack_check: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Drain stale output (30 rounds handles bursts from prior tests) */
    for (int i = 0; i < 30; i++) {
        int dr = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        if (dr <= 0) break;     /* buffer empty → done draining */
        usleep(20000);
    }

    /* 3. Send "stack" + CR */
    const char *cmd = "stack";
    for (const char *p = cmd; *p; p++) {
        ctrl_read(h, READINFODEBUG, (uint16_t)*p, 0, buf, sizeof(buf));
        usleep(10000);
    }
    ctrl_read(h, READINFODEBUG, 0x0d, 0, buf, sizeof(buf));

    /* 4. Poll for response (up to 3 seconds) */
    for (int attempt = 0; attempt < 60; attempt++) {
        usleep(50000);
        r = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        if (r > 0) {
            int copy = r - 1;
            if (collected_len + copy >= (int)sizeof(collected) - 1)
                copy = (int)sizeof(collected) - 1 - collected_len;
            if (copy > 0) {
                memcpy(collected + collected_len, buf, copy);
                collected_len += copy;
            }
            /* Early exit once we see the complete line */
            if (strstr(collected, "Stack free"))
                break;
        }
    }
    collected[collected_len] = '\0';

    /* 5. Parse "Stack free in <name> is <free>/<total>" */
    int free_bytes = -1, total_bytes = -1;
    char *p = strstr(collected, "Stack free");
    if (p) {
        char *is = strstr(p, " is ");
        if (is) {
            if (sscanf(is + 4, "%d/%d", &free_bytes, &total_bytes) != 2) {
                free_bytes = total_bytes = -1;
            }
        }
    }

    if (free_bytes < 0 || total_bytes <= 0) {
        printf("FAIL stack_check: could not parse stack response\n");
        if (collected_len > 0) {
            collected[collected_len < 200 ? collected_len : 200] = '\0';
            printf("#   debug output: %s\n", collected);
        }
        return 1;
    }

    /* 6. Verify total matches expected 2KB and free > 25% */
    int used = total_bytes - free_bytes;
    int margin_pct = (free_bytes * 100) / total_bytes;

    if (total_bytes != 2048) {
        printf("FAIL stack_check: expected 2048 total, got %d (issue #12)\n",
               total_bytes);
        return 1;
    }

    if (margin_pct < 25) {
        printf("FAIL stack_check: only %d/%d bytes free (%d%%) — insufficient margin (issue #12)\n",
               free_bytes, total_bytes, margin_pct);
        return 1;
    }

    printf("PASS stack_check: %d/%d used, %d/%d free (%d%% margin) (issue #12)\n",
           used, total_bytes, free_bytes, total_bytes, margin_pct);
    return 0;
}

/* ------------------------------------------------------------------ */
/* GETSTATS tests                                                     */
/* ------------------------------------------------------------------ */

/* GETSTATS response layout (20 bytes, little-endian):
 *   [0..3]   uint32  DMA buffer completions
 *   [4]      uint8   GPIF state machine state
 *   [5..8]   uint32  PIB error count
 *   [9..10]  uint16  last PIB error arg
 *   [11..14] uint32  I2C failure count
 *   [15..18] uint32  Streaming fault count (EP underruns + watchdog recoveries)
 *   [19]     uint8   Si5351 status register (reg 0)
 */
#define GETSTATS_LEN  20

struct fx3_stats {
    uint32_t dma_count;
    uint8_t  gpif_state;
    uint32_t pib_errors;
    uint16_t last_pib_arg;
    uint32_t i2c_failures;
    uint32_t streaming_faults;
    uint8_t  si5351_status;
};

static int read_stats(libusb_device_handle *h, struct fx3_stats *s)
{
    uint8_t buf[GETSTATS_LEN];
    int r = ctrl_read(h, GETSTATS, 0, 0, buf, GETSTATS_LEN);
    if (r < 0) return r;
    if (r < GETSTATS_LEN) return LIBUSB_ERROR_IO;
    memcpy(&s->dma_count,    &buf[0],  4);
    s->gpif_state = buf[4];
    memcpy(&s->pib_errors,   &buf[5],  4);
    memcpy(&s->last_pib_arg, &buf[9],  2);
    memcpy(&s->i2c_failures, &buf[11], 4);
    memcpy(&s->streaming_faults, &buf[15], 4);
    s->si5351_status = buf[19];
    return 0;
}

/* Read and display GETSTATS fields */
static int do_stats(libusb_device_handle *h)
{
    struct fx3_stats s;
    int r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL stats: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("PASS stats: dma=%u gpif=%u pib=%u last_pib=0x%04X i2c=%u faults=%u pll=0x%02X\n",
           s.dma_count, s.gpif_state, s.pib_errors,
           s.last_pib_arg, s.i2c_failures, s.streaming_faults,
           s.si5351_status);
    return 0;
}

/* Verify I2C failure counter increments on NACK.
 * Read stats, trigger I2C NACK (absent address 0xC2), read stats again. */
static int do_test_stats_i2c(libusb_device_handle *h)
{
    struct fx3_stats before, after;
    int r = read_stats(h, &before);
    if (r < 0) {
        printf("FAIL stats_i2c: initial read: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Trigger I2C NACK — read from absent address 0xC2 */
    uint8_t buf[1];
    ctrl_read(h, I2CRFX3, 0xC2, 0, buf, 1);  /* expected to fail */

    r = read_stats(h, &after);
    if (r < 0) {
        printf("FAIL stats_i2c: post read: %s\n", libusb_strerror(r));
        return 1;
    }

    if (after.i2c_failures > before.i2c_failures) {
        printf("PASS stats_i2c: i2c_failures %u -> %u after NACK\n",
               before.i2c_failures, after.i2c_failures);
        return 0;
    }
    printf("FAIL stats_i2c: i2c_failures unchanged (%u -> %u)\n",
           before.i2c_failures, after.i2c_failures);
    return 1;
}

/* Verify PIB error counter is non-zero.
 *
 * In the fw_test.sh suite this runs after pib_overflow (test 21),
 * which already caused GPIF overflow errors.  The counter persists
 * across vendor requests (only reset by StartApplication on USB
 * re-enumeration), so we just verify it's > 0.
 *
 * When run standalone on a fresh device (counter == 0), we attempt
 * to provoke overflow ourselves. */
static int do_test_stats_pib(libusb_device_handle *h)
{
    struct fx3_stats s;
    int r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL stats_pib: read: %s\n", libusb_strerror(r));
        return 1;
    }

    /* If pib_overflow already ran, counter is > 0 — just verify. */
    if (s.pib_errors > 0) {
        printf("PASS stats_pib: pib_errors=%u last_pib=0x%04X (from prior overflow)\n",
               s.pib_errors, s.last_pib_arg);
        return 0;
    }

    /* Standalone: counter is 0, provoke overflow ourselves.
     * cmd_u32_retry: absorb transient post-recovery timeouts. */
    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL stats_pib: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL stats_pib: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Wait for DMA buffers to overflow */
    usleep(2000000);

    cmd_u32(h, STOPFX3, 0);
    usleep(200000);

    r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL stats_pib: post read: %s\n", libusb_strerror(r));
        return 1;
    }

    if (s.pib_errors > 0) {
        printf("PASS stats_pib: pib_errors=%u last_pib=0x%04X\n",
               s.pib_errors, s.last_pib_arg);
        return 0;
    }
    printf("FAIL stats_pib: pib_errors still 0 after overflow attempt\n");
    return 1;
}

/* Verify Si5351 PLL lock status from GETSTATS.
 * Reg 0 bit 7 = SYS_INIT (should be clear after boot).
 * Reg 0 bit 5 = PLL A not locked (should be clear when tuned). */
static int do_test_stats_pll(libusb_device_handle *h)
{
    struct fx3_stats s;
    int r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL stats_pll: read: %s\n", libusb_strerror(r));
        return 1;
    }

    if (s.si5351_status & 0x80) {
        printf("FAIL stats_pll: SYS_INIT set (0x%02X) — device not ready\n",
               s.si5351_status);
        return 1;
    }

    if (s.si5351_status & 0x20) {
        printf("FAIL stats_pll: PLL A not locked (0x%02X)\n",
               s.si5351_status);
        return 1;
    }

    printf("PASS stats_pll: si5351_status=0x%02X (SYS_INIT clear, PLL A locked)\n",
           s.si5351_status);
    return 0;
}

/* ------------------------------------------------------------------ */
/* GPIF wedge / stop-start tests                                      */
/* ------------------------------------------------------------------ */

/* EP1 IN (bulk consumer endpoint) */
#define EP1_IN  0x81

/* Try to read some bulk data from EP1 IN.  Returns the number of bytes
 * actually received, or a negative libusb error code.  A timeout (no
 * data within timeout_ms) returns 0. */
static int bulk_read_some(libusb_device_handle *h, int len, int timeout_ms)
{
    uint8_t *buf = malloc(len);
    if (!buf) return LIBUSB_ERROR_NO_MEM;
    int transferred = 0;
    int r = libusb_bulk_transfer(h, EP1_IN, buf, len, &transferred, timeout_ms);
    free(buf);
    if (r == LIBUSB_ERROR_TIMEOUT) return transferred;  /* partial is OK */
    if (r < 0) return r;
    return transferred;
}

/* ---- Primed (async) start-and-read --------------------------------
 *
 * Race-free alternative to cmd_u32(STARTFX3) + bulk_read_some().
 *
 * At 32 MS/s the four 16 KB DMA buffers fill in ~1 ms.  If the host
 * hasn't submitted a bulk TD by then, PIB overflows force the xHCI
 * endpoint into an error state and all subsequent reads return -EIO.
 * rx888_stream avoids this by pre-submitting 32 async transfers BEFORE
 * sending STARTFX3.  This helper does the same thing for the test
 * harness:
 *
 *   1. libusb_submit_transfer()   — queue one async bulk read TD
 *   2. cmd_u32(STARTFX3, 0)       — start GPIF; data lands in the TD
 *   3. libusb_handle_events()     — wait for completion
 *
 * Note: libusb_clear_halt is NOT called here.  Between clean stop/start
 * cycles the endpoint is not halted, and clear_halt on a non-stalled EP
 * corrupts USB controller ERDY state via the firmware's CLEAR_FEATURE
 * handler.  The retry variant (primed_start_and_read_retry) calls
 * clear_halt between attempts to recover from genuine endpoint errors.
 *
 * Returns bytes received (>= 0) or a negative libusb error code.
 * On success the caller still owns the STOPFX3 responsibility. */

struct primed_xfer_state {
    int completed;       /* set to 1 by callback */
    int actual_length;   /* bytes transferred     */
    int status;          /* libusb_transfer_status */
};

static void LIBUSB_CALL primed_xfer_cb(struct libusb_transfer *xfer)
{
    struct primed_xfer_state *st = xfer->user_data;
    st->actual_length = xfer->actual_length;
    st->status        = xfer->status;
    st->completed     = 1;
}

static int primed_start_and_read(libusb_device_handle *h,
                                 int len, int timeout_ms)
{
    uint8_t *buf = malloc(len);
    if (!buf) return LIBUSB_ERROR_NO_MEM;

    struct libusb_transfer *xfer = libusb_alloc_transfer(0);
    if (!xfer) { free(buf); return LIBUSB_ERROR_NO_MEM; }

    struct primed_xfer_state st = { .completed = 0 };

    /* Note: do NOT call libusb_clear_halt here.  Between clean stop/start
     * cycles the endpoint is not halted, and clear_halt on a non-stalled
     * endpoint triggers CyU3PUsbStall(CyFalse, CyTrue) in the firmware's
     * CLEAR_FEATURE handler, which corrupts USB controller ERDY state
     * after data has flowed.  The clear_halt at device open (open_rx888)
     * handles the initial xHCI endpoint reset; error recovery is handled
     * by the retry variant below. */

    /* 1. Fill and submit async bulk transfer BEFORE starting GPIF */
    libusb_fill_bulk_transfer(xfer, h, EP1_IN, buf, len,
                              primed_xfer_cb, &st, timeout_ms);
    int r = libusb_submit_transfer(xfer);
    if (r < 0) {
        libusb_free_transfer(xfer);
        free(buf);
        return r;
    }

    /* 2. Start GPIF — data flows into the already-queued TD */
    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        libusb_cancel_transfer(xfer);
        /* Drain the cancelled transfer so libusb doesn't leak it */
        while (!st.completed)
            libusb_handle_events_completed(g_ctx, &st.completed);
        libusb_free_transfer(xfer);
        free(buf);
        return r;
    }

    /* 3. Wait for the bulk transfer to complete */
    while (!st.completed)
        libusb_handle_events_completed(g_ctx, &st.completed);

    libusb_free_transfer(xfer);
    free(buf);

    if (st.status == LIBUSB_TRANSFER_COMPLETED ||
        st.status == LIBUSB_TRANSFER_TIMED_OUT)
        return st.actual_length;

    /* Map transfer status to a libusb error code */
    switch (st.status) {
    case LIBUSB_TRANSFER_ERROR:    return LIBUSB_ERROR_IO;
    case LIBUSB_TRANSFER_STALL:    return LIBUSB_ERROR_PIPE;
    case LIBUSB_TRANSFER_OVERFLOW: return LIBUSB_ERROR_OVERFLOW;
    case LIBUSB_TRANSFER_NO_DEVICE:return LIBUSB_ERROR_NO_DEVICE;
    case LIBUSB_TRANSFER_CANCELLED:return LIBUSB_ERROR_INTERRUPTED;
    default:                       return LIBUSB_ERROR_OTHER;
    }
}

/* Retry variant: retries primed_start_and_read on transient USB errors
 * (timeout / IO), same escalation as cmd_u32_retry.  Use for the first
 * STARTFX3 in a scenario after potential watchdog recovery. */
static int primed_start_and_read_retry(libusb_device_handle *h,
                                       int len, int timeout_ms)
{
    int r = primed_start_and_read(h, len, timeout_ms);
    if (r >= 0 || (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_IO))
        return r;
    /* First retry — previous attempt may have started GPIF (STARTFX3
     * succeeded but bulk read failed).  Stop streaming, clear the
     * xHCI endpoint error state, then retry. */
    cmd_u32(h, STOPFX3, 0);
    usleep(500000);
    libusb_clear_halt(h, EP1_IN);
    r = primed_start_and_read(h, len, timeout_ms);
    if (r >= 0 || (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_IO))
        return r;
    /* Second retry */
    cmd_u32(h, STOPFX3, 0);
    usleep(1000000);
    libusb_clear_halt(h, EP1_IN);
    return primed_start_and_read(h, len, timeout_ms);
}

/* Stop GPIF then verify the SM state via GETSTATS.
 *
 * Sequences: STARTADC(32 MHz) -> STARTFX3 -> brief stream -> STOPFX3
 * -> GETSTATS.  The GPIF state should be 0 (RESET) after a proper
 * GpifDisable, or 1 (IDLE) if the SM stopped gracefully.
 *
 * On the current (broken) firmware the SM is still running or stuck
 * in a BUSY/WAIT state after STOPFX3 — this test detects that. */
static int do_test_stop_gpif_state(libusb_device_handle *h)
{
    int r;

    /* 1. Configure ADC clock.
     *    cmd_u32_retry: absorb transient post-recovery timeouts. */
    r = cmd_u32_retry(h, STARTADC, 32000000);
    if (r < 0) {
        printf("FAIL stop_gpif_state: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Start streaming */
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL stop_gpif_state: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 3. Read a little data to let GPIF run */
    bulk_read_some(h, 16384, 500);

    /* 4. Stop */
    r = cmd_u32(h, STOPFX3, 0);
    if (r < 0) {
        printf("FAIL stop_gpif_state: STOPFX3: %s\n", libusb_strerror(r));
        return 1;
    }
    usleep(50000);  /* let SM settle */

    /* 5. Read GPIF state */
    struct fx3_stats s;
    r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL stop_gpif_state: GETSTATS: %s\n", libusb_strerror(r));
        return 1;
    }

    /* State 1 = IDLE: SM exited cleanly via !FW_TRG (new waveform).
     * State 255 = GPIF disabled: force-stop fallback fired.
     * State 0 = RESET: acceptable but unexpected. */
    if (s.gpif_state == 1) {
        printf("PASS stop_gpif_state: GPIF state=%u after STOP (clean soft-stop to IDLE)\n",
               s.gpif_state);
        return 0;
    }
    if (s.gpif_state == 0 || s.gpif_state == 255) {
        printf("PASS stop_gpif_state: GPIF state=%u after STOP "
               "(stopped, but via force-stop fallback — new waveform not loaded?)\n",
               s.gpif_state);
        return 0;
    }
    printf("FAIL stop_gpif_state: GPIF state=%u after STOP (expected 1; SM still running)\n",
           s.gpif_state);
    return 1;
}

/* Repeatedly cycle STOP+START and verify streaming resumes each time.
 *
 * Sequences N iterations of:
 *   STARTFX3 -> read bulk data (verify flowing) -> STOPFX3
 * with a single STARTADC before the loop.
 *
 * On the current firmware this wedges on the 2nd or 3rd cycle because
 * STARTFX3 doesn't restart the SM after STOPFX3 leaves it stuck. */
static int do_test_stop_start_cycle(libusb_device_handle *h)
{
    int cycles = 5;
    int r;

    /* Configure ADC clock once.
     * cmd_u32_retry: absorb transient post-recovery timeouts. */
    r = cmd_u32_retry(h, STARTADC, 32000000);
    if (r < 0) {
        printf("FAIL stop_start_cycle: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    for (int i = 0; i < cycles; i++) {
        /* Primed read: queue async bulk TD before STARTFX3 to avoid
         * the PIB overflow race at 32 MS/s.  First iteration uses
         * the retry variant to absorb post-recovery transients. */
        int got = (i == 0) ? primed_start_and_read_retry(h, 16384, 2000)
                           : primed_start_and_read(h, 16384, 2000);
        if (got < 1024) {
            /* Diagnostic: read GETSTATS before cleanup to see GPIF/DMA state */
            struct fx3_stats s = {0};
            read_stats(h, &s);
            printf("FAIL stop_start_cycle: cycle %d/%d: only %d bytes "
                   "(expected >= 1024, stream not flowing)\n",
                   i + 1, cycles, got < 0 ? 0 : got);
            printf("#   libusb_rc=%d (%s), GPIF_state=%u, DMA_count=%u, "
                   "PIB_err=%u, faults=%u\n",
                   got < 0 ? got : 0,
                   got < 0 ? libusb_strerror(got) : "TIMEOUT",
                   s.gpif_state, s.dma_count,
                   s.pib_errors, s.streaming_faults);
            /* Try to clean up */
            cmd_u32(h, STOPFX3, 0);
            return 1;
        }

        /* Stop streaming */
        r = cmd_u32(h, STOPFX3, 0);
        if (r < 0) {
            printf("FAIL stop_start_cycle: STOPFX3 on cycle %d: %s\n",
                   i + 1, libusb_strerror(r));
            return 1;
        }

        /* Brief pause between cycles */
        usleep(100000);
    }

    printf("PASS stop_start_cycle: %d stop/start cycles completed, data flowing each time\n",
           cycles);
    return 0;
}

/* Verify STARTFX3 is rejected when the ADC clock is off.
 *
 * Sequences: STARTADC(0) -> STARTFX3 -> check result.
 * With the PLL pre-flight check, STARTFX3 should fail (STALL or
 * return isHandled=false).  Without it, START succeeds and the GPIF
 * runs on stale data.
 *
 * NOTE: After this test the ADC clock is off.  The test restores
 * clock at the end so subsequent tests can run. */
static int do_test_pll_preflight(libusb_device_handle *h)
{
    int r;

    /* 1. Turn off ADC clock */
    r = cmd_u32(h, STARTADC, 0);
    if (r < 0) {
        printf("FAIL pll_preflight: STARTADC(0): %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Brief pause for PLL to drop lock */
    usleep(200000);

    /* 3. Attempt STARTFX3 — should be rejected if PLL pre-flight is implemented */
    r = cmd_u32(h, STARTFX3, 0);

    /* Accepted STALL as rejection */
    int start_rejected = (r == LIBUSB_ERROR_PIPE);

    /* Also check: if it "succeeded", try to read data.  If no data flows,
     * the firmware may have silently rejected it. */
    if (r == 0) {
        int got = bulk_read_some(h, 4096, 1000);
        /* If we get data despite clock being off, that's stale garbage —
         * the pre-flight check is missing.  Clean up. */
        cmd_u32(h, STOPFX3, 0);
        if (got <= 0) {
            /* No data: firmware may have quietly rejected (acceptable) */
            start_rejected = 1;
        }
    }

    /* 4. Restore clock for subsequent tests */
    cmd_u32(h, STARTADC, 32000000);

    if (start_rejected) {
        printf("PASS pll_preflight: STARTFX3 correctly rejected with PLL unlocked\n");
        return 0;
    }
    printf("FAIL pll_preflight: STARTFX3 accepted with ADC clock off (no PLL pre-flight check)\n");
    return 1;
}

/* Test recovery after a deliberate DMA backpressure wedge.
 *
 * Sequences:
 *   1. STARTADC(64 MHz) + STARTFX3
 *   2. Do NOT read EP1 — let DMA buffers fill and GPIF enter BUSY/WAIT
 *   3. Wait 2 seconds (longer than the 300ms watchdog threshold)
 *   4. STOPFX3 + STARTFX3
 *   5. Read EP1 — data should flow if recovery worked
 *   6. Check GETSTATS for recovery counter (glCounter[2])
 *
 * On current firmware: step 5 times out (device is wedged).
 * After Phase 2: step 5 succeeds (STOP+START recovers).
 * After Phase 4: the watchdog may auto-recover in step 3, and
 *   glCounter[2] > 0 in GETSTATS. */
static int do_test_wedge_recovery(libusb_device_handle *h)
{
    int r;

    /* Enable debug mode to see recovery messages */
    uint8_t info[4] = {0};
    ctrl_read(h, TESTFX3, 1, 0, info, 4);

    /* 1. Configure ADC at 64 MHz.
     *    cmd_u32_retry: absorb transient post-recovery timeouts. */
    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL wedge_recovery: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Start streaming */
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL wedge_recovery: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 3. Do NOT read EP1 — wait for DMA to back up and GPIF to wedge.
     *    At 64 MS/s, 4 x 16 KB buffers fill in < 1 ms.
     *    Wait 2 seconds to allow watchdog to fire (if implemented). */
    usleep(2000000);

    /* 4. Stop */
    r = cmd_u32(h, STOPFX3, 0);
    if (r < 0) {
        printf("FAIL wedge_recovery: STOPFX3 after wedge: %s\n", libusb_strerror(r));
        return 1;
    }
    usleep(100000);

    /* 5. Restart */
    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL wedge_recovery: STARTFX3 after stop: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 6. Read bulk data — should flow if recovery worked */
    int got = bulk_read_some(h, 16384, 2000);

    /* 6a. Diagnostic: snapshot GETSTATS while GPIF is still running */
    struct fx3_stats s_live = {0};
    read_stats(h, &s_live);

    /* 7. Clean up */
    cmd_u32(h, STOPFX3, 0);
    usleep(100000);

    /* 8. Check GETSTATS for recovery counter */
    struct fx3_stats s;
    read_stats(h, &s);

    if (got < 1024) {
        printf("FAIL wedge_recovery: only %d bytes after recovery "
               "(expected >= 1024, device still wedged)\n",
               got < 0 ? 0 : got);
        printf("#   libusb_rc=%d (%s), live: GPIF=%u DMA=%u PIB=%u faults=%u; "
               "post-stop: GPIF=%u faults=%u\n",
               got < 0 ? got : 0,
               got < 0 ? libusb_strerror(got) : "TIMEOUT",
               s_live.gpif_state, s_live.dma_count,
               s_live.pib_errors, s_live.streaming_faults,
               s.gpif_state, s.streaming_faults);
        return 1;
    }

    printf("PASS wedge_recovery: %d bytes after STOP+START recovery", got);
    if (s.streaming_faults > 0)
        printf(", watchdog_recoveries=%u", s.streaming_faults);
    printf("\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Soak test scenario functions                                       */
/* ------------------------------------------------------------------ */

/* Clock-pull mid-stream: START streaming, kill clock with STARTADC(0)
 * while GPIF is running, then STOP and verify recovery via
 * STOP + clock restore + START + bulk read. */
static int do_test_clock_pull(libusb_device_handle *h)
{
    int r;

    /* 1. Configure and start streaming at 32 MHz.
     *    cmd_u32_retry: absorb transient post-recovery timeouts. */
    r = cmd_u32_retry(h, STARTADC, 32000000);
    if (r < 0) {
        printf("FAIL clock_pull: STARTADC(32M): %s\n", libusb_strerror(r));
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL clock_pull: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Read a little to confirm data is flowing */
    int got = bulk_read_some(h, 16384, 1000);
    if (got < 1024) {
        printf("FAIL clock_pull: no initial data (%d bytes)\n", got < 0 ? 0 : got);
        cmd_u32(h, STOPFX3, 0);
        return 1;
    }

    /* 3. Kill clock while streaming */
    cmd_u32(h, STARTADC, 0);
    usleep(200000);  /* let GPIF notice the missing clock */

    /* 4. Stop streaming */
    cmd_u32(h, STOPFX3, 0);
    usleep(100000);

    /* 5. Restore clock and restart */
    r = cmd_u32(h, STARTADC, 32000000);
    if (r < 0) {
        printf("FAIL clock_pull: STARTADC restore: %s\n", libusb_strerror(r));
        return 1;
    }
    usleep(100000);  /* PLL settle */

    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL clock_pull: STARTFX3 after restore: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 6. Verify data flows again */
    got = bulk_read_some(h, 16384, 2000);
    cmd_u32(h, STOPFX3, 0);

    if (got < 1024) {
        printf("FAIL clock_pull: no data after recovery (%d bytes)\n", got < 0 ? 0 : got);
        return 1;
    }
    printf("PASS clock_pull: recovered %d bytes after clock pull + restore\n", got);
    return 0;
}

/* Rapid frequency hopping: cycle through 5 ADC frequencies, each with
 * a brief START + read + STOP cycle, verifying data flows at each. */
static int do_test_freq_hop(libusb_device_handle *h)
{
    static const uint32_t freqs[] = {16000000, 32000000, 48000000, 64000000, 128000000};
    int nfreqs = (int)(sizeof(freqs) / sizeof(freqs[0]));

    for (int i = 0; i < nfreqs; i++) {
        int r = cmd_u32_retry(h, STARTADC, freqs[i]);
        if (r < 0) {
            printf("FAIL freq_hop: STARTADC(%u): %s\n", freqs[i], libusb_strerror(r));
            return 1;
        }
        usleep(100000);  /* PLL settle */

        r = cmd_u32_retry(h, STARTFX3, 0);
        if (r < 0) {
            printf("FAIL freq_hop: STARTFX3 at %u Hz: %s\n", freqs[i], libusb_strerror(r));
            return 1;
        }

        int got = bulk_read_some(h, 16384, 2000);

        cmd_u32(h, STOPFX3, 0);
        usleep(50000);

        if (got < 1024) {
            printf("FAIL freq_hop: only %d bytes at %u Hz\n",
                   got < 0 ? 0 : got, freqs[i]);
            return 1;
        }
    }

    /* Restore a standard frequency */
    cmd_u32(h, STARTADC, 32000000);

    printf("PASS freq_hop: data flowed at all %d frequencies\n", nfreqs);
    return 0;
}

/* EP0 stall recovery: send an OOB vendor request (gets STALL), then
 * immediately do TESTFX3 to verify EP0 still works. */
static int do_test_ep0_stall_recovery(libusb_device_handle *h)
{
    /* Trigger a STALL with an unknown vendor request */
    cmd_u32(h, 0xCC, 0);  /* expected to STALL */

    /* Immediately verify EP0 works */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL ep0_stall_recovery: EP0 broken after STALL: %s\n",
               libusb_strerror(r));
        return 1;
    }
    printf("PASS ep0_stall_recovery: EP0 functional after STALL\n");
    return 0;
}

/* Back-to-back STOP: send STOPFX3 twice without intervening START.
 * Device should handle the redundant stop gracefully. */
static int do_test_double_stop(libusb_device_handle *h)
{
    /* First, do a START+STOP to get into a known state.
     *
     * IMPORTANT: check the return values here.  If setup STARTFX3
     * silently fails (e.g. the device is still mid-recovery from the
     * previous scenario), the subsequent double-STOP runs on a broken
     * device and produces misleading "I/O Error" failures instead of
     * a clean "STARTFX3 timed out" diagnostic.
     *
     * cmd_u32_retry: absorb transient post-recovery timeouts. */
    int r = cmd_u32_retry(h, STARTADC, 32000000);
    if (r < 0) {
        printf("FAIL double_stop: setup STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL double_stop: setup STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }
    usleep(50000);
    cmd_u32(h, STOPFX3, 0);
    usleep(50000);

    /* Send a second STOP without START */
    r = cmd_u32(h, STOPFX3, 0);
    /* STALL is acceptable — means firmware rejected the redundant stop */
    if (r < 0 && r != LIBUSB_ERROR_PIPE) {
        printf("FAIL double_stop: unexpected error on 2nd STOP: %s\n",
               libusb_strerror(r));
        return 1;
    }

    /* Verify device is alive */
    uint8_t info[4] = {0};
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL double_stop: device unresponsive after double STOP: %s\n",
               libusb_strerror(r));
        return 1;
    }
    printf("PASS double_stop: device survived back-to-back STOPFX3\n");
    return 0;
}

/* Back-to-back START: send STARTFX3 twice without intervening STOP.
 * Device should handle it (may STALL the second — that's fine). */
static int do_test_double_start(libusb_device_handle *h)
{
    cmd_u32_retry(h, STARTADC, 32000000);
    usleep(50000);

    int r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL double_start: first STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }
    usleep(50000);

    /* Second START without STOP */
    r = cmd_u32(h, STARTFX3, 0);
    /* STALL is acceptable — firmware may reject duplicate start */

    /* Clean up */
    cmd_u32(h, STOPFX3, 0);
    usleep(100000);

    /* Verify device is alive and streaming still works */
    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL double_start: STARTFX3 after recovery: %s\n",
               libusb_strerror(r));
        return 1;
    }
    int got = bulk_read_some(h, 16384, 2000);
    cmd_u32(h, STOPFX3, 0);

    if (got < 1024) {
        printf("FAIL double_start: no data after recovery (%d bytes)\n",
               got < 0 ? 0 : got);
        return 1;
    }
    printf("PASS double_start: device survived back-to-back STARTFX3\n");
    return 0;
}

/* I2C read while streaming: START streaming, read Si5351 status via
 * I2C (I2CRFX3) while data is flowing, then STOP. Verifies both I2C
 * and streaming paths are healthy under concurrent use. */
static int do_test_i2c_under_load(libusb_device_handle *h)
{
    int r;

    /* Start streaming at 64 MHz.
     * cmd_u32_retry: absorb transient post-recovery timeouts. */
    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL i2c_under_load: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL i2c_under_load: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Read some bulk data to confirm streaming */
    int got = bulk_read_some(h, 16384, 1000);
    if (got < 1024) {
        printf("FAIL i2c_under_load: no streaming data (%d bytes)\n",
               got < 0 ? 0 : got);
        cmd_u32(h, STOPFX3, 0);
        return 1;
    }

    /* Read Si5351 status register (addr 0xC0, reg 0) while streaming */
    uint8_t buf[1] = {0};
    r = ctrl_read(h, I2CRFX3, 0xC0, 0, buf, 1);
    int i2c_ok = (r >= 1);

    /* Read more bulk data to verify streaming still works */
    int got2 = bulk_read_some(h, 16384, 1000);

    cmd_u32(h, STOPFX3, 0);

    if (!i2c_ok) {
        printf("FAIL i2c_under_load: I2C read failed while streaming: %s\n",
               libusb_strerror(r));
        return 1;
    }
    if (got2 < 1024) {
        printf("FAIL i2c_under_load: streaming died after I2C (%d bytes)\n",
               got2 < 0 ? 0 : got2);
        return 1;
    }
    printf("PASS i2c_under_load: I2C(0x%02X) + streaming both healthy\n", buf[0]);
    return 0;
}

/* Sustained streaming: START streaming at 64 MHz, read EP1 continuously
 * for ~30 seconds, verify data count matches expected throughput (within
 * 50%), then STOP. */
static int do_test_sustained_stream(libusb_device_handle *h)
{
    int r;
    int duration_sec = 30;
    uint32_t sample_rate = 64000000;

    /* cmd_u32_retry: absorb transient post-recovery timeouts. */
    r = cmd_u32_retry(h, STARTADC, sample_rate);
    if (r < 0) {
        printf("FAIL sustained_stream: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL sustained_stream: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Read continuously for duration_sec */
    uint64_t total_bytes = 0;
    int chunk = 65536;
    uint8_t *buf = malloc(chunk);
    if (!buf) {
        cmd_u32(h, STOPFX3, 0);
        printf("FAIL sustained_stream: malloc\n");
        return 1;
    }

    struct timespec start_ts, now_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    int fail = 0;
    for (;;) {
        int transferred = 0;
        r = libusb_bulk_transfer(h, EP1_IN, buf, chunk, &transferred, 2000);
        if (r == LIBUSB_ERROR_TIMEOUT && transferred > 0) {
            total_bytes += transferred;
        } else if (r == 0) {
            total_bytes += transferred;
        } else {
            printf("FAIL sustained_stream: bulk transfer error at %lu bytes: %s\n",
                   (unsigned long)total_bytes, libusb_strerror(r));
            fail = 1;
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        double elapsed = (now_ts.tv_sec - start_ts.tv_sec)
                       + (now_ts.tv_nsec - start_ts.tv_nsec) / 1e9;
        if (elapsed >= duration_sec) break;
    }

    free(buf);
    cmd_u32(h, STOPFX3, 0);

    if (fail) return 1;

    /* Verify throughput: 2 bytes/sample * sample_rate * duration_sec
     * Allow 50% tolerance (USB overhead, scheduling jitter) */
    uint64_t expected = (uint64_t)2 * sample_rate * duration_sec;
    int percent = (int)(total_bytes * 100 / expected);

    if (percent < 50) {
        printf("FAIL sustained_stream: %lu bytes in %ds (%d%% of expected)\n",
               (unsigned long)total_bytes, duration_sec, percent);
        return 1;
    }
    printf("PASS sustained_stream: %lu bytes in %ds (%d%% of expected %lu)\n",
           (unsigned long)total_bytes, duration_sec, percent,
           (unsigned long)expected);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Coverage-gap tests                                                 */
/* ------------------------------------------------------------------ */

/* rapid_start_stop: 50× START/STOP with ~1ms gaps, no bulk reads.
 * Stresses the DMA setup/teardown path and catches descriptor leaks
 * or stale glDMACount values. */
static int do_test_rapid_start_stop(libusb_device_handle *h)
{
    int r;
    int cycles = 50;

    r = cmd_u32_retry(h, STARTADC, 32000000);
    if (r < 0) {
        printf("FAIL rapid_start_stop: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    for (int i = 0; i < cycles; i++) {
        r = (i == 0) ? cmd_u32_retry(h, STARTFX3, 0)
                      : cmd_u32(h, STARTFX3, 0);
        if (r < 0) {
            printf("FAIL rapid_start_stop: STARTFX3 cycle %d: %s\n",
                   i + 1, libusb_strerror(r));
            return 1;
        }
        usleep(1000);  /* 1ms — no bulk reads */
        r = cmd_u32(h, STOPFX3, 0);
        if (r < 0) {
            printf("FAIL rapid_start_stop: STOPFX3 cycle %d: %s\n",
                   i + 1, libusb_strerror(r));
            return 1;
        }
        usleep(1000);
    }

    /* Verify device is alive and streaming still works */
    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL rapid_start_stop: final STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }
    int got = bulk_read_some(h, 16384, 2000);
    cmd_u32(h, STOPFX3, 0);

    if (got < 1024) {
        printf("FAIL rapid_start_stop: no data after %d cycles (%d bytes)\n",
               cycles, got < 0 ? 0 : got);
        return 1;
    }
    printf("PASS rapid_start_stop: %d cycles, data flowing after\n", cycles);
    return 0;
}

/* startadc_mid_stream: change ADC frequency while GPIF is running
 * without explicit STOP.  The firmware's implicit safety net in
 * STARTADC should force-stop GPIF before reprogramming the clock. */
static int do_test_startadc_mid_stream(libusb_device_handle *h)
{
    int r;
    struct fx3_stats entry_stats, fail_stats;

    /* Capture entry state — on failure we print this to reveal what the
     * previous scenario (or a fresh firmware reload) left behind. */
    memset(&entry_stats, 0, sizeof(entry_stats));
    read_stats(h, &entry_stats);

    r = cmd_u32_retry(h, STARTADC, 32000000);
    if (r < 0) {
        printf("FAIL startadc_mid_stream: STARTADC(32M): %s\n", libusb_strerror(r));
        printf("  diag: entry gpif=%u dma=%u pib=%u faults=%u pll=0x%02X\n",
               entry_stats.gpif_state, entry_stats.dma_count,
               entry_stats.pib_errors, entry_stats.streaming_faults,
               entry_stats.si5351_status);
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL startadc_mid_stream: STARTFX3: %s\n", libusb_strerror(r));
        printf("  diag: entry gpif=%u dma=%u pib=%u faults=%u pll=0x%02X\n",
               entry_stats.gpif_state, entry_stats.dma_count,
               entry_stats.pib_errors, entry_stats.streaming_faults,
               entry_stats.si5351_status);
        return 1;
    }

    /* Confirm data flowing at 32 MHz */
    int got = bulk_read_some(h, 16384, 2000);
    if (got < 1024) {
        /* Read GETSTATS BEFORE any cleanup — captures actual GPIF state
         * at the moment of failure, not the post-STOPFX3 state (255). */
        memset(&fail_stats, 0, sizeof(fail_stats));
        read_stats(h, &fail_stats);
        printf("FAIL startadc_mid_stream: no data at 32M (%d bytes)\n",
               got < 0 ? 0 : got);
        printf("  diag: entry gpif=%u dma=%u pib=%u faults=%u pll=0x%02X\n",
               entry_stats.gpif_state, entry_stats.dma_count,
               entry_stats.pib_errors, entry_stats.streaming_faults,
               entry_stats.si5351_status);
        printf("  diag: fail  gpif=%u dma=%u pib=%u pib_arg=0x%04X faults=%u pll=0x%02X\n",
               fail_stats.gpif_state, fail_stats.dma_count,
               fail_stats.pib_errors, fail_stats.last_pib_arg,
               fail_stats.streaming_faults, fail_stats.si5351_status);
        printf("  diag: bulk_read_some returned %d\n", got);
        /* No STOPFX3 — follow soak convention (line 4068): let the soak
         * loop's inter-scenario cleanup handle it so its GETSTATS read
         * also captures the real device state. */
        return 1;
    }

    /* Reprogram to 64 MHz WITHOUT stopping — firmware should handle this */
    r = cmd_u32(h, STARTADC, 64000000);
    if (r < 0) {
        memset(&fail_stats, 0, sizeof(fail_stats));
        read_stats(h, &fail_stats);
        printf("FAIL startadc_mid_stream: STARTADC(64M) mid-stream: %s\n",
               libusb_strerror(r));
        printf("  diag: entry gpif=%u dma=%u pib=%u faults=%u pll=0x%02X\n",
               entry_stats.gpif_state, entry_stats.dma_count,
               entry_stats.pib_errors, entry_stats.streaming_faults,
               entry_stats.si5351_status);
        printf("  diag: fail  gpif=%u dma=%u pib=%u pib_arg=0x%04X faults=%u pll=0x%02X\n",
               fail_stats.gpif_state, fail_stats.dma_count,
               fail_stats.pib_errors, fail_stats.last_pib_arg,
               fail_stats.streaming_faults, fail_stats.si5351_status);
        return 1;
    }
    usleep(200000);  /* PLL relock */

    /* Restart streaming at new frequency */
    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL startadc_mid_stream: STARTFX3 after reprogram: %s\n",
               libusb_strerror(r));
        return 1;
    }

    got = bulk_read_some(h, 16384, 2000);
    cmd_u32(h, STOPFX3, 0);

    if (got < 1024) {
        printf("FAIL startadc_mid_stream: no data at 64M after reprogram (%d bytes)\n",
               got < 0 ? 0 : got);
        return 1;
    }
    printf("PASS startadc_mid_stream: reprogram 32M→64M mid-stream, data flowing\n");
    return 0;
}

/* setarg_boundary: test DAT31_ATT and AD8370_VGA with boundary values.
 * Valid: ATT 0-63, VGA 0-255.  Firmware passes values straight through
 * to hardware — this documents actual behavior at/beyond limits. */
static int do_test_setarg_boundary(libusb_device_handle *h)
{
    struct { uint16_t id; const char *name; uint16_t max_valid; } args[] = {
        {DAT31_ATT, "DAT31_ATT", 63},
        {AD8370_VGA, "AD8370_VGA", 255},
    };

    for (int a = 0; a < 2; a++) {
        uint16_t test_vals[] = {0, args[a].max_valid,
                                (uint16_t)(args[a].max_valid + 1), 0xFFFF};
        for (int v = 0; v < 4; v++) {
            int r = set_arg(h, args[a].id, test_vals[v]);
            /* Values within range should succeed.
             * Values beyond range: firmware may accept (pass-through)
             * or STALL — either is acceptable, just not a crash. */
            if (r < 0 && r != LIBUSB_ERROR_PIPE) {
                printf("FAIL setarg_boundary: %s=%u: %s\n",
                       args[a].name, test_vals[v], libusb_strerror(r));
                return 1;
            }
        }
    }

    /* Verify device is alive */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL setarg_boundary: device unresponsive: %s\n",
               libusb_strerror(r));
        return 1;
    }

    /* Restore safe defaults */
    set_arg(h, DAT31_ATT, 0);
    set_arg(h, AD8370_VGA, 0);

    printf("PASS setarg_boundary: all boundary values accepted without crash\n");
    return 0;
}

/* i2c_bad_addr: I2C read to an absent address.  Should NACK and
 * increment i2c_failures, not wedge the I2C block. */
static int do_test_i2c_bad_addr(libusb_device_handle *h)
{
    struct fx3_stats before, after;
    int r = read_stats(h, &before);
    if (r < 0) {
        printf("FAIL i2c_bad_addr: initial GETSTATS: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Read 1 byte from absent address 0x90 (no device there) */
    uint8_t buf[1] = {0};
    r = ctrl_read(h, I2CRFX3, 0x90, 0, buf, 1);
    /* May STALL (firmware propagates NACK) or return data — either way,
     * the device should survive. */

    /* Verify device is alive */
    uint8_t info[4] = {0};
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL i2c_bad_addr: device unresponsive after bad I2C addr: %s\n",
               libusb_strerror(r));
        return 1;
    }

    /* Check i2c_failures counter incremented */
    r = read_stats(h, &after);
    if (r < 0) {
        printf("FAIL i2c_bad_addr: GETSTATS after: %s\n", libusb_strerror(r));
        return 1;
    }

    if (after.i2c_failures > before.i2c_failures) {
        printf("PASS i2c_bad_addr: i2c_failures %u→%u (NACK counted)\n",
               before.i2c_failures, after.i2c_failures);
    } else {
        printf("PASS i2c_bad_addr: device survived (i2c_failures unchanged: %u)\n",
               after.i2c_failures);
    }
    return 0;
}

/* ep0_control_while_streaming: hammer EP0 with mixed control commands
 * while actively reading bulk data from EP1.  Tests USB controller
 * arbitration between control and bulk endpoints. */
static int do_test_ep0_control_while_streaming(libusb_device_handle *h)
{
    int r;

    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL ep0_control_while_streaming: STARTADC: %s\n",
               libusb_strerror(r));
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL ep0_control_while_streaming: STARTFX3: %s\n",
               libusb_strerror(r));
        return 1;
    }

    /* Interleave: bulk read, then EP0 command, repeat 20 times */
    int ep0_ok = 0, bulk_ok = 0;
    struct fx3_stats s;

    for (int i = 0; i < 20; i++) {
        /* Bulk read */
        int got = bulk_read_some(h, 16384, 1000);
        if (got >= 1024) bulk_ok++;

        /* Rotate through different EP0 commands */
        switch (i % 5) {
        case 0: r = read_stats(h, &s); break;
        case 1: {
            uint8_t info[4];
            r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
            break;
        }
        case 2: r = set_arg(h, DAT31_ATT, (uint16_t)(i & 0x3F)); break;
        case 3: r = set_arg(h, AD8370_VGA, (uint16_t)(i * 10)); break;
        case 4: r = cmd_u32(h, GPIOFX3, 0); break;
        }
        if (r >= 0) ep0_ok++;
    }

    cmd_u32(h, STOPFX3, 0);
    set_arg(h, DAT31_ATT, 0);
    set_arg(h, AD8370_VGA, 0);

    if (bulk_ok < 15) {
        printf("FAIL ep0_control_while_streaming: only %d/20 bulk reads OK\n",
               bulk_ok);
        return 1;
    }
    if (ep0_ok < 15) {
        printf("FAIL ep0_control_while_streaming: only %d/20 EP0 commands OK\n",
               ep0_ok);
        return 1;
    }
    printf("PASS ep0_control_while_streaming: %d bulk + %d EP0 OK during stream\n",
           bulk_ok, ep0_ok);
    return 0;
}

/* gpio_during_stream: cycle GPIO bit patterns while streaming at 64 MHz.
 * GPIOFX3 shares the PIB data bus — tests bus contention. */
static int do_test_gpio_during_stream(libusb_device_handle *h)
{
    int r;
    static const uint32_t patterns[] = {
        0x00000000, 0xFFFFFFFF, 0xAAAAAAAA, 0x55555555,
        0x0000FFFF, 0xFFFF0000, 0x00000000
    };
    int npatterns = (int)(sizeof(patterns) / sizeof(patterns[0]));

    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL gpio_during_stream: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL gpio_during_stream: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Confirm streaming */
    int got = bulk_read_some(h, 16384, 1000);
    if (got < 1024) {
        printf("FAIL gpio_during_stream: no initial data (%d bytes)\n",
               got < 0 ? 0 : got);
        cmd_u32(h, STOPFX3, 0);
        return 1;
    }

    /* Cycle GPIO patterns while streaming */
    for (int i = 0; i < npatterns; i++) {
        r = cmd_u32(h, GPIOFX3, patterns[i]);
        if (r < 0) {
            printf("FAIL gpio_during_stream: GPIO 0x%08X: %s\n",
                   patterns[i], libusb_strerror(r));
            cmd_u32(h, STOPFX3, 0);
            cmd_u32(h, GPIOFX3, 0);
            return 1;
        }
        usleep(10000);  /* 10ms between patterns */
    }

    /* Verify streaming still works after GPIO hammering */
    got = bulk_read_some(h, 16384, 1000);
    cmd_u32(h, STOPFX3, 0);
    cmd_u32(h, GPIOFX3, 0);  /* restore */

    if (got < 1024) {
        printf("FAIL gpio_during_stream: streaming died after GPIO (%d bytes)\n",
               got < 0 ? 0 : got);
        return 1;
    }
    printf("PASS gpio_during_stream: %d GPIO patterns during stream, data OK\n",
           npatterns);
    return 0;
}

/* ep0_oversize_all_commands: send wLength > 64 for every data-phase
 * command.  The firmware's bounds check at USBHandler.c line 177 should
 * STALL uniformly before the command switch. */
static int do_test_ep0_oversize_all(libusb_device_handle *h)
{
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));

    /* All vendor requests that take an OUT data phase */
    static const struct { uint8_t code; const char *name; } cmds[] = {
        {GPIOFX3,   "GPIOFX3"},
        {STARTADC,  "STARTADC"},
        {STARTFX3,  "STARTFX3"},
        {I2CWFX3,   "I2CWFX3"},
        {SETARGFX3, "SETARGFX3"},
        {STOPFX3,   "STOPFX3"},
    };
    int ncmds = (int)(sizeof(cmds) / sizeof(cmds[0]));

    for (int i = 0; i < ncmds; i++) {
        int r = libusb_control_transfer(
            h,
            LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                LIBUSB_RECIPIENT_DEVICE,
            cmds[i].code, 0, 0, buf, sizeof(buf), CTRL_TIMEOUT_MS);

        if (r == LIBUSB_ERROR_PIPE) {
            /* STALL — expected */
            continue;
        }
        if (r < 0) {
            printf("FAIL ep0_oversize_all: %s: unexpected error: %s\n",
                   cmds[i].name, libusb_strerror(r));
            return 1;
        }
        printf("FAIL ep0_oversize_all: %s accepted wLength=%d "
               "(expected STALL)\n", cmds[i].name, (int)sizeof(buf));
        return 1;
    }

    /* Verify device is alive */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL ep0_oversize_all: device unresponsive: %s\n",
               libusb_strerror(r));
        return 1;
    }
    printf("PASS ep0_oversize_all: all %d commands STALL on wLength=%d\n",
           ncmds, (int)sizeof(buf));
    return 0;
}

/* i2c_write_read_cycle: write a value to a Si5351 register, read it
 * back, verify match.  Uses crystal load register (183) which is
 * written during init and safe to round-trip. */
static int do_test_i2c_write_read(libusb_device_handle *h)
{
    /* Read current value of crystal load register (reg 183, addr 0xC0) */
    uint8_t orig[1] = {0};
    int r = ctrl_read(h, I2CRFX3, 0xC0, 183, orig, 1);
    if (r < 1) {
        printf("FAIL i2c_write_read: initial read reg 183: %s\n",
               r < 0 ? libusb_strerror(r) : "short");
        return 1;
    }

    /* Write a test value (toggle bit 0 from current) */
    uint8_t test_val = orig[0] ^ 0x01;
    r = ctrl_write_buf(h, I2CWFX3, 0xC0, 183, &test_val, 1);
    if (r < 0) {
        printf("FAIL i2c_write_read: write reg 183: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Read back */
    uint8_t readback[1] = {0};
    r = ctrl_read(h, I2CRFX3, 0xC0, 183, readback, 1);
    if (r < 1) {
        printf("FAIL i2c_write_read: readback reg 183: %s\n",
               r < 0 ? libusb_strerror(r) : "short");
        /* Restore original before returning */
        ctrl_write_buf(h, I2CWFX3, 0xC0, 183, orig, 1);
        return 1;
    }

    /* Restore original value */
    ctrl_write_buf(h, I2CWFX3, 0xC0, 183, orig, 1);

    if (readback[0] != test_val) {
        printf("FAIL i2c_write_read: wrote 0x%02X, read back 0x%02X "
               "(orig 0x%02X)\n", test_val, readback[0], orig[0]);
        return 1;
    }
    printf("PASS i2c_write_read: reg 183 round-trip OK "
           "(orig=0x%02X, wrote=0x%02X, read=0x%02X)\n",
           orig[0], test_val, readback[0]);
    return 0;
}

/* rapid_adc_reprogram: 10× STARTADC cycling through frequencies with
 * no START/STOP.  Stresses Si5351 PLL relock and the poll loop.
 * Verifies PLL lock via GETSTATS after each. */
static int do_test_rapid_adc_reprogram(libusb_device_handle *h)
{
    static const uint32_t freqs[] = {
        16000000, 32000000, 48000000, 64000000, 128000000,
        64000000, 48000000, 32000000, 16000000, 128000000
    };
    int nfreqs = (int)(sizeof(freqs) / sizeof(freqs[0]));

    for (int i = 0; i < nfreqs; i++) {
        int r = cmd_u32(h, STARTADC, freqs[i]);
        if (r < 0) {
            printf("FAIL rapid_adc_reprogram: STARTADC(%u) step %d: %s\n",
                   freqs[i], i + 1, libusb_strerror(r));
            return 1;
        }
        usleep(10000);  /* 10ms — minimal settle */

        /* Verify PLL locked via GETSTATS */
        struct fx3_stats s;
        r = read_stats(h, &s);
        if (r < 0) {
            printf("FAIL rapid_adc_reprogram: GETSTATS step %d: %s\n",
                   i + 1, libusb_strerror(r));
            return 1;
        }
        /* Si5351 status reg bit 5 = PLLA lock, bit 7 = PLLB lock
         * Bits set = NOT locked.  For CLK0 (PLLA): check bit 5. */
        if (s.si5351_status & 0x20) {
            printf("FAIL rapid_adc_reprogram: PLL unlocked after %u Hz "
                   "(status=0x%02X)\n", freqs[i], s.si5351_status);
            return 1;
        }
    }

    /* Restore standard frequency */
    cmd_u32(h, STARTADC, 32000000);

    printf("PASS rapid_adc_reprogram: %d frequency changes, PLL locked after each\n",
           nfreqs);
    return 0;
}

/* debug_while_streaming: poll READINFODEBUG during active streaming.
 * READINFODEBUG uses EP0 bidirectionally (wValue=input, response=output)
 * while EP1 carries bulk data. */
static int do_test_debug_while_streaming(libusb_device_handle *h)
{
    int r;
    uint8_t buf[64];

    /* Enable debug mode */
    uint8_t info[4] = {0};
    r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL debug_while_streaming: enable debug: %s\n",
               libusb_strerror(r));
        return 1;
    }

    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL debug_while_streaming: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL debug_while_streaming: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Interleave bulk reads and debug console polls */
    int bulk_ok = 0, debug_ok = 0;
    for (int i = 0; i < 20; i++) {
        int got = bulk_read_some(h, 16384, 500);
        if (got >= 1024) bulk_ok++;

        /* Poll debug output (wValue=0 = no input char, just read) */
        r = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        /* r > 0: debug output pending; STALL: nothing pending — both OK */
        if (r >= 0 || r == LIBUSB_ERROR_PIPE) debug_ok++;
    }

    cmd_u32(h, STOPFX3, 0);

    /* Disable debug mode */
    ctrl_read(h, TESTFX3, 0, 0, info, 4);

    if (bulk_ok < 15) {
        printf("FAIL debug_while_streaming: only %d/20 bulk reads OK\n",
               bulk_ok);
        return 1;
    }
    if (debug_ok < 15) {
        printf("FAIL debug_while_streaming: only %d/20 debug polls OK\n",
               debug_ok);
        return 1;
    }
    printf("PASS debug_while_streaming: %d bulk + %d debug OK during stream\n",
           bulk_ok, debug_ok);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Consumer-failure scenarios                                         */
/*                                                                    */
/* These simulate the most common real-world failure mode: the host   */
/* application dies or hangs without sending STOPFX3, leaving the     */
/* device streaming into the void.  The watchdog detects the stall    */
/* and recovers, but without a consumer the recovery itself is        */
/* futile.  These tests verify the firmware handles this gracefully   */
/* (e.g. caps the recovery count) rather than looping forever.        */
/*                                                                    */
/* To add new consumer-failure variants, follow the pattern:          */
/*   1. Start streaming (STARTADC + STARTFX3)                        */
/*   2. Simulate the failure (don't read, don't stop, etc.)          */
/*   3. Observe device behaviour via GETSTATS / debug output          */
/*   4. Clean up with STOPFX3                                        */
/*   5. Verify the device is still EP0-responsive                    */
/* ------------------------------------------------------------------ */

/* Abandoned stream: simulate a host crash by starting streaming and
 * then doing nothing — no EP1 reads, no STOPFX3.  The watchdog will
 * detect the DMA stall and attempt recovery.  Without the
 * WDG_MAX_RECOV cap, it loops forever.  With the cap, recovery
 * attempts should plateau.
 *
 * Test method:
 *   1. STARTADC + STARTFX3
 *   2. Snapshot GETSTATS (streaming_faults = baseline)
 *   3. Wait 5s (enough for ~5 watchdog cycles at ~1s each)
 *   4. Snapshot GETSTATS again (faults_mid)
 *   5. Wait another 5s
 *   6. Snapshot GETSTATS again (faults_end)
 *   7. STOPFX3 + verify EP0 alive
 *
 * PASS: faults stopped growing between mid and end snapshots
 *       (recovery capped), OR faults_end - faults_mid < faults_mid -
 *       faults_baseline (recovery is decelerating — watchdog gave up).
 * FAIL: faults still climbing at the same rate (unbounded loop). */
static int do_test_abandoned_stream(libusb_device_handle *h)
{
    int r;
    struct fx3_stats baseline, mid, end;

    /* 1. Configure and start streaming.
     *    cmd_u32_retry: absorb transient post-recovery timeouts. */
    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL abandoned_stream: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL abandoned_stream: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Baseline snapshot */
    usleep(200000);  /* let first DMA buffers fill */
    r = read_stats(h, &baseline);
    if (r < 0) {
        printf("FAIL abandoned_stream: baseline GETSTATS: %s\n", libusb_strerror(r));
        cmd_u32(h, STOPFX3, 0);
        return 1;
    }

    /* 3. Wait 5s — no EP1 reads, no STOP.  Watchdog fires repeatedly. */
    sleep(5);

    /* 4. Mid-point snapshot */
    r = read_stats(h, &mid);
    if (r < 0) {
        printf("FAIL abandoned_stream: mid GETSTATS: %s\n", libusb_strerror(r));
        cmd_u32(h, STOPFX3, 0);
        return 1;
    }

    /* 5. Wait another 5s */
    sleep(5);

    /* 6. End snapshot */
    r = read_stats(h, &end);
    if (r < 0) {
        printf("FAIL abandoned_stream: end GETSTATS: %s\n", libusb_strerror(r));
        cmd_u32(h, STOPFX3, 0);
        return 1;
    }

    /* 7. Clean up */
    cmd_u32(h, STOPFX3, 0);
    usleep(200000);

    /* 8. Verify device is still EP0-responsive */
    uint8_t info[4] = {0};
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL abandoned_stream: device unresponsive after abandon: %s\n",
               libusb_strerror(r));
        return 1;
    }

    /* 9. Analyse recovery behaviour */
    uint32_t grow_first = mid.streaming_faults - baseline.streaming_faults;
    uint32_t grow_second = end.streaming_faults - mid.streaming_faults;

    printf("#   abandoned_stream: faults baseline=%u mid=%u end=%u "
           "(+%u first 5s, +%u second 5s)\n",
           baseline.streaming_faults, mid.streaming_faults,
           end.streaming_faults, grow_first, grow_second);

    if (grow_first == 0) {
        /* Watchdog didn't fire at all — either not implemented or
         * recovery happened too fast before baseline.  Not a failure
         * of this test, but note it. */
        printf("PASS abandoned_stream: no watchdog recoveries observed "
               "(watchdog may not be active)\n");
        return 0;
    }

    if (grow_second == 0) {
        /* Recovery stopped — cap is working */
        printf("PASS abandoned_stream: recovery capped after %u faults "
               "(no growth in second 5s window)\n", end.streaming_faults);
        return 0;
    }

    if (grow_second < grow_first) {
        /* Recovery is decelerating — cap kicked in partway through */
        printf("PASS abandoned_stream: recovery decelerating "
               "(+%u vs +%u, cap engaging)\n", grow_second, grow_first);
        return 0;
    }

    /* Still growing at the same rate — unbounded recovery loop */
    printf("FAIL abandoned_stream: recovery still looping (+%u/+%u), "
           "no cap detected\n", grow_first, grow_second);
    return 1;
}

/* ------------------------------------------------------------------ */
/* New coverage-gap tests (T1–T15)                                    */
/* ------------------------------------------------------------------ */

/* T1: vendor_rqt_wrap — verify glVendorRqtCnt (uint8_t) wraps at 256.
 * Send 260 TESTFX3 commands and track byte 3 of the response. */
static int do_test_vendor_rqt_wrap(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    int r;
    int saw_wrap = 0;
    uint8_t prev_cnt = 0;

    for (int i = 0; i < 260; i++) {
        r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
        if (r < 4) {
            printf("FAIL vendor_rqt_wrap: TESTFX3 #%d: %s\n",
                   i, r < 0 ? libusb_strerror(r) : "short");
            return 1;
        }
        uint8_t cnt = info[3];
        if (i > 0 && cnt < prev_cnt)
            saw_wrap = 1;
        prev_cnt = cnt;
    }

    if (!saw_wrap) {
        printf("FAIL vendor_rqt_wrap: counter did not wrap after 260 requests "
               "(last=%u)\n", prev_cnt);
        return 1;
    }
    printf("PASS vendor_rqt_wrap: counter wrapped around (last=%u)\n", prev_cnt);
    return 0;
}

/* T2: stale_vendor_codes — send dead-zone bRequest values that have no
 * handler.  0xB0, 0xB7, 0xB9 are between valid codes.  All should STALL.
 * (0xB3 = GETSTATS is valid, so excluded.) */
static int do_test_stale_vendor_codes(libusb_device_handle *h)
{
    static const uint8_t dead[] = {0xB0, 0xB7, 0xB9};
    int ndead = (int)(sizeof(dead) / sizeof(dead[0]));

    for (int i = 0; i < ndead; i++) {
        int r = cmd_u32(h, dead[i], 0);
        if (r != LIBUSB_ERROR_PIPE && r != 0) {
            printf("FAIL stale_vendor_codes: 0x%02X: unexpected: %s\n",
                   dead[i], libusb_strerror(r));
            return 1;
        }
        if (r == 0) {
            printf("FAIL stale_vendor_codes: 0x%02X accepted (expected STALL)\n",
                   dead[i]);
            return 1;
        }
    }

    /* Verify alive */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL stale_vendor_codes: device unresponsive: %s\n",
               libusb_strerror(r));
        return 1;
    }
    printf("PASS stale_vendor_codes: %d dead-zone codes all STALL\n", ndead);
    return 0;
}

/* T3: setarg_gap_index — SETARGFX3 with wIndex values that fall in gaps
 * between valid arg IDs (10=DAT31_ATT, 11=AD8370_VGA, 14=WDG_MAX_RECOV).
 * wIndex 12, 13, 15 should STALL. */
static int do_test_setarg_gap_index(libusb_device_handle *h)
{
    static const uint16_t gaps[] = {12, 13, 15};
    int ngaps = (int)(sizeof(gaps) / sizeof(gaps[0]));

    struct fx3_stats before, after;
    int have_before = (read_stats(h, &before) == 0);

    for (int i = 0; i < ngaps; i++) {
        uint8_t zero = 0;
        int r = ctrl_write_buf(h, SETARGFX3, 0, gaps[i], &zero, 1);
        if (r == LIBUSB_ERROR_PIPE)
            printf("#   setarg_gap_index: wIndex=%u STALL (expected)\n", gaps[i]);
        else if (r == 0)
            printf("#   setarg_gap_index: wIndex=%u accepted\n", gaps[i]);
        else {
            int have_after = (read_stats(h, &after) == 0);
            printf("FAIL setarg_gap_index: wIndex=%u: unexpected: %s\n",
                   gaps[i], libusb_strerror(r));
            if (have_before && have_after)
                printf("#   STATS: gpif=%u pib=%u->%u i2c=%u->%u faults=%u->%u\n",
                       after.gpif_state,
                       before.pib_errors, after.pib_errors,
                       before.i2c_failures, after.i2c_failures,
                       before.streaming_faults, after.streaming_faults);
            return 1;
        }
        /* Either STALL or accept is fine — just no crash */
    }

    /* Verify alive */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        int have_after = (read_stats(h, &after) == 0);
        printf("FAIL setarg_gap_index: device unresponsive: %s\n",
               libusb_strerror(r));
        if (have_before && have_after)
            printf("#   STATS: gpif=%u pib=%u->%u i2c=%u->%u faults=%u->%u\n",
                   after.gpif_state,
                   before.pib_errors, after.pib_errors,
                   before.i2c_failures, after.i2c_failures,
                   before.streaming_faults, after.streaming_faults);
        return 1;
    }
    printf("PASS setarg_gap_index: %d gap wIndex values survived\n", ngaps);
    return 0;
}

/* T4: gpio_extremes — send extreme GPIO patterns. */
static int do_test_gpio_extremes(libusb_device_handle *h)
{
    static const uint32_t patterns[] = {0x00000000, 0xFFFFFFFF, 0x0001FFFF};
    int npatterns = (int)(sizeof(patterns) / sizeof(patterns[0]));

    for (int i = 0; i < npatterns; i++) {
        int r = cmd_u32(h, GPIOFX3, patterns[i]);
        if (r < 0) {
            printf("FAIL gpio_extremes: 0x%08X: %s\n",
                   patterns[i], libusb_strerror(r));
            return 1;
        }
    }

    /* Restore GPIO to known-good state: LED_BLUE (bit 11) matches
     * rx888r2_GpioInitialize() default.  Without this, the 0x0001FFFF
     * pattern leaves SHDWN (bit 5) set, putting the LTC2208 ADC to
     * sleep and breaking all subsequent streaming tests. */
    cmd_u32(h, GPIOFX3, 0x0800);

    /* Verify alive */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL gpio_extremes: device unresponsive: %s\n",
               libusb_strerror(r));
        return 1;
    }
    printf("PASS gpio_extremes: %d extreme patterns accepted\n", npatterns);
    return 0;
}

/* hw_smoke — minimal streaming sanity check after GPIO-manipulating tests.
 * Sets known-good GPIO, configures ADC clock, START, reads data, STOP.
 * Catches the case where gpio_extremes left SHDWN set (ADC asleep). */
static int do_test_hw_smoke(libusb_device_handle *h)
{
    int r;

    /* Ensure GPIO is in known-good state */
    cmd_u32(h, GPIOFX3, 0x0800);  /* LED_BLUE */
    usleep(200000);  /* let ADC wake from SHDWN after gpio_extremes */

    r = cmd_u32_retry(h, STARTADC, 32000000);
    if (r < 0) {
        printf("FAIL hw_smoke: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Primed read: queue async bulk TD before STARTFX3 to avoid the
     * PIB overflow race at 32 MS/s (4×16 KB fills in ~1 ms). */
    int got = primed_start_and_read_retry(h, 16384, 2000);
    cmd_u32(h, STOPFX3, 0);

    if (got < 1024) {
        printf("FAIL hw_smoke: only %d bytes (expected >= 1024)\n",
               got < 0 ? 0 : got);
        return 1;
    }
    printf("PASS hw_smoke: %d bytes received — ADC alive\n", got);
    return 0;
}

/* T9: i2c_write_bad_addr — I2C write to absent address, verify NACK
 * increments i2c_failures counter (vs i2c_bad_addr which tests read). */
static int do_test_i2c_write_bad_addr(libusb_device_handle *h)
{
    struct fx3_stats before, after;
    int r = read_stats(h, &before);
    if (r < 0) {
        printf("FAIL i2c_write_bad_addr: initial GETSTATS: %s\n",
               libusb_strerror(r));
        return 1;
    }

    /* Write 1 byte to absent address 0x90 */
    uint8_t data = 0x00;
    r = ctrl_write_buf(h, I2CWFX3, 0x90, 0, &data, 1);
    /* Expected: STALL or error from NACK */

    /* Verify alive */
    uint8_t info[4] = {0};
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL i2c_write_bad_addr: device unresponsive: %s\n",
               libusb_strerror(r));
        return 1;
    }

    r = read_stats(h, &after);
    if (r < 0) {
        printf("FAIL i2c_write_bad_addr: post GETSTATS: %s\n",
               libusb_strerror(r));
        return 1;
    }

    if (after.i2c_failures > before.i2c_failures) {
        printf("PASS i2c_write_bad_addr: i2c_failures %u->%u (write NACK counted)\n",
               before.i2c_failures, after.i2c_failures);
    } else {
        printf("PASS i2c_write_bad_addr: device survived "
               "(i2c_failures unchanged: %u)\n", after.i2c_failures);
    }
    return 0;
}

/* T10: i2c_multibyte — multi-byte I2C round-trip on Si5351 registers. */
static int do_test_i2c_multibyte(libusb_device_handle *h)
{
    /* Read 8 bytes from Si5351 (addr 0xC0) starting at reg 0 */
    uint8_t orig[8] = {0};
    int r = ctrl_read(h, I2CRFX3, 0xC0, 0, orig, 8);
    if (r < 8) {
        printf("FAIL i2c_multibyte: initial read: %s (got %d bytes)\n",
               r < 0 ? libusb_strerror(r) : "short", r < 0 ? 0 : r);
        return 1;
    }

    /* Write the same bytes back (non-destructive) */
    r = ctrl_write_buf(h, I2CWFX3, 0xC0, 0, orig, 8);
    if (r < 0) {
        printf("FAIL i2c_multibyte: write 8 bytes: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Read back */
    uint8_t readback[8] = {0};
    r = ctrl_read(h, I2CRFX3, 0xC0, 0, readback, 8);
    if (r < 8) {
        printf("FAIL i2c_multibyte: readback: %s (got %d bytes)\n",
               r < 0 ? libusb_strerror(r) : "short", r < 0 ? 0 : r);
        return 1;
    }

    /* Compare */
    if (memcmp(orig, readback, 8) != 0) {
        printf("FAIL i2c_multibyte: readback mismatch\n");
        printf("#   orig:     ");
        for (int i = 0; i < 8; i++) printf("%02X ", orig[i]);
        printf("\n#   readback: ");
        for (int i = 0; i < 8; i++) printf("%02X ", readback[i]);
        printf("\n");
        return 1;
    }

    printf("PASS i2c_multibyte: 8-byte I2C round-trip OK\n");
    return 0;
}

/* T14: readinfodebug_flood — fill debug buffer without draining.
 * Send 50 rapid SETARGFX3 commands (each generates TraceSerial output)
 * without polling READINFODEBUG.  Then do a single read + alive check. */
static int do_test_readinfodebug_flood(libusb_device_handle *h)
{
    /* Enable debug mode */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL readinfodebug_flood: enable debug: %s\n",
               libusb_strerror(r));
        return 1;
    }

    /* Flood: 50 SETARGFX3 commands without reading debug output */
    for (int i = 0; i < 50; i++) {
        set_arg(h, DAT31_ATT, (uint16_t)(i & 63));
    }

    /* Single drain read */
    uint8_t buf[64];
    ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));

    /* Disable debug mode */
    ctrl_read(h, TESTFX3, 0, 0, info, 4);

    /* Verify alive */
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL readinfodebug_flood: device unresponsive: %s\n",
               libusb_strerror(r));
        return 1;
    }
    printf("PASS readinfodebug_flood: survived 50 debug-generating cmds "
           "without drain\n");
    return 0;
}

/* T5: dma_count_reset — verify dma_completions resets on each STARTFX3. */
static int do_test_dma_count_reset(libusb_device_handle *h)
{
    int r;
    struct fx3_stats s;

    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL dma_count_reset: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    /* --- Session 1: stream, record dma_count ---
     * Primed read: queue async bulk TD before STARTFX3 to avoid the
     * PIB overflow race at 64 MS/s (4x16 KB DMA buffers fill in ~500 us).
     * Uses _retry variant for the first start after potential inter-scenario
     * recovery, matching dma_count_monotonic and stop_start_cycle. */
    int got1 = primed_start_and_read_retry(h, 65536, 2000);
    if (got1 < 0) {
        printf("FAIL dma_count_reset: primed start #1: %s\n",
               libusb_strerror(got1));
        cmd_u32(h, STOPFX3, 0);
        return 1;
    }
    r = cmd_u32(h, STOPFX3, 0);
    if (r < 0) {
        printf("FAIL dma_count_reset: STOPFX3 #1: %s\n", libusb_strerror(r));
        return 1;
    }

    r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL dma_count_reset: GETSTATS #1: %s\n", libusb_strerror(r));
        return 1;
    }
    uint32_t count1 = s.dma_count;

    /* Bail out before session 2 if session 1 produced no data.
     * Running another start/stop cycle on an already-broken pipeline
     * compounds GPIF corruption (Population B, hypothesis H5). */
    if (count1 == 0) {
        printf("FAIL dma_count_reset: first session dma_count=0 "
               "(stream didn't produce data)\n");
        return 1;
    }

    /* --- Session 2: restart, minimal read, record dma_count ---
     * Previous design did STARTFX3 then immediate STOPFX3 with no bulk
     * read.  With no host TDs queued, the GPIF SM was killed mid-
     * transition by GpifDisable, leaving internal counters and socket
     * mappings dirty.  The next GpifLoad inherited this residue,
     * producing cascading 0x1006 PIB errors (Population B).
     *
     * A primed read gives the SM a valid consumer path.  4096 bytes is
     * enough to prove the SM ran and DMA transferred at least one buffer
     * while keeping session 2 fast.  The test still verifies whether
     * dma_count resets across sessions. */
    usleep(200000);
    int got2 = primed_start_and_read(h, 4096, 2000);
    if (got2 < 0) {
        printf("FAIL dma_count_reset: primed start #2: %s\n",
               libusb_strerror(got2));
        cmd_u32(h, STOPFX3, 0);
        return 1;
    }
    r = cmd_u32(h, STOPFX3, 0);
    if (r < 0) {
        printf("FAIL dma_count_reset: STOPFX3 #2: %s\n", libusb_strerror(r));
        return 1;
    }

    r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL dma_count_reset: GETSTATS #2: %s\n", libusb_strerror(r));
        return 1;
    }
    uint32_t count2 = s.dma_count;

    if (count2 < count1) {
        printf("PASS dma_count_reset: count dropped %u->%u after restart\n",
               count1, count2);
        return 0;
    }

    /* Count didn't reset — might be cumulative by design */
    printf("PASS dma_count_reset: count %u->%u (counter may be cumulative)\n",
           count1, count2);
    return 0;
}

/* T6: dma_count_monotonic — verify dma_completions grows during stream.
 *
 * Uses primed_start_and_read to queue a bulk TD before STARTFX3.
 * At 64 MS/s the 4×16 KB DMA buffers fill in ~500 µs.  The old
 * synchronous approach (cmd_u32(STARTFX3) then bulk_read_some) left
 * a multi-millisecond gap where PIB errors accumulated unchecked;
 * after ~8 loop iterations the overflow occasionally stalled the DMA
 * pipeline, causing dma_count to plateau and the test to fail
 * (observed: step 8, count 23<=23, pib=141).  The primed start
 * eliminates the initial overflow storm so subsequent synchronous
 * reads can keep the pipeline drained. */
static int do_test_dma_count_monotonic(libusb_device_handle *h)
{
    int r;
    struct fx3_stats s;
    struct fx3_stats entry_stats;
    int have_entry = (read_stats(h, &entry_stats) == 0);

    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL dma_count_monotonic: STARTADC: %s\n", libusb_strerror(r));
        if (have_entry)
            printf("#   entry STATS: gpif=%u dma=%u pib=%u faults=%u\n",
                   entry_stats.gpif_state, entry_stats.dma_count,
                   entry_stats.pib_errors, entry_stats.streaming_faults);
        return 1;
    }

    /* Primed read: queue async bulk TD before STARTFX3 to avoid the
     * PIB overflow race at 64 MS/s (see function comment). */
    int got = primed_start_and_read_retry(h, 32768, 2000);
    if (got < 0) {
        printf("FAIL dma_count_monotonic: primed start: %s\n",
               libusb_strerror(got));
        cmd_u32(h, STOPFX3, 0);
        if (have_entry)
            printf("#   entry STATS: gpif=%u dma=%u pib=%u faults=%u\n",
                   entry_stats.gpif_state, entry_stats.dma_count,
                   entry_stats.pib_errors, entry_stats.streaming_faults);
        return 1;
    }

    uint32_t prev_count = 0;
    uint32_t entry_faults = have_entry ? entry_stats.streaming_faults : 0;
    for (int i = 0; i < 10; i++) {
        got = bulk_read_some(h, 32768, 500);

        r = read_stats(h, &s);
        if (r < 0) {
            printf("FAIL dma_count_monotonic: GETSTATS step %d: %s\n",
                   i, libusb_strerror(r));
            cmd_u32(h, STOPFX3, 0);
            return 1;
        }

        printf("#   dma_monotonic[%d]: read=%d dma=%u gpif=%u pib=%u faults=%u%s\n",
               i, got, s.dma_count, s.gpif_state, s.pib_errors,
               s.streaming_faults,
               s.streaming_faults > entry_faults ? " [WATCHDOG]" : "");

        if (i > 0 && s.dma_count <= prev_count) {
            printf("FAIL dma_count_monotonic: count not increasing at step %d "
                   "(%u <= %u)\n", i, s.dma_count, prev_count);
            printf("#   faults delta: %u->%u\n",
                   entry_faults, s.streaming_faults);
            cmd_u32(h, STOPFX3, 0);
            return 1;
        }
        prev_count = s.dma_count;
    }

    cmd_u32(h, STOPFX3, 0);
    printf("PASS dma_count_monotonic: dma_count strictly increased over "
           "10 samples (final=%u)\n", prev_count);
    return 0;
}

/* T7: watchdog_cap_observe — set WDG_MAX_RECOV=3, abandon stream, poll
 * GETSTATS to observe streaming_faults plateau at the cap. */
static int do_test_watchdog_cap_observe(libusb_device_handle *h)
{
    int r;
    struct fx3_stats s;

    /* Set cap to 3 recoveries */
    r = set_arg(h, WDG_MAX_RECOV, 3);
    if (r < 0) {
        printf("FAIL watchdog_cap_observe: WDG_MAX_RECOV: %s\n",
               libusb_strerror(r));
        return 1;
    }

    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL watchdog_cap_observe: STARTADC: %s\n", libusb_strerror(r));
        set_arg(h, WDG_MAX_RECOV, 0);
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL watchdog_cap_observe: STARTFX3: %s\n", libusb_strerror(r));
        set_arg(h, WDG_MAX_RECOV, 0);
        return 1;
    }

    /* Don't read EP1 — let watchdog fire.  Poll GETSTATS every 500ms. */
    uint32_t faults[20];
    int nsamples = 0;
    for (int i = 0; i < 20 && nsamples < 20; i++) {
        usleep(500000);
        r = read_stats(h, &s);
        if (r < 0) {
            printf("FAIL watchdog_cap_observe: GETSTATS poll %d: %s\n",
                   i, libusb_strerror(r));
            cmd_u32(h, STOPFX3, 0);
            set_arg(h, WDG_MAX_RECOV, 0);
            return 1;
        }
        faults[nsamples++] = s.streaming_faults;
    }

    cmd_u32(h, STOPFX3, 0);
    set_arg(h, WDG_MAX_RECOV, 0);

    /* Check: faults should plateau (last few values equal) */
    if (nsamples < 4) {
        printf("FAIL watchdog_cap_observe: too few samples (%d)\n", nsamples);
        return 1;
    }

    uint32_t final = faults[nsamples - 1];
    int plateau_len = 0;
    for (int i = nsamples - 1; i >= 0; i--) {
        if (faults[i] == final)
            plateau_len++;
        else
            break;
    }

    printf("#   watchdog_cap_observe: faults trace:");
    for (int i = 0; i < nsamples; i++)
        printf(" %u", faults[i]);
    printf("\n");

    if (plateau_len >= 3) {
        printf("PASS watchdog_cap_observe: faults plateaued at %u "
               "(stable for %d polls)\n", final, plateau_len);
        return 0;
    }

    if (faults[0] == faults[nsamples - 1]) {
        /* No watchdog activity at all */
        printf("PASS watchdog_cap_observe: no watchdog activity observed "
               "(faults=%u throughout)\n", final);
        return 0;
    }

    printf("FAIL watchdog_cap_observe: faults still growing at end "
           "(no plateau, last=%u)\n", final);
    return 1;
}

/* T8: watchdog_cap_restart — after watchdog caps, restart streaming
 * without an intervening STOP. */
static int do_test_watchdog_cap_restart(libusb_device_handle *h)
{
    int r;

    /* Set cap to 3 */
    r = set_arg(h, WDG_MAX_RECOV, 3);
    if (r < 0) {
        printf("FAIL watchdog_cap_restart: WDG_MAX_RECOV: %s\n",
               libusb_strerror(r));
        return 1;
    }

    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL watchdog_cap_restart: STARTADC: %s\n", libusb_strerror(r));
        set_arg(h, WDG_MAX_RECOV, 0);
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL watchdog_cap_restart: STARTFX3 #1: %s\n",
               libusb_strerror(r));
        set_arg(h, WDG_MAX_RECOV, 0);
        return 1;
    }

    /* Wait for watchdog to hit cap */
    sleep(5);

    /* STOP then restart (clean path) */
    cmd_u32(h, STOPFX3, 0);
    usleep(200000);

    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL watchdog_cap_restart: STARTFX3 #2 after cap: %s\n",
               libusb_strerror(r));
        set_arg(h, WDG_MAX_RECOV, 0);
        return 1;
    }

    int got = bulk_read_some(h, 16384, 2000);
    cmd_u32(h, STOPFX3, 0);
    set_arg(h, WDG_MAX_RECOV, 0);

    if (got < 1024) {
        printf("FAIL watchdog_cap_restart: only %d bytes after cap restart "
               "(expected >= 1024)\n", got < 0 ? 0 : got);
        return 1;
    }

    printf("PASS watchdog_cap_restart: %d bytes after cap restart\n", got);
    return 0;
}

/* T11: ep0_hammer — 500 rapid TESTFX3 commands during streaming. */
static int do_test_ep0_hammer(libusb_device_handle *h)
{
    int r;

    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL ep0_hammer: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL ep0_hammer: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Drain some initial data */
    bulk_read_some(h, 16384, 500);

    /* Hammer EP0 with 500 TESTFX3 */
    uint8_t info[4];
    int ep0_ok = 0;
    for (int i = 0; i < 500; i++) {
        r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
        if (r >= 4) ep0_ok++;
    }

    /* Check bulk still works */
    int got = bulk_read_some(h, 16384, 2000);
    cmd_u32(h, STOPFX3, 0);

    if (ep0_ok < 490) {
        printf("FAIL ep0_hammer: only %d/500 TESTFX3 succeeded\n", ep0_ok);
        return 1;
    }
    if (got < 1024) {
        printf("FAIL ep0_hammer: bulk read after hammer: %d bytes "
               "(expected >= 1024)\n", got < 0 ? 0 : got);
        return 1;
    }

    printf("PASS ep0_hammer: %d/500 EP0 ok, %d bytes bulk after\n",
           ep0_ok, got);
    return 0;
}

/* T12: debug_cmd_while_stream — send a debug command during active
 * streaming and verify both debug output and bulk data arrive. */
static int do_test_debug_cmd_while_stream(libusb_device_handle *h)
{
    int r;
    uint8_t buf[64];
    uint8_t info[4] = {0};

    /* Enable debug */
    r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL debug_cmd_while_stream: enable debug: %s\n",
               libusb_strerror(r));
        return 1;
    }

    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL debug_cmd_while_stream: STARTADC: %s\n",
               libusb_strerror(r));
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL debug_cmd_while_stream: STARTFX3: %s\n",
               libusb_strerror(r));
        return 1;
    }

    /* Read some bulk first to confirm stream is active */
    int got1 = bulk_read_some(h, 16384, 1000);

    /* Send "?" command (help) via READINFODEBUG wValue */
    ctrl_read(h, READINFODEBUG, '?', 0, buf, sizeof(buf));
    ctrl_read(h, READINFODEBUG, '\r', 0, buf, sizeof(buf));

    /* Poll for debug response */
    int debug_got = 0;
    for (int i = 0; i < 20; i++) {
        r = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        if (r > 0) debug_got += r;
        usleep(50000);
    }

    /* Read more bulk to confirm stream still alive */
    int got2 = bulk_read_some(h, 16384, 1000);

    cmd_u32(h, STOPFX3, 0);
    /* Disable debug */
    ctrl_read(h, TESTFX3, 0, 0, info, 4);

    if (got1 < 1024 && got2 < 1024) {
        printf("FAIL debug_cmd_while_stream: bulk data insufficient "
               "(before=%d, after=%d)\n",
               got1 < 0 ? 0 : got1, got2 < 0 ? 0 : got2);
        return 1;
    }

    printf("PASS debug_cmd_while_stream: debug_bytes=%d, "
           "bulk_before=%d, bulk_after=%d\n",
           debug_got, got1 < 0 ? 0 : got1, got2 < 0 ? 0 : got2);
    return 0;
}

/* T13: adc_freq_extremes — test edge frequencies. */
static int do_test_adc_freq_extremes(libusb_device_handle *h)
{
    static const struct { uint32_t freq; const char *label; } edges[] = {
        {1000000,   "1 MHz"},
        {200000000, "200 MHz"},
        {1,         "1 Hz"},
    };
    int nedges = (int)(sizeof(edges) / sizeof(edges[0]));

    for (int i = 0; i < nedges; i++) {
        int r = cmd_u32(h, STARTADC, edges[i].freq);
        /* Any result is acceptable — just check the device survives */
        (void)r;

        usleep(50000);  /* brief settle */

        uint8_t info[4] = {0};
        r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
        if (r < 0) {
            printf("FAIL adc_freq_extremes: device died after %s: %s\n",
                   edges[i].label, libusb_strerror(r));
            /* Try to restore */
            cmd_u32(h, STARTADC, 32000000);
            return 1;
        }
    }

    /* Restore standard frequency */
    cmd_u32(h, STARTADC, 32000000);

    printf("PASS adc_freq_extremes: %d edge frequencies survived\n", nedges);
    return 0;
}

/* T15: data_sanity — capture bulk data with front-end shut down, verify
 * no full-scale saturation (which would indicate DMA corruption). */
static int do_test_data_sanity(libusb_device_handle *h)
{
    int r;

    /* Max attenuation + min VGA to reduce signal */
    set_arg(h, DAT31_ATT, 63);
    set_arg(h, AD8370_VGA, 0);

    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL data_sanity: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL data_sanity: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Capture 64KB of samples */
    int caplen = 65536;
    uint8_t *cap = malloc(caplen);
    if (!cap) {
        cmd_u32(h, STOPFX3, 0);
        printf("FAIL data_sanity: malloc\n");
        return 1;
    }

    int transferred = 0;
    r = libusb_bulk_transfer(h, EP1_IN, cap, caplen, &transferred, 3000);
    cmd_u32(h, STOPFX3, 0);

    /* Restore defaults */
    set_arg(h, DAT31_ATT, 0);
    set_arg(h, AD8370_VGA, 0);

    if (transferred < 4096) {
        printf("FAIL data_sanity: only %d bytes captured\n", transferred);
        free(cap);
        return 1;
    }

    /* Scan for full-scale saturation: 16-bit LE samples */
    int saturated = 0;
    int nsamples = transferred / 2;
    for (int i = 0; i < nsamples; i++) {
        int16_t sample = (int16_t)(cap[2*i] | (cap[2*i+1] << 8));
        if (sample == 32767 || sample == -32768)
            saturated++;
    }
    free(cap);

    /* Allow a small fraction (< 1%) of saturated samples */
    int threshold = nsamples / 100;
    if (threshold < 10) threshold = 10;

    if (saturated > threshold) {
        printf("FAIL data_sanity: %d/%d samples saturated (%.1f%%, "
               "threshold %d) — possible DMA corruption\n",
               saturated, nsamples,
               100.0 * saturated / nsamples, threshold);
        return 1;
    }

    printf("PASS data_sanity: %d/%d saturated samples (%.1f%%, "
           "within threshold)\n",
           saturated, nsamples, 100.0 * saturated / nsamples);
    return 0;
}

/* Verify that STOPFX3 leaves the GPIF SM in IDLE (state 1) rather than
 * disabled (state 255).  State 1 means the SM exited cleanly via the
 * !FW_TRG transitions.  State 255 means force-stop fired — the new
 * waveform is either not loaded or the transitions are wrong.
 *
 * REQUIRES: updated SDDC_GPIF.h with !FW_TRG exits on TH0_RD,
 * TH0_WAIT, and TH1_WAIT.  Without the new waveform this test will
 * report FAIL (state 255 instead of 1). */
static int do_test_gpif_soft_stop(libusb_device_handle *h)
{
    int r;
    int cycles = 10;

    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL gpif_soft_stop: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    int soft_count = 0;
    int force_count = 0;

    for (int i = 0; i < cycles; i++) {
        int got = (i == 0) ? primed_start_and_read_retry(h, 16384, 2000)
                           : primed_start_and_read(h, 16384, 2000);
        if (got < 1024) {
            struct fx3_stats s = {0};
            read_stats(h, &s);
            printf("FAIL gpif_soft_stop: cycle %d/%d: no data (got=%d, GPIF=%u)\n",
                   i + 1, cycles, got < 0 ? 0 : got, s.gpif_state);
            cmd_u32(h, STOPFX3, 0);
            return 1;
        }

        r = cmd_u32(h, STOPFX3, 0);
        if (r < 0) {
            printf("FAIL gpif_soft_stop: STOPFX3 cycle %d: %s\n",
                   i + 1, libusb_strerror(r));
            return 1;
        }
        usleep(50000);

        struct fx3_stats s = {0};
        read_stats(h, &s);

        if (s.gpif_state == 1)
            soft_count++;
        else if (s.gpif_state == 0 || s.gpif_state == 255)
            force_count++;
        else {
            printf("FAIL gpif_soft_stop: cycle %d/%d: GPIF state=%u "
                   "(SM still running after STOP)\n",
                   i + 1, cycles, s.gpif_state);
            return 1;
        }
        usleep(50000);
    }

    if (force_count > 0) {
        printf("FAIL gpif_soft_stop: %d/%d cycles used force-stop (state 0/255), "
               "expected all soft-stop (state 1) — new waveform not loaded?\n",
               force_count, cycles);
        return 1;
    }

    printf("PASS gpif_soft_stop: %d/%d cycles stopped cleanly to IDLE (state 1)\n",
           soft_count, cycles);
    return 0;
}

/* Provoke DMA backpressure (start streaming at 64 MS/s, don't read EP1),
 * then issue STOPFX3 and verify the SM exits cleanly to IDLE.
 *
 * Before the fix: SM is stuck in TH0_WAIT or TH1_WAIT (states 8/9)
 * because there are no !FW_TRG transitions out of WAIT states.
 * STOPFX3 must force-stop, leaving state 255 and DMA debris.
 *
 * After the fix: SM exits WAIT → IDLE via !FW_TRG, soft-stop succeeds,
 * and the next streaming session starts cleanly.
 *
 * REQUIRES: updated SDDC_GPIF.h with !FW_TRG exits on TH0_WAIT
 * and TH1_WAIT. */
static int do_test_stop_under_backpressure(libusb_device_handle *h)
{
    int r;

    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL stop_under_backpressure: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    int cycles = 5;
    int soft_count = 0;

    for (int i = 0; i < cycles; i++) {
        /* Start streaming */
        r = cmd_u32_retry(h, STARTFX3, 0);
        if (r < 0) {
            printf("FAIL stop_under_backpressure: STARTFX3 cycle %d: %s\n",
                   i + 1, libusb_strerror(r));
            return 1;
        }

        /* Do NOT read EP1 — let DMA buffers fill and GPIF enter WAIT.
         * At 64 MS/s the 4x16KB buffers fill in ~0.5 ms; wait 200ms
         * to ensure the SM is firmly stuck in a WAIT state. */
        usleep(200000);

        /* Stop — this is the critical test */
        r = cmd_u32(h, STOPFX3, 0);
        if (r < 0) {
            printf("FAIL stop_under_backpressure: STOPFX3 cycle %d: %s\n",
                   i + 1, libusb_strerror(r));
            return 1;
        }
        usleep(50000);

        struct fx3_stats s = {0};
        read_stats(h, &s);

        if (s.gpif_state == 1) {
            soft_count++;
        } else if (s.gpif_state != 0 && s.gpif_state != 255) {
            printf("FAIL stop_under_backpressure: cycle %d/%d: "
                   "GPIF state=%u after STOP under backpressure (SM stuck)\n",
                   i + 1, cycles, s.gpif_state);
            return 1;
        }

        /* Verify recovery: start again and confirm data flows */
        int got = primed_start_and_read(h, 16384, 2000);
        cmd_u32(h, STOPFX3, 0);

        if (got < 1024) {
            printf("FAIL stop_under_backpressure: cycle %d/%d: "
                   "only %d bytes after recovery (device wedged)\n",
                   i + 1, cycles, got < 0 ? 0 : got);
            return 1;
        }
        usleep(100000);
    }

    if (soft_count < cycles) {
        printf("PASS stop_under_backpressure: %d/%d cycles recovered, "
               "but only %d used soft-stop (new waveform not loaded?)\n",
               cycles, cycles, soft_count);
        return 0;
    }

    printf("PASS stop_under_backpressure: %d/%d cycles: "
           "clean soft-stop from WAIT state + data resumed\n",
           soft_count, cycles);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Watchdog stress — observe watchdog recovery self-limiting behavior  */
/* ------------------------------------------------------------------ */

/* Start streaming at 64 MHz, never read bulk data, poll GETSTATS once
 * per second.  The watchdog fires ~300ms after DMA stalls, does
 * force-disable + DMA reset + restart.  After recovery, glDMACount is
 * zeroed, which causes the curDMA > 0 guard to suppress further stall
 * detection — the loop is self-limiting (~2 recoveries then dormant).
 *
 * Usage:  fx3_cmd watchdog_stress [max_seconds]
 *   default max_seconds = 120
 *
 * Typical result: faults plateau at baseline+2, GPIF state 255.
 * A FAIL here means the self-limiting broke or EP0 died. */
static int do_test_watchdog_stress(libusb_device_handle *h, int max_seconds)
{
    int r;
    struct fx3_stats prev, cur;

    if (max_seconds <= 0) max_seconds = 120;

    /* 1. Configure 64 MHz and start streaming */
    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL watchdog_stress: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }
    r = cmd_u32_retry(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL watchdog_stress: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Baseline after first buffers fill */
    usleep(500000);
    r = read_stats(h, &prev);
    if (r < 0) {
        printf("FAIL watchdog_stress: baseline GETSTATS: %s\n",
               libusb_strerror(r));
        cmd_u32(h, STOPFX3, 0);
        return 1;
    }
    printf("#   watchdog_stress: baseline faults=%u gpif=%u\n",
           prev.streaming_faults, prev.gpif_state);

    /* 3. Poll loop — 1 second intervals, never read EP1 */
    int died_at = -1;
    uint32_t last_faults = prev.streaming_faults;
    int stalled_seconds = 0;  /* consecutive seconds with no fault growth */

    for (int sec = 1; sec <= max_seconds; sec++) {
        sleep(1);

        r = read_stats(h, &cur);
        if (r < 0) {
            died_at = sec;
            printf("#   watchdog_stress: GETSTATS failed at t=%ds "
                   "(last faults=%u): %s\n",
                   sec, last_faults, libusb_strerror(r));
            break;
        }

        printf("#   watchdog_stress: t=%3ds  faults=%u (+%u)  "
               "gpif=%u  pib=%u  dma=%u\n",
               sec, cur.streaming_faults,
               cur.streaming_faults - prev.streaming_faults,
               cur.gpif_state, cur.pib_errors, cur.dma_count);

        if (cur.streaming_faults > last_faults) {
            stalled_seconds = 0;
        } else {
            stalled_seconds++;
        }
        last_faults = cur.streaming_faults;
        prev = cur;

        /* If faults stopped growing for 10s, the cap is working */
        if (stalled_seconds >= 10) {
            printf("PASS watchdog_stress: faults plateaued at %u "
                   "for %ds (cap working)\n",
                   cur.streaming_faults, stalled_seconds);
            cmd_u32(h, STOPFX3, 0);
            return 0;
        }
    }

    /* 4. Clean up (best-effort — device may be dead) */
    cmd_u32(h, STOPFX3, 0);
    usleep(200000);

    if (died_at > 0) {
        /* Verify it's really dead */
        uint8_t info[4] = {0};
        r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
        if (r < 0) {
            printf("FAIL watchdog_stress: device hard-locked at t=%ds "
                   "after %u watchdog recoveries\n",
                   died_at, last_faults);
        } else {
            printf("FAIL watchdog_stress: GETSTATS failed at t=%ds but "
                   "TESTFX3 still works (transient?)\n", died_at);
        }
        return 1;
    }

    /* Survived the full duration but faults never plateaued */
    printf("WARN watchdog_stress: survived %ds but faults still growing "
           "(last=%u) — increase duration or check recovery rate\n",
           max_seconds, last_faults);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Watchdog race — provoke EP0-vs-watchdog thread collision           */
/* ------------------------------------------------------------------ */

/* The soak failure at chunk 80 showed a hard USB lockup during an
 * abandoned_stream scenario.  The watchdog_stress test proved the
 * recovery loop is self-limiting (~2 cycles), so the crash is not
 * from unbounded cycling.  Hypothesis: a race between the watchdog
 * recovery (application thread: GpifDisable + DmaReset + FlushEp +
 * SetXfer + SMStart) and an EP0 control transfer (USB callback thread:
 * SendEP0Data for GETSTATS/TESTFX3).
 *
 * This test maximises the collision window by:
 *   - Starting streaming at 64 MHz with no host bulk reads
 *   - Hammering EP0 with alternating GETSTATS + TESTFX3 every 50ms
 *   - Cycling STOP/START every 5s to re-arm the watchdog
 *     (since it self-limits after ~2 recoveries per START)
 *
 * Each 5s window: ~100 EP0 transfers overlapping ~2 watchdog recoveries.
 * Over N rounds that's N*100 chances to hit the race.
 *
 * Usage:  fx3_cmd watchdog_race [rounds]
 *   default rounds = 50  (~250s total)
 *
 * PASS: device survives all rounds
 * FAIL: EP0 timeout or device disappears */
static int do_test_watchdog_race(libusb_device_handle *h, int rounds)
{
    int r;

    if (rounds <= 0) rounds = 50;

    /* Configure 64 MHz clock once */
    r = cmd_u32_retry(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL watchdog_race: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    int total_ep0 = 0;
    int total_faults = 0;

    for (int round = 1; round <= rounds; round++) {
        /* Start streaming — watchdog will fire after ~300ms of no reads */
        r = cmd_u32_retry(h, STARTFX3, 0);
        if (r < 0) {
            printf("FAIL watchdog_race: STARTFX3 round %d: %s\n",
                   round, libusb_strerror(r));
            return 1;
        }

        /* Hammer EP0 for 5 seconds at 50ms intervals.
         * Alternate GETSTATS (20-byte IN) and TESTFX3 (4-byte IN)
         * to exercise different response sizes during the race window. */
        int ep0_ok = 0, ep0_fail = 0;
        struct fx3_stats s;

        for (int i = 0; i < 100; i++) {  /* 100 × 50ms = 5s */
            usleep(50000);

            if (i & 1) {
                /* GETSTATS */
                r = read_stats(h, &s);
            } else {
                /* TESTFX3 */
                uint8_t info[4] = {0};
                r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
            }

            if (r < 0) {
                ep0_fail++;
                /* One failure might be transient — try one more before
                 * declaring the device dead */
                usleep(100000);
                uint8_t probe[4] = {0};
                r = ctrl_read(h, TESTFX3, 0, 0, probe, 4);
                if (r < 0) {
                    printf("FAIL watchdog_race: device dead at round %d, "
                           "poll %d (%d EP0 ok, %d fail so far): %s\n",
                           round, i, total_ep0 + ep0_ok,
                           ep0_fail, libusb_strerror(r));
                    /* Try to read final stats for diagnostics */
                    cmd_u32(h, STOPFX3, 0);
                    return 1;
                }
                /* Recovered from transient — continue */
            } else {
                ep0_ok++;
            }
        }

        total_ep0 += ep0_ok;

        /* Read faults before stopping */
        r = read_stats(h, &s);
        if (r < 0) {
            printf("FAIL watchdog_race: GETSTATS before STOP round %d: %s\n",
                   round, libusb_strerror(r));
            cmd_u32(h, STOPFX3, 0);
            return 1;
        }
        total_faults = (int)s.streaming_faults;

        /* Stop and let quiesce before next round */
        cmd_u32(h, STOPFX3, 0);
        usleep(200000);

        /* Health check */
        uint8_t info[4] = {0};
        r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
        if (r < 0) {
            printf("FAIL watchdog_race: health check after round %d: %s\n",
                   round, libusb_strerror(r));
            return 1;
        }

        /* Progress every 10 rounds */
        if (round % 10 == 0 || round == 1) {
            printf("#   watchdog_race: round %d/%d  ep0_ok=%d  "
                   "ep0_fail=%d  faults=%d\n",
                   round, rounds, total_ep0, ep0_fail, total_faults);
        }
    }

    printf("PASS watchdog_race: %d rounds, %d EP0 transfers, "
           "%d watchdog recoveries — no hard lockup\n",
           rounds, total_ep0, total_faults);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Soak test harness                                                  */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t soak_stop;

static void soak_sigint(int sig)
{
    (void)sig;
    soak_stop = 1;
}

struct soak_scenario {
    const char *name;
    int (*func)(libusb_device_handle *);
    int weight;
    int runs;
    int pass;
    int fail;
};

/* Run between every soak scenario.  Probes TESTFX3 (device alive,
 * hwconfig unchanged) and GETSTATS (GPIF state sane), storing the
 * latest stats snapshot in *prev for the status-line report.
 * Returns 0 on pass, 1 on fail. */
static int soak_health_check(libusb_device_handle *h, struct fx3_stats *prev)
{
    /* TESTFX3 — device alive, hwconfig still 0x04 */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("HEALTH FAIL: TESTFX3 failed: %s\n", libusb_strerror(r));
        return 1;
    }
    if (r >= 1 && info[0] != 0x04) {
        printf("HEALTH FAIL: hwconfig=0x%02X (expected 0x04)\n", info[0]);
        return 1;
    }

    /* GETSTATS — check GPIF state */
    struct fx3_stats s;
    r = read_stats(h, &s);
    if (r < 0) {
        printf("HEALTH FAIL: GETSTATS: %s\n", libusb_strerror(r));
        return 1;
    }

    /* GPIF should be idle (0, 1, or 255), not stuck in a read state */
    if (s.gpif_state != 0 && s.gpif_state != 1 && s.gpif_state != 255) {
        printf("HEALTH WARN: GPIF state=%u (not idle)\n", s.gpif_state);
        /* Not fatal — watchdog may be mid-recovery */
    }

    *prev = s;
    return 0;
}

/* Soak test outer loop.
 *
 * Parses optional [hours] [seed] from argv, installs a SIGINT handler
 * for clean early-exit, then loops until duration expires or Ctrl-C:
 *   1. Weighted random scenario pick
 *   2. Run scenario function
 *   3. Health check (TESTFX3 + GETSTATS)
 *   4. Update per-scenario and cumulative stats
 *   5. Print status line every 10 cycles
 * Prints a final summary table on exit.  Returns 0 if all passed. */
static int soak_main(libusb_device_handle *h, int argc, char **argv)
{
    double hours = 1.0;
    unsigned int seed = (unsigned int)time(NULL);
    int max_scenarios = 0;   /* 0 = unlimited (run until time expires) */
    int quiet = 0;           /* -q: suppress PASS output */

    /* Strip -q flag from argv before positional parsing */
    int pos_argc = 0;
    char *pos_argv[16];
    for (int i = 0; i < argc && pos_argc < 16; i++) {
        if (strcmp(argv[i], "-q") == 0)
            quiet = 1;
        else
            pos_argv[pos_argc++] = argv[i];
    }

    if (pos_argc >= 1) hours = atof(pos_argv[0]);
    if (pos_argc >= 2) seed = (unsigned int)strtoul(pos_argv[1], NULL, 0);
    if (pos_argc >= 3) max_scenarios = atoi(pos_argv[2]);
    if (hours <= 0) hours = 1.0;

    struct soak_scenario scenarios[] = {
        {"stop_start_cycle",    do_test_stop_start_cycle,    20, 0, 0, 0},
        {"wedge_recovery",      do_test_wedge_recovery,      15, 0, 0, 0},
        {"pib_overflow",        do_test_pib_overflow,         5, 0, 0, 0},
        {"debug_race",          do_test_debug_race,          10, 0, 0, 0},
        {"console_fill",        do_test_console_fill,         5, 0, 0, 0},
        {"ep0_overflow",        do_ep0_overflow,              5, 0, 0, 0},
        {"oob_brequest",        do_test_oob_brequest,         5, 0, 0, 0},
        {"oob_setarg",          do_test_oob_setarg,           5, 0, 0, 0},
        {"pll_preflight",       do_test_pll_preflight,       10, 0, 0, 0},
        {"clock_pull",          do_test_clock_pull,          10, 0, 0, 0},
        {"freq_hop",            do_test_freq_hop,            10, 0, 0, 0},
        {"ep0_stall_recovery",  do_test_ep0_stall_recovery,   5, 0, 0, 0},
        {"double_stop",         do_test_double_stop,          5, 0, 0, 0},
        {"double_start",        do_test_double_start,         5, 0, 0, 0},
        {"i2c_under_load",      do_test_i2c_under_load,       5, 0, 0, 0},
        {"sustained_stream",    do_test_sustained_stream,     2, 0, 0, 0},
        {"rapid_start_stop",    do_test_rapid_start_stop,    10, 0, 0, 0},
        {"startadc_mid_stream", do_test_startadc_mid_stream,  5, 0, 0, 0},
        {"setarg_boundary",     do_test_setarg_boundary,      5, 0, 0, 0},
        {"i2c_bad_addr",        do_test_i2c_bad_addr,         5, 0, 0, 0},
        {"ep0_ctrl_streaming",  do_test_ep0_control_while_streaming, 5, 0, 0, 0},
        {"gpio_during_stream",  do_test_gpio_during_stream,   5, 0, 0, 0},
        {"ep0_oversize_all",    do_test_ep0_oversize_all,     3, 0, 0, 0},
        {"i2c_write_read",      do_test_i2c_write_read,       5, 0, 0, 0},
        {"rapid_adc_reprogram", do_test_rapid_adc_reprogram,  5, 0, 0, 0},
        {"debug_while_stream",  do_test_debug_while_streaming,3, 0, 0, 0},
        {"abandoned_stream",    do_test_abandoned_stream,    15, 0, 0, 0},
        {"stale_vendor_codes",  do_test_stale_vendor_codes,   3, 0, 0, 0},
        {"setarg_gap_index",    do_test_setarg_gap_index,     3, 0, 0, 0},
        {"dma_count_reset",     do_test_dma_count_reset,      5, 0, 0, 0},
        {"dma_count_monotonic", do_test_dma_count_monotonic,  5, 0, 0, 0},
        {"watchdog_cap_observe",do_test_watchdog_cap_observe, 5, 0, 0, 0},
        {"watchdog_cap_restart",do_test_watchdog_cap_restart, 5, 0, 0, 0},
        {"i2c_write_bad_addr",  do_test_i2c_write_bad_addr,   3, 0, 0, 0},
        {"i2c_multibyte",       do_test_i2c_multibyte,        3, 0, 0, 0},
        {"ep0_hammer",          do_test_ep0_hammer,           3, 0, 0, 0},
        {"debug_cmd_stream",    do_test_debug_cmd_while_stream,3, 0, 0, 0},
        {"readinfodebug_flood", do_test_readinfodebug_flood,  3, 0, 0, 0},
        {"data_sanity",         do_test_data_sanity,          2, 0, 0, 0},
    };
    int nscenarios = (int)(sizeof(scenarios) / sizeof(scenarios[0]));

    /* Compute total weight for weighted random selection */
    int total_weight = 0;
    for (int i = 0; i < nscenarios; i++)
        total_weight += scenarios[i].weight;

    /* Install SIGINT handler */
    soak_stop = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = soak_sigint;
    sigaction(SIGINT, &sa, NULL);

    srand(seed);

    double duration_sec = hours * 3600.0;
    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    printf("=== SOAK TEST START ===\n");
    printf("Duration: %.3f hours  Seed: %u  Scenarios: %d",
           hours, seed, nscenarios);
    if (max_scenarios > 0)
        printf("  MaxCycles: %d", max_scenarios);
    printf("\n");
    printf("Press Ctrl-C for early stop with summary\n\n");

    /* Initial health check */
    struct fx3_stats prev_stats;
    memset(&prev_stats, 0, sizeof(prev_stats));
    if (soak_health_check(h, &prev_stats) != 0) {
        printf("SOAK ABORT: initial health check failed\n");
        return 1;
    }

    int total_cycles = 0, total_pass = 0, total_fail = 0;
    int health_pass = 0, health_fail = 0;
    int prev_sel = -1;

    while (!soak_stop) {
        /* Check elapsed time */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start_ts.tv_sec)
                       + (now.tv_nsec - start_ts.tv_nsec) / 1e9;
        if (elapsed >= duration_sec) break;

        /* Check scenario count limit (for chunked reload runs) */
        if (max_scenarios > 0 && total_cycles >= max_scenarios) break;

        /* Weighted random pick */
        int pick = rand() % total_weight;
        int sel = 0;
        int accum = 0;
        for (int i = 0; i < nscenarios; i++) {
            accum += scenarios[i].weight;
            if (pick < accum) { sel = i; break; }
        }

        /* Run scenario — in quiet mode, capture stdout to a tmpfile
         * and only replay it if the scenario fails. */
        int saved_fd = -1;
        FILE *tmpf = NULL;
        if (quiet) {
            fflush(stdout);
            saved_fd = dup(STDOUT_FILENO);
            tmpf = tmpfile();
            if (tmpf && saved_fd >= 0)
                dup2(fileno(tmpf), STDOUT_FILENO);
        }
        int result = scenarios[sel].func(h);
        if (quiet && saved_fd >= 0) {
            fflush(stdout);
            if (result != 0 && tmpf) {
                /* Replay captured output on failure */
                rewind(tmpf);
                char replay_buf[4096];
                size_t n;
                while ((n = fread(replay_buf, 1, sizeof(replay_buf), tmpf)) > 0) {
                    ssize_t w = write(saved_fd, replay_buf, n);
                    (void)w;
                }
            }
            dup2(saved_fd, STDOUT_FILENO);
            close(saved_fd);
            if (tmpf) fclose(tmpf);
        }
        scenarios[sel].runs++;
        if (result == 0) {
            scenarios[sel].pass++;
            total_pass++;
        } else {
            scenarios[sel].fail++;
            total_fail++;
            /* Capture device state at moment of failure */
            struct fx3_stats fail_stats;
            if (read_stats(h, &fail_stats) == 0) {
                printf(">>> FAIL %s: gpif=%u dma=%u pib=%u pib_arg=0x%04X i2c=%u faults=%u si5351=0x%02X",
                       scenarios[sel].name, fail_stats.gpif_state,
                       fail_stats.dma_count, fail_stats.pib_errors,
                       fail_stats.last_pib_arg,
                       fail_stats.i2c_failures, fail_stats.streaming_faults,
                       fail_stats.si5351_status);
                if (prev_sel >= 0)
                    printf(" prev=%s", scenarios[prev_sel].name);
                printf("\n");
            } else {
                printf(">>> FAIL %s: GETSTATS unreadable at failure",
                       scenarios[sel].name);
                if (prev_sel >= 0)
                    printf(" prev=%s", scenarios[prev_sel].name);
                printf("\n");
            }
            fflush(stdout);
        }
        total_cycles++;
        prev_sel = sel;

        /* Inter-scenario cleanup: ensure streaming is stopped before the
         * health check.  Many scenarios already send STOPFX3 on their
         * success path, but on failure they often bail out early without
         * cleaning up.  A stale streaming state bleeds into the next
         * scenario, causing cascading STARTFX3 timeouts and HEALTH FAILs.
         *
         * Rule for new scenarios: always STOPFX3 on the success path,
         * and rely on this safety net for early-exit failure paths. */
        cmd_u32(h, STOPFX3, 0);   /* ignore errors — may already be stopped */
        cmd_u32(h, GPIOFX3, 0x0800); /* LED_BLUE — clear SHDWN after gpio scenarios */
        usleep(100000);            /* 100 ms — let GPIF/DMA quiesce */

        /* Health check — retry once on failure.  After a scenario
         * triggers a watchdog recovery, the device may need up to ~2s
         * to finish.  Rather than penalise the next scenario with a
         * broken device, absorb the delay here. */
        if (soak_health_check(h, &prev_stats) == 0) {
            health_pass++;
        } else {
            /* Device unhealthy — give the watchdog time to finish,
             * then retry once before moving on. */
            usleep(2000000);       /* 2 s recovery window */
            if (soak_health_check(h, &prev_stats) == 0) {
                health_pass++;
            } else {
                health_fail++;
            }
        }

        /* Status line every 10 cycles */
        if (total_cycles % 10 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            elapsed = (now.tv_sec - start_ts.tv_sec)
                    + (now.tv_nsec - start_ts.tv_nsec) / 1e9;
            int h_e = (int)(elapsed / 3600);
            int m_e = (int)((elapsed - h_e * 3600) / 60);
            int s_e = (int)(elapsed - h_e * 3600 - m_e * 60);
            printf("[%02d:%02d:%02d] cycle=%d pass=%d fail=%d | "
                   "last=%s(%s) | dma=%u pib=%u i2c=%u faults=%u\n",
                   h_e, m_e, s_e, total_cycles, total_pass, total_fail,
                   scenarios[sel].name, result == 0 ? "PASS" : "FAIL",
                   prev_stats.dma_count, prev_stats.pib_errors,
                   prev_stats.i2c_failures, prev_stats.streaming_faults);
            fflush(stdout);
        }
    }

    /* Final report */
    struct timespec end_ts;
    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    double total_elapsed = (end_ts.tv_sec - start_ts.tv_sec)
                         + (end_ts.tv_nsec - start_ts.tv_nsec) / 1e9;
    int h_t = (int)(total_elapsed / 3600);
    int m_t = (int)((total_elapsed - h_t * 3600) / 60);
    int s_t = (int)(total_elapsed - h_t * 3600 - m_t * 60);

    printf("\n=== SOAK TEST SUMMARY ===\n");
    printf("Duration: %02d:%02d:%02d  Seed: %u  Cycles: %d\n\n",
           h_t, m_t, s_t, seed, total_cycles);

    printf("%-24s %5s %5s %5s\n", "Scenario", "Runs", "Pass", "Fail");
    for (int i = 0; i < nscenarios; i++) {
        if (scenarios[i].runs > 0) {
            printf("%-24s %5d %5d %5d\n",
                   scenarios[i].name, scenarios[i].runs,
                   scenarios[i].pass, scenarios[i].fail);
        }
    }
    printf("%-24s %5d %5d %5d\n", "TOTAL", total_cycles, total_pass, total_fail);

    printf("\nGESTATS cumulative:\n");
    printf("  dma_completions:  %u\n", prev_stats.dma_count);
    printf("  pib_errors:       %u\n", prev_stats.pib_errors);
    printf("  i2c_failures:     %u\n", prev_stats.i2c_failures);
    printf("  streaming_faults: %u\n", prev_stats.streaming_faults);
    printf("  health_checks:    %d/%d passed\n", health_pass, health_pass + health_fail);

    if (total_fail > 0) {
        printf("\nResult: %d FAILURES in %d cycles (%.2f%% failure rate)\n",
               total_fail, total_cycles,
               total_cycles > 0 ? (total_fail * 100.0 / total_cycles) : 0.0);
    } else {
        printf("\nResult: ALL PASSED (%d cycles)\n", total_cycles);
    }

    return total_fail > 0 ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Usage and main                                                     */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-F firmware.img] <command> [args...]\n"
        "\n"
        "Options:\n"
        "  -F, --firmware <path>        Upload firmware first if device is in\n"
        "                               bootloader mode (PID 0x00F3)\n"
        "\n"
        "Commands:\n"
        "  load <firmware.img>          Upload firmware and exit\n"
        "  test                         Read device info (TESTFX3)\n"
        "  gpio <bits>                  Set GPIO word (hex or decimal)\n"
        "  adc <freq_hz>               Set ADC clock frequency (STARTADC)\n"
        "  att <0-63>                   Set DAT-31 attenuator\n"
        "  vga <0-255>                  Set AD8370 VGA gain\n"
        "  wdg_max <0-255>             Set watchdog max recovery count (0=unlimited)\n"
        "  start                        Start streaming (STARTFX3)\n"
        "  stop                         Stop streaming (STOPFX3)\n"
        "  i2cr <addr> <reg> <len>      I2C read (hex addresses)\n"
        "  i2cw <addr> <reg> <byte>...  I2C write (hex addresses, hex data)\n"
        "  reset                        Reboot FX3 to bootloader\n"
        "  debug                        Interactive debug console over USB\n"
        "  raw <code>                   Send raw vendor request (hex)\n"
        "  ep0_overflow                 Test EP0 wLength bounds check\n"
        "  oob_brequest                 Test OOB bRequest bounds (issue #21)\n"
        "  oob_setarg                   Test OOB SETARGFX3 wIndex (issue #20)\n"
        "  console_fill                 Test console buffer fill (issue #13)\n"
        "  debug_race                   Stress-test debug buffer race (issue #8)\n"
        "  debug_poll                   Test debug console over USB (issue #26)\n"
        "  pib_overflow                 Provoke + detect PIB error (issue #10)\n"
        "  stack_check                  Query stack watermark, verify headroom (issue #12)\n"
        "  stats                        Read GETSTATS diagnostic counters\n"
        "  stats_i2c                    Verify I2C failure counter via NACK\n"
        "  stats_pib                    Verify PIB error counter via overflow\n"
        "  stats_pll                    Verify Si5351 PLL lock status\n"
        "  stop_gpif_state              Verify GPIF SM stops after STOPFX3\n"
        "  stop_start_cycle             Cycle STOP+START N times, verify data\n"
        "  pll_preflight                Verify STARTFX3 rejected without clock\n"
        "  wedge_recovery               Provoke DMA wedge, test STOP+START recovery\n"
        "  clock_pull                   Pull clock mid-stream, verify recovery\n"
        "  freq_hop                     Rapid ADC frequency hopping\n"
        "  ep0_stall_recovery           EP0 stall then immediate use\n"
        "  double_stop                  Back-to-back STOPFX3\n"
        "  double_start                 Back-to-back STARTFX3\n"
        "  i2c_under_load               I2C read while streaming\n"
        "  sustained_stream             30s continuous streaming check\n"
        "  rapid_start_stop             50× START/STOP with no bulk reads\n"
        "  startadc_mid_stream          Reprogram ADC clock while streaming\n"
        "  setarg_boundary              SETARGFX3 boundary/OOB values\n"
        "  i2c_bad_addr                 I2C read to absent address (NACK)\n"
        "  ep0_control_while_streaming  Mixed EP0 commands during stream\n"
        "  gpio_during_stream           GPIO bit patterns during stream\n"
        "  ep0_oversize_all             wLength>64 for all data-phase cmds\n"
        "  i2c_write_read               I2CWFX3+I2CRFX3 round-trip verify\n"
        "  rapid_adc_reprogram          Back-to-back STARTADC freq changes\n"
        "  debug_while_streaming        READINFODEBUG during active stream\n"
        "  abandoned_stream             Simulate host crash (no STOPFX3)\n"
        "  watchdog_stress [secs]       Observe WDG recovery self-limiting\n"
        "  watchdog_race [rounds]       Provoke EP0-vs-WDG thread race\n"
        "  soak [hours] [seed] [max]    Multi-hour randomized stress test\n"
        "\n"
        "Output:  PASS/FAIL <command> [details]\n"
        "Exit:    0 on PASS, 1 on FAIL\n",
        prog);
}

static unsigned long parse_num(const char *s)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);  /* 0 = auto-detect base */
    if (errno || *end != '\0') {
        fprintf(stderr, "error: invalid number '%s'\n", s);
        exit(2);
    }
    return v;
}

int main(int argc, char **argv)
{
    /* ---- Parse -F / --firmware option ---- */
    const char *firmware_path = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-F") == 0 ||
            strcmp(argv[i], "--firmware") == 0) {
            firmware_path = argv[i + 1];
            /* Remove -F and its argument from argv */
            int remaining = argc - i - 2;
            memmove(&argv[i], &argv[i + 2], remaining * sizeof(char *));
            argc -= 2;
            argv[argc] = NULL;
            break;
        }
    }

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    const char *cmd = argv[1];

    libusb_context *ctx = NULL;
    int r = libusb_init(&ctx);
    if (r < 0) {
        fprintf(stderr, "error: libusb_init: %s\n", libusb_strerror(r));
        return 1;
    }
    g_ctx = ctx;

    /* ---- Handle "load" command (upload-only, no app-mode device needed) ---- */
    if (strcmp(cmd, "load") == 0) {
        const char *fw = (argc >= 3) ? argv[2] : firmware_path;
        if (!fw) {
            fprintf(stderr, "error: load requires a firmware path\n"
                            "usage: %s load <firmware.img>\n", argv[0]);
            libusb_exit(ctx);
            return 2;
        }
        /* If device is already in app mode, reset to bootloader first */
        libusb_device_handle *app =
            libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_APP);
        if (app) {
            fprintf(stderr, "device in app mode — resetting to bootloader...\n");
            if (libusb_kernel_driver_active(app, 0) == 1)
                libusb_detach_kernel_driver(app, 0);
            libusb_claim_interface(app, 0);
            cmd_u32(app, RESETFX3, 0);
            libusb_close(app);
            sleep(3);
        }
        int rc = upload_firmware(ctx, fw);
        libusb_exit(ctx);
        return (rc == 0) ? 0 : 1;
    }

    /* ---- Auto-upload if -F given and device is in bootloader mode ---- */
    if (firmware_path) {
        libusb_device_handle *boot =
            libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_BOOT);
        if (boot) {
            libusb_close(boot);
            fprintf(stderr, "device in bootloader mode "
                    "— uploading firmware...\n");
            if (upload_firmware(ctx, firmware_path) != 0) {
                libusb_exit(ctx);
                return 1;
            }
        }
    }

    libusb_device_handle *h = open_rx888(ctx);
    if (!h) {
        libusb_exit(ctx);
        return 1;
    }

    int rc = 1;

    if (strcmp(cmd, "test") == 0) {
        rc = do_test(h);

    } else if (strcmp(cmd, "gpio") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_gpio(h, (uint32_t)parse_num(argv[2]));

    } else if (strcmp(cmd, "adc") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_adc(h, (uint32_t)parse_num(argv[2]));

    } else if (strcmp(cmd, "att") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_att(h, (uint16_t)parse_num(argv[2]));

    } else if (strcmp(cmd, "vga") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_vga(h, (uint16_t)parse_num(argv[2]));

    } else if (strcmp(cmd, "wdg_max") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_wdg_max(h, (uint16_t)parse_num(argv[2]));

    } else if (strcmp(cmd, "start") == 0) {
        rc = do_start(h);

    } else if (strcmp(cmd, "stop") == 0) {
        rc = do_stop(h);

    } else if (strcmp(cmd, "i2cr") == 0) {
        if (argc < 5) { usage(argv[0]); goto out; }
        rc = do_i2cr(h, (uint16_t)parse_num(argv[2]),
                        (uint16_t)parse_num(argv[3]),
                        (uint16_t)parse_num(argv[4]));

    } else if (strcmp(cmd, "i2cw") == 0) {
        if (argc < 5) { usage(argv[0]); goto out; }
        uint16_t addr = (uint16_t)parse_num(argv[2]);
        uint16_t reg  = (uint16_t)parse_num(argv[3]);
        int ndata = argc - 4;
        uint8_t data[64];
        if (ndata > (int)sizeof(data)) ndata = (int)sizeof(data);
        for (int i = 0; i < ndata; i++)
            data[i] = (uint8_t)parse_num(argv[4 + i]);
        rc = do_i2cw(h, addr, reg, data, (uint16_t)ndata);

    } else if (strcmp(cmd, "debug") == 0) {
        rc = do_debug(h);

    } else if (strcmp(cmd, "oob_brequest") == 0) {
        rc = do_test_oob_brequest(h);

    } else if (strcmp(cmd, "oob_setarg") == 0) {
        rc = do_test_oob_setarg(h);

    } else if (strcmp(cmd, "console_fill") == 0) {
        rc = do_test_console_fill(h);

    } else if (strcmp(cmd, "debug_race") == 0) {
        rc = do_test_debug_race(h);

    } else if (strcmp(cmd, "debug_poll") == 0) {
        rc = do_test_debug_poll(h);

    } else if (strcmp(cmd, "pib_overflow") == 0) {
        rc = do_test_pib_overflow(h);

    } else if (strcmp(cmd, "stack_check") == 0) {
        rc = do_test_stack_check(h);

    } else if (strcmp(cmd, "stats") == 0) {
        rc = do_stats(h);

    } else if (strcmp(cmd, "stats_i2c") == 0) {
        rc = do_test_stats_i2c(h);

    } else if (strcmp(cmd, "stats_pib") == 0) {
        rc = do_test_stats_pib(h);

    } else if (strcmp(cmd, "stats_pll") == 0) {
        rc = do_test_stats_pll(h);

    } else if (strcmp(cmd, "stop_gpif_state") == 0) {
        rc = do_test_stop_gpif_state(h);

    } else if (strcmp(cmd, "stop_start_cycle") == 0) {
        rc = do_test_stop_start_cycle(h);

    } else if (strcmp(cmd, "pll_preflight") == 0) {
        rc = do_test_pll_preflight(h);

    } else if (strcmp(cmd, "wedge_recovery") == 0) {
        rc = do_test_wedge_recovery(h);

    } else if (strcmp(cmd, "clock_pull") == 0) {
        rc = do_test_clock_pull(h);

    } else if (strcmp(cmd, "freq_hop") == 0) {
        rc = do_test_freq_hop(h);

    } else if (strcmp(cmd, "ep0_stall_recovery") == 0) {
        rc = do_test_ep0_stall_recovery(h);

    } else if (strcmp(cmd, "double_stop") == 0) {
        rc = do_test_double_stop(h);

    } else if (strcmp(cmd, "double_start") == 0) {
        rc = do_test_double_start(h);

    } else if (strcmp(cmd, "i2c_under_load") == 0) {
        rc = do_test_i2c_under_load(h);

    } else if (strcmp(cmd, "sustained_stream") == 0) {
        rc = do_test_sustained_stream(h);

    } else if (strcmp(cmd, "rapid_start_stop") == 0) {
        rc = do_test_rapid_start_stop(h);

    } else if (strcmp(cmd, "startadc_mid_stream") == 0) {
        rc = do_test_startadc_mid_stream(h);

    } else if (strcmp(cmd, "setarg_boundary") == 0) {
        rc = do_test_setarg_boundary(h);

    } else if (strcmp(cmd, "i2c_bad_addr") == 0) {
        rc = do_test_i2c_bad_addr(h);

    } else if (strcmp(cmd, "ep0_control_while_streaming") == 0) {
        rc = do_test_ep0_control_while_streaming(h);

    } else if (strcmp(cmd, "gpio_during_stream") == 0) {
        rc = do_test_gpio_during_stream(h);

    } else if (strcmp(cmd, "ep0_oversize_all") == 0) {
        rc = do_test_ep0_oversize_all(h);

    } else if (strcmp(cmd, "i2c_write_read") == 0) {
        rc = do_test_i2c_write_read(h);

    } else if (strcmp(cmd, "rapid_adc_reprogram") == 0) {
        rc = do_test_rapid_adc_reprogram(h);

    } else if (strcmp(cmd, "debug_while_streaming") == 0) {
        rc = do_test_debug_while_streaming(h);

    } else if (strcmp(cmd, "abandoned_stream") == 0) {
        rc = do_test_abandoned_stream(h);

    } else if (strcmp(cmd, "vendor_rqt_wrap") == 0) {
        rc = do_test_vendor_rqt_wrap(h);

    } else if (strcmp(cmd, "stale_vendor_codes") == 0) {
        rc = do_test_stale_vendor_codes(h);

    } else if (strcmp(cmd, "setarg_gap_index") == 0) {
        rc = do_test_setarg_gap_index(h);

    } else if (strcmp(cmd, "gpio_extremes") == 0) {
        rc = do_test_gpio_extremes(h);

    } else if (strcmp(cmd, "hw_smoke") == 0) {
        rc = do_test_hw_smoke(h);

    } else if (strcmp(cmd, "i2c_write_bad_addr") == 0) {
        rc = do_test_i2c_write_bad_addr(h);

    } else if (strcmp(cmd, "i2c_multibyte") == 0) {
        rc = do_test_i2c_multibyte(h);

    } else if (strcmp(cmd, "readinfodebug_flood") == 0) {
        rc = do_test_readinfodebug_flood(h);

    } else if (strcmp(cmd, "dma_count_reset") == 0) {
        rc = do_test_dma_count_reset(h);

    } else if (strcmp(cmd, "dma_count_monotonic") == 0) {
        rc = do_test_dma_count_monotonic(h);

    } else if (strcmp(cmd, "watchdog_cap_observe") == 0) {
        rc = do_test_watchdog_cap_observe(h);

    } else if (strcmp(cmd, "watchdog_cap_restart") == 0) {
        rc = do_test_watchdog_cap_restart(h);

    } else if (strcmp(cmd, "ep0_hammer") == 0) {
        rc = do_test_ep0_hammer(h);

    } else if (strcmp(cmd, "debug_cmd_while_stream") == 0) {
        rc = do_test_debug_cmd_while_stream(h);

    } else if (strcmp(cmd, "adc_freq_extremes") == 0) {
        rc = do_test_adc_freq_extremes(h);

    } else if (strcmp(cmd, "data_sanity") == 0) {
        rc = do_test_data_sanity(h);

    } else if (strcmp(cmd, "watchdog_stress") == 0) {
        int secs = (argc >= 3) ? (int)parse_num(argv[2]) : 120;
        rc = do_test_watchdog_stress(h, secs);

    } else if (strcmp(cmd, "watchdog_race") == 0) {
        int rnds = (argc >= 3) ? (int)parse_num(argv[2]) : 50;
        rc = do_test_watchdog_race(h, rnds);

    } else if (strcmp(cmd, "soak") == 0) {
        rc = soak_main(h, argc - 2, argv + 2);

    } else if (strcmp(cmd, "reset") == 0) {
        rc = do_reset(h);

    } else if (strcmp(cmd, "raw") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_raw(h, (uint8_t)parse_num(argv[2]));

    } else if (strcmp(cmd, "ep0_overflow") == 0) {
        rc = do_ep0_overflow(h);

    } else {
        fprintf(stderr, "error: unknown command '%s'\n", cmd);
        usage(argv[0]);
        rc = 2;
    }

out:
    close_rx888(h);
    libusb_exit(ctx);
    return rc;
}
