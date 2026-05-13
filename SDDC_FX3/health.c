/*
 * health.c — recovery state machine implementation
 *
 * PR 1 (this commit): skeleton only.  Three functions are no-ops with
 * structure in place for future additions.  No callers; no behavior
 * change at runtime.
 *
 * See health.h for the contract and PLAN_RECOVERY.md for design notes.
 *
 * The discipline rule: new recovery code MUST extend one of these
 * three functions.  Do not invent parallel mechanisms.  Do not add
 * inline cleanup in handlers — record an event here and let
 * health_recover() handle it.
 */

#include "health.h"

/* Accumulated state inspected by health_evaluate().  Grows as new
 * liveness signals are added.  Initial state is empty. */
static struct {
    int placeholder;  /* removed when first real signal lands */
} glHealthState = { 0 };

void health_init(void)
{
    /* PR 1: nothing to initialize.  PR 3 will add HWDT setup here
     * (CyU3PSysWatchDogConfigure) as the Level 5 catastrophic-backstop. */
    glHealthState.placeholder = 0;
}

void health_record_event(health_event_t event)
{
    /* PR 1: no event types defined yet.  Dispatch will go here.
     * Implementations must be O(1) and safe from any thread context
     * including USB callbacks and timer ISRs. */
    (void)event;
}

health_status_t health_evaluate(void)
{
    /* PR 1: no signals to check.  Always healthy.
     *
     * Future PRs add evaluations such as:
     *   - EP0 callback exit timestamp vs main-loop "now" → WEDGED_EP0
     *   - glDMACount stale + GPIF state ∈ {5,7,8,9} → WEDGED_STREAMING
     *     (migrated from the existing watchdog in RunApplication.c) */
    return HEALTH_OK;
}

void health_recover(health_status_t status)
{
    /* PR 1: cascade is empty.  Future PRs add cases per cascade level.
     * See PLAN_RECOVERY.md §4 for the documented escalation ladder.
     *
     *   case HEALTH_WEDGED_EP0:       CyU3PDeviceReset(CyFalse);
     *   case HEALTH_WEDGED_STREAMING: <migrated streaming watchdog>;
     */
    switch (status) {
        case HEALTH_OK:
            return;
        case HEALTH_WEDGED_UNKNOWN:
        default:
            /* No remedy implemented yet.  Deliberately do nothing
             * rather than guess at a remedy that may not match the
             * actual failure mode.  Future PRs add real remedies. */
            return;
    }
}
