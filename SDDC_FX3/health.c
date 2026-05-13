/*
 * health.c — recovery state machine implementation
 *
 * PR 2: EP0 vendor-callback liveness tracking and Level 4
 * (CyU3PDeviceReset) backstop.  Addresses issues #104, #105.
 *
 * PR 3: Level 5 (FX3 hardware watchdog) catastrophic backstop.
 * Catches the failure mode where the main thread itself stops running
 * and Levels 1-4 (which all depend on the main thread to fire) become
 * unreachable.  Health.c is the only place HWDT is configured/petted.
 *
 * See health.h for the contract and PLAN_RECOVERY.md for design notes.
 *
 * The discipline rule: new recovery code MUST extend one of these
 * functions.  Do not invent parallel mechanisms.  Do not add inline
 * cleanup in handlers — record an event here and let health_recover()
 * handle it.  Do not pet HWDT from anywhere outside health_pet().
 */

#include "health.h"
#include "cyu3os.h"
#include "cyu3system.h"

/* If an EP0 vendor callback runs longer than this, declare it wedged
 * and reset the device.  Legitimate handlers complete in <100 ms
 * (longest observed: STOPFX3 soft-stop ~5 ms incl. 1 ms ThreadSleep).
 * Host's libusb_control_transfer times out at 1000 ms (CTRL_TIMEOUT_MS
 * in tests/fx3_cmd.c), so 2000 ms gives ~1 s after host gives up
 * before we hard-reset. */
#define EP0_HANDLER_TIMEOUT_MS  2000

/* FX3 hardware watchdog timer period.  The main thread pets every
 * ~100 ms (one per ThreadSleep iteration), so 10 s is ~100x safety
 * margin against transient processing delays.  Short enough that a
 * true catastrophic main-thread hang gets reset within ~10 s.  Used
 * in CyU3PSysWatchDogConfigure(); see SDK docs for hardware details. */
#define HWDT_PERIOD_MS  10000

/* Bisection knob (issue: PR 3 1-hour soak introduced a streaming-dead
 * regression at ~cycle 900 that wasn't present on PR 2 firmware).
 * Set to 0 to skip BOTH the CyU3PSysWatchDogConfigure() call in
 * health_init() AND the CyU3PSysWatchDogClear() calls in health_pet()
 * — so the WD register isn't touched at all.  Flip back to 1 once
 * the bisection is complete. */
#define HEALTH_HWDT_ENABLED  0

/* Accumulated state inspected by health_evaluate().  Written by the
 * USB callback thread, read by the main loop.  Single-byte/word
 * accesses are atomic on ARM926; volatile prevents compiler reordering.
 * Ordering rule: writers update timestamp BEFORE setting in_progress,
 * so a reader never sees in_progress=1 with a stale enter_ms. */
static struct {
    volatile uint32_t ep0_handler_enter_ms;
    volatile uint8_t  ep0_handler_in_progress;
} glHealthState = { 0, 0 };

/* Boot counter — incremented once per firmware boot in health_init().
 * Exposed via GETSTATS so the host can detect mid-test device resets
 * (Level 4 CyU3PDeviceReset, Level 5 HWDT, manual RESETFX3, or power
 * cycle).  No NVRAM persistence; cold boot resets to 0. */
static uint32_t glBootCount = 0;

void health_init(void)
{
    glHealthState.ep0_handler_enter_ms = 0;
    glHealthState.ep0_handler_in_progress = 0;

    /* Increment first thing so the count reflects "this is boot N".
     * Exposed via health_boot_count() / GETSTATS for reset detection. */
    glBootCount++;

    /* Level 5 — enable the FX3 hardware watchdog.  Main loop must call
     * health_pet() at least every HWDT_PERIOD_MS to keep the device
     * alive; see RunApplication.c.  Requires CyU3PDeviceInit to have
     * been called (it has, by main() in StartUp.c before us). */
#if HEALTH_HWDT_ENABLED
    CyU3PSysWatchDogConfigure(CyTrue, HWDT_PERIOD_MS);
#endif
}

uint32_t health_boot_count(void)
{
    return glBootCount;
}

void health_pet(void)
{
    /* Level 5 — keep the FX3 hardware watchdog from firing.  This is
     * a single register write; cheap.  Called from every main-loop
     * iteration in RunApplication.c (both wait-for-enumeration and
     * run-forever loops).  Gated by HEALTH_HWDT_ENABLED so the
     * bisection probe doesn't touch the WD register at all when
     * disabled. */
#if HEALTH_HWDT_ENABLED
    CyU3PSysWatchDogClear();
#endif
}

void health_record_event(health_event_t event)
{
    /* O(1).  Safe from any thread context including USB callbacks
     * and timer ISRs.  All branches must remain non-blocking. */
    switch (event) {
        case HEALTH_EVENT_EP0_HANDLER_ENTER:
            /* Order matters: stamp time first, then raise flag, so a
             * reader observing in_progress=1 always sees a fresh
             * enter_ms (volatile blocks compiler reorder). */
            glHealthState.ep0_handler_enter_ms = CyU3PGetTime();
            glHealthState.ep0_handler_in_progress = 1;
            break;
        case HEALTH_EVENT_EP0_HANDLER_EXIT:
            glHealthState.ep0_handler_in_progress = 0;
            break;
    }
}

health_status_t health_evaluate(void)
{
    /* PR 2: one signal — EP0 handler duration vs timeout. */
    if (glHealthState.ep0_handler_in_progress) {
        uint32_t now = CyU3PGetTime();
        uint32_t enter = glHealthState.ep0_handler_enter_ms;
        /* Unsigned subtraction is wraparound-safe for any handler that
         * actually completes within ~24 days (CyU3PGetTime ms rollover). */
        if ((now - enter) >= EP0_HANDLER_TIMEOUT_MS) {
            return HEALTH_WEDGED_EP0;
        }
    }
    return HEALTH_OK;
}

void health_recover(health_status_t status)
{
    /* See PLAN_RECOVERY.md §4 for the documented cascade levels.
     * PR 2 implements Level 4 only.  Future PRs add Levels 2, 3, 5
     * and migrate Level 1 (streaming watchdog). */
    switch (status) {
        case HEALTH_WEDGED_EP0:
            /* Level 4 — full device reset.  The vendor callback thread
             * is deadlocked inside an SDK call; only escape is to drop
             * USB entirely and re-enumerate.  Host will see disconnect
             * + bootloader-PID device; firmware must be re-uploaded.
             * No DebugPrint before reset — UART output is fine but the
             * SDK call may itself be the thing hung, so avoid waiting. */
            CyU3PDeviceReset(CyFalse);
            /* Should not return. */
            break;

        case HEALTH_OK:
        case HEALTH_WEDGED_UNKNOWN:
        default:
            /* No remedy for unknown wedge — deliberately do nothing
             * rather than guess.  Future PRs may add a catch-all
             * remedy (e.g. cascade through Levels 2 -> 3 -> 4) once
             * we have signals that can produce HEALTH_WEDGED_UNKNOWN. */
            return;
    }
}
