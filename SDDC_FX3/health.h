/*
 * health.h — recovery state machine interface
 *
 * Owns every recovery decision for the firmware.  All recovery-related
 * code goes through this interface; no parallel mechanisms; no inline
 * cleanup in handlers.
 *
 * Three functions form the contract:
 *
 *   health_record_event()  — called by code that observes a liveness
 *                            event (EP0 callback exit, DMA progress,
 *                            etc.).  Cheap; safe from any thread.
 *
 *   health_evaluate()      — called periodically from the main loop.
 *                            Pure: examines accumulated state, returns
 *                            health status.  Does NOT take action.
 *
 *   health_recover()       — called when health_evaluate() returns
 *                            unhealthy.  Selects a remedy from the
 *                            cascade based on the failure reason.
 *
 * Adding a new failure-mode detection means adding a new event type
 * and a new evaluation branch — NOT writing a new watchdog.
 *
 * See docs/archive/PLAN_RECOVERY.md for the original design notes
 * and recovery cascade table.
 */

#ifndef _INCLUDED_HEALTH_H_
#define _INCLUDED_HEALTH_H_

#include "cyu3types.h"

/* Liveness events recorded by callers.  Add new event types here as
 * new failure modes are addressed. */
typedef enum {
    HEALTH_EVENT_EP0_HANDLER_ENTER = 0,  /* USB vendor callback entered */
    HEALTH_EVENT_EP0_HANDLER_EXIT,       /* USB vendor callback returned */
} health_event_t;

/* Health status returned by health_evaluate().  Add new wedged-*
 * statuses as recovery cascade levels are implemented. */
typedef enum {
    HEALTH_OK = 0,
    HEALTH_WEDGED_EP0,        /* Vendor callback hung in SDK call (issue #104, #105) */
    HEALTH_WEDGED_UNKNOWN,    /* Unidentified wedge — fall-through case */
} health_status_t;

/* One-time initialization.  Called once at firmware boot before any
 * other health_*() function.  Configures the FX3 hardware watchdog
 * timer (Level 5 catastrophic backstop) — see
 * docs/archive/PLAN_RECOVERY.md §4. */
void health_init(void);

/* HWDT pet is handled internally by a ThreadX timer set up in
 * health_init() — per Infineon KB223337 the documented pattern is a
 * timer-callback Clear, not a main-thread call.  Callers should not
 * pet the watchdog directly; if the main thread is alive enough to
 * service the timer, HWDT stays satisfied.
 *
 * To make Level 5 catch main-thread death (not just timer/scheduler
 * death), the main loop must call health_main_heartbeat() on every
 * iteration.  The WD-clear timer callback only pets HWDT when this
 * heartbeat counter has advanced since its last check.  If the main
 * thread hangs, the heartbeat freezes, the timer stops petting, and
 * HWDT fires within HWDT_PERIOD_MS. */

/* Bumped by the main loop on every iteration (~10 Hz).  Internally
 * compared by the WD timer callback against the previous value to
 * decide whether to pet HWDT.  Safe to call from any thread. */
void health_main_heartbeat(void);

/* TEST-ONLY: when set non-zero (via the HANGMAIN vendor command),
 * causes the main loop to spin forever on its next iteration.  Used
 * by the host-side test_main_recovery scenario to validate the HWDT
 * reset round-trip end-to-end. */
extern volatile uint8_t glHealthHangMain;

/* Read the boot counter — incremented once per firmware boot inside
 * health_init().  Exposed via GETSTATS so the host can detect that the
 * device reset between two snapshots, regardless of the cause (Level 4
 * CyU3PDeviceReset, Level 5 HWDT, manual RESETFX3, power cycle).
 * Reset to 0 by every cold boot — no NVRAM persistence. */
uint32_t health_boot_count(void);

/* Record a liveness event.  Safe from any thread (main, USB callback,
 * DMA callback, timer ISR).  O(1). */
void health_record_event(health_event_t event);

/* Examine accumulated state and return current health.  Pure: no
 * recovery action taken here.  Called periodically (~10 Hz) from the
 * main loop. */
health_status_t health_evaluate(void);

/* Apply a remedy from the cascade based on the given status.  Called
 * by main loop when health_evaluate() returns anything other than
 * HEALTH_OK.  May call CyU3PDeviceReset() for catastrophic statuses. */
void health_recover(health_status_t status);

#endif /* _INCLUDED_HEALTH_H_ */
