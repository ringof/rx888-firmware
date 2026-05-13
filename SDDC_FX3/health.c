/*
 * health.c — recovery state machine implementation
 *
 * PR 2: adds EP0 vendor-callback liveness tracking and Level 4
 * (CyU3PDeviceReset) backstop.  Addresses issues #104, #105.
 *
 * See health.h for the contract and PLAN_RECOVERY.md for design notes.
 *
 * The discipline rule: new recovery code MUST extend one of these
 * three functions.  Do not invent parallel mechanisms.  Do not add
 * inline cleanup in handlers — record an event here and let
 * health_recover() handle it.
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

/* Accumulated state inspected by health_evaluate().  Written by the
 * USB callback thread, read by the main loop.  Single-byte/word
 * accesses are atomic on ARM926; volatile prevents compiler reordering.
 * Ordering rule: writers update timestamp BEFORE setting in_progress,
 * so a reader never sees in_progress=1 with a stale enter_ms. */
static struct {
    volatile uint32_t ep0_handler_enter_ms;
    volatile uint8_t  ep0_handler_in_progress;
} glHealthState = { 0, 0 };

void health_init(void)
{
    /* PR 3 will add HWDT setup here (CyU3PSysWatchDogConfigure) as the
     * Level 5 catastrophic-backstop. */
    glHealthState.ep0_handler_enter_ms = 0;
    glHealthState.ep0_handler_in_progress = 0;
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
