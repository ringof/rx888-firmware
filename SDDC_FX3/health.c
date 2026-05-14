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

/* Phase 3 of the HWDT bisection.
 *
 * Phase 1 (caabaf4, both off): 1-hour soak clean.  Confirmed touching
 *   the WD register is the trigger.
 * Phase 2 (178f93d, Configure off / Clear from main loop): planned but
 *   superseded — Infineon KB223337 documents a fundamentally different
 *   pattern than what we were testing.
 * Phase 3 (this commit): implement the KB-documented pattern verbatim.
 *
 * Per KB223337 the watchdog is petted by a ThreadX timer callback,
 * NOT by the main thread.  Pet frequency is ~3 sec, WD period ~5 sec
 * (our previous broken approach used 100 ms / 10 sec — pet was 30x
 * more often than recommended).  The clock-domain story explains why
 * register-write frequency might matter: with no external 32 KHz,
 * the WD clock is derived from SYS_CLK, the same clock driving
 * GPIF/DMA.  Per-pet contention scales with pet frequency.
 *
 * Set HEALTH_HWDT_ENABLED=0 to skip all WD activity (matches the
 * Phase 1 clean baseline). */
#define HEALTH_HWDT_ENABLED      1

#define HWDT_PERIOD_MS           5000   /* Per KB223337. */
#define HWDT_CLEAR_PERIOD_MS     3000   /* Per KB223337; must be < HWDT_PERIOD_MS. */

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

/* Main-loop heartbeat — bumped by health_main_heartbeat() on every
 * RunApplication iteration.  The WD-clear timer callback reads this
 * to decide whether to pet HWDT.  If main hangs (e.g. via HANGMAIN),
 * heartbeat freezes, timer stops petting, HWDT fires within
 * HWDT_PERIOD_MS. */
static volatile uint32_t glMainHeartbeat = 0;

/* HANGMAIN test flag — set by the vendor handler in USBHandler.c,
 * read at the top of each main-loop iteration in RunApplication.c. */
volatile uint8_t glHealthHangMain = 0;

/* ThreadX timer used to pet the HWDT.  Per KB223337 the pet must
 * happen from a timer callback context, not from the main thread. */
#if HEALTH_HWDT_ENABLED
static CyU3PTimer glWdClearTimer;
static uint32_t glLastHeartbeatSeen = 0;

static void health_wd_clear_cb(uint32_t param)
{
    (void)param;
    /* Only pet HWDT if the main thread has made progress since last
     * check.  If the heartbeat is stale, deliberately skip the pet —
     * eventually HWDT will fire and reset the device.  This is what
     * makes Level 5 catch main-thread death. */
    uint32_t now = glMainHeartbeat;
    if (now != glLastHeartbeatSeen) {
        glLastHeartbeatSeen = now;
        CyU3PSysWatchDogClear();
    }
}
#endif

void health_main_heartbeat(void)
{
    /* Single-word increment; volatile prevents compiler reorder.
     * Wraparound is fine — the timer callback only checks for
     * advance-since-last-call, which is wraparound-safe. */
    glMainHeartbeat++;
}

void health_init(void)
{
    glHealthState.ep0_handler_enter_ms = 0;
    glHealthState.ep0_handler_in_progress = 0;

    /* Increment first thing so the count reflects "this is boot N".
     * Exposed via health_boot_count() / GETSTATS for reset detection. */
    glBootCount++;

#if HEALTH_HWDT_ENABLED
    /* Level 5 — enable the FX3 hardware watchdog and create a
     * ThreadX timer to pet it.  Per Infineon KB223337 the pet must
     * come from a timer callback (not the main thread), and at a
     * rate well below the WD period.  Requires CyU3PDeviceInit to
     * have already run (it has — StartUp.c invokes it before us). */
    CyU3PSysWatchDogConfigure(CyTrue, HWDT_PERIOD_MS);
    (void)CyU3PTimerCreate(&glWdClearTimer,
                           health_wd_clear_cb,
                           0,                       /* callback arg (unused) */
                           HWDT_CLEAR_PERIOD_MS,    /* initialTicks */
                           HWDT_CLEAR_PERIOD_MS,    /* rescheduleTicks (periodic) */
                           CYU3P_AUTO_ACTIVATE);
#endif
}

uint32_t health_boot_count(void)
{
    return glBootCount;
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
