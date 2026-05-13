/*
 * protocol.h
 *
 * USB vendor request protocol definitions for FX3 firmware.
 * Internalized from the former shared Interface.h for RX888mk2-only firmware.
 */

#pragma once

#define FIRMWARE_VER_MAJOR 2
#define FIRMWARE_VER_MINOR 2

/* USB vendor request command codes (bRequest field in SETUP packet) */
enum FX3Command {
    STARTFX3      = 0xAA,   /* Start GPIF engine and stream ADC data */
    STOPFX3       = 0xAB,   /* Stop GPIF engine */
    TESTFX3       = 0xAC,   /* Get device info (model, version) */
    GPIOFX3       = 0xAD,   /* Control GPIOs */
    I2CWFX3       = 0xAE,   /* Write data to I2C bus */
    I2CRFX3       = 0xAF,   /* Read data from I2C bus */
    RESETFX3      = 0xB1,   /* Reset FX3 to bootloader */
    STARTADC      = 0xB2,   /* Start ADC clock at specified frequency */
    GETSTATS      = 0xB3,   /* Read diagnostic counters (read-only) */
    SETARGFX3     = 0xB6,   /* Set argument by index/value */
    READINFODEBUG = 0xBA,   /* Read debug output / send debug input */
    HANGFX3       = 0xCE,   /* TEST-ONLY: sleep wValue ms in the EP0 handler
                             * to deterministically wedge the vendor callback.
                             * Used by tests/fx3_cmd.c test_health_recovery to
                             * validate the health watchdog's Level-4 reset
                             * (issues #104, #105).  Safe in production: the
                             * watchdog auto-resets the device if invoked
                             * with wValue >= EP0_HANDLER_TIMEOUT_MS. */
    HANGMAIN      = 0xCF,   /* TEST-ONLY: set glHealthHangMain so the main
                             * thread enters an infinite spin on its next
                             * iteration.  Heartbeat stops; the HWDT timer
                             * callback stops petting; HWDT fires after
                             * ~HWDT_PERIOD_MS and resets the device.
                             * Used by tests/fx3_cmd.c test_main_recovery
                             * to validate Level-5 end-to-end.  Safe in
                             * production: HWDT auto-recovers. */
};

/* GPIO bit masks for GPIOFX3 control word (active-low/high depends on pin) */
#define OUTXIO0 (1U << 0)    /* ATT_LE */
#define OUTXIO1 (1U << 1)    /* ATT_CLK */
#define OUTXIO2 (1U << 2)    /* ATT_DATA */
#define OUTXIO3 (1U << 3)    /* SEL0 */
#define OUTXIO4 (1U << 4)    /* SEL1 */
#define OUTXIO5 (1U << 5)    /* SHDWN */
#define OUTXIO6 (1U << 6)    /* DITH */
#define OUTXIO7 (1U << 7)    /* RAND */
#define OUTXIO8 (1U << 8)
#define OUTXIO9 (1U << 9)
#define OUTXI10 (1U << 10)
#define OUTXI11 (1U << 11)
#define OUTXI12 (1U << 12)
#define OUTXI13 (1U << 13)
#define OUTXI14 (1U << 14)
#define OUTXI15 (1U << 15)
#define OUTXI16 (1U << 16)

enum GPIOPin {
    SHDWN      = OUTXIO5,
    DITH       = OUTXIO6,
    RANDO      = OUTXIO7,
    BIAS_HF    = OUTXIO8,
    BIAS_VHF   = OUTXIO9,
    LED_BLUE   = OUTXI11,
    ATT_SEL0   = OUTXI13,
    ATT_SEL1   = OUTXI14,
    VHF_EN     = OUTXI15,
    PGA_EN     = OUTXI16,
};

enum RadioModel {
    NORADIO = 0x00,
    RX888r2 = 0x04,
};

enum ArgumentList {
    DAT31_ATT        = 10,   /* DAT-31 attenuator (0-63) */
    AD8370_VGA       = 11,   /* AD8370 VGA (0-255) */
    WDG_MAX_RECOV    = 14,   /* Max consecutive watchdog recoveries (0=unlimited) */
};

#define WDG_MAX_RECOVERY_DEFAULT 5

#define _DEBUG_USB_
#define MAXLEN_D_USB (256)

/* Debug trace: command-name lookup tables */
#define FX3_CMD_BASE           0xAA
#define FX3_CMD_COUNT          17
#define SETARGFX3_LIST_COUNT   15

#ifdef TRACESERIAL
extern const char *FX3CommandName[FX3_CMD_COUNT];
extern const char *SETARGFX3List[SETARGFX3_LIST_COUNT];
#endif
