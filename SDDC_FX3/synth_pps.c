/*
 * synth_pps.c — software-driven in-band PPS marker (issue #125)
 *
 * Implementation of the synthetic-PPS commit mechanism declared in
 * synth_pps.h.  The module owns one ThreadX timer; on tick (or one-shot
 * call) it forces a partial commit on the active producer socket of
 * the existing AUTO_MANY_TO_ONE streaming DMA channel via
 * CyU3PDmaMultiChannelSetWrapUp().  The resulting partial DMA buffer
 * becomes a short USB bulk transfer on EP1 IN.
 *
 * Active-socket selection: the channel has two producer sockets in
 * ping-pong, only one is "filling" at any instant.  Rather than thread
 * socket-index info through DmaCallback (the existing callback is
 * registered with a CyU3PDmaMultiCallback_t cast and doesn't carry
 * socket info), the tick attempts SetWrapUp on socket 0 first and
 * falls back to socket 1 if that errors.  Exactly one succeeds when
 * streaming is active; both fail when the channel is between buffers
 * or streaming is stopped.
 */

#include "Application.h"
#include "synth_pps.h"

/* DMA multi-channel handle defined in StartStopApplication.c.  This is
 * the producer-to-USB channel that synth_pps targets. */
extern CyU3PDmaMultiChannel glMultiChHandleSlFifoPtoU;

/* Streaming-active flag defined in StartStopApplication.c.  We gate
 * the wrap-up path on this — a tick that fires while streaming is
 * stopped is a no-op, not a fault. */
extern CyBool_t glIsApplnActive;

/* Diagnostic counters declared in synth_pps.h. */
uint32_t glPpsCount = 0;
uint32_t glPpsCommitFailCount = 0;

/* INSTRUMENTATION (issue #125): last CyU3PReturnStatus_t returned by
 * the two CyU3PDmaMultiChannelSetWrapUp calls in commit_once.  Surfaced
 * via GETSTATS so the host can read which SDK error path we're hitting
 * (DebugPrint from CyU3PTimer callback context is silently dropped —
 * stack constraints).  Revert before the actual A3 mechanism fix. */
uint8_t glPpsLastWrapS0 = 0xFF;
uint8_t glPpsLastWrapS1 = 0xFF;

static CyU3PTimer glSynthPpsTimer;
static CyBool_t   glSynthPpsTimerCreated = CyFalse;

/* Internal: attempt one partial commit.  Returns CY_U3P_SUCCESS if
 * at least one producer socket accepted the wrap-up; otherwise a
 * non-success status.  Updates the diagnostic counters. */
static CyU3PReturnStatus_t synth_pps_commit_once(void)
{
    if (!glIsApplnActive) {
        glPpsCommitFailCount++;
        return CY_U3P_ERROR_NOT_STARTED;
    }

    /* Try producer socket 0 first; if it errors (= the other socket
     * is the active one), try socket 1.  Exactly one is filling at
     * any instant in ping-pong, so first-success is correct. */
    CyU3PReturnStatus_t s0 = CyU3PDmaMultiChannelSetWrapUp(
                                &glMultiChHandleSlFifoPtoU, 0);
    glPpsLastWrapS0 = (uint8_t)s0;  /* INSTRUMENTATION (#125), see header */
    if (s0 == CY_U3P_SUCCESS) {
        glPpsCount++;
        return CY_U3P_SUCCESS;
    }
    CyU3PReturnStatus_t s1 = CyU3PDmaMultiChannelSetWrapUp(
                                &glMultiChHandleSlFifoPtoU, 1);
    glPpsLastWrapS1 = (uint8_t)s1;  /* INSTRUMENTATION (#125), see header */
    if (s1 == CY_U3P_SUCCESS) {
        glPpsCount++;
        return CY_U3P_SUCCESS;
    }

    /* Both sockets refused.  Channel is between buffers, was reset,
     * or hit a transient state.  Count the miss but don't escalate —
     * the next tick should normally land. */
    glPpsCommitFailCount++;
    return s0;
}

/* ThreadX timer callback context — keep lightweight, no blocking SDK
 * calls.  SetWrapUp is documented as safe from any thread context. */
static void synth_pps_tick(uint32_t param)
{
    (void)param;
    (void)synth_pps_commit_once();
}

CyU3PReturnStatus_t synth_pps_init(void)
{
    if (glSynthPpsTimerCreated) {
        return CY_U3P_SUCCESS;
    }

    /* Create the timer in NO_ACTIVATE state.  Initial / reschedule
     * ticks are set to a placeholder; synth_pps_start() will
     * reprogram before starting.  Tick == 1 ms per cyu3os.h. */
    uint32_t r = CyU3PTimerCreate(&glSynthPpsTimer,
                                  synth_pps_tick,
                                  0,                  /* callback arg unused */
                                  1000,               /* initialTicks (placeholder) */
                                  1000,               /* rescheduleTicks (placeholder) */
                                  CYU3P_NO_ACTIVATE);
    if (r != CY_U3P_SUCCESS) {
        return (CyU3PReturnStatus_t)r;
    }
    glSynthPpsTimerCreated = CyTrue;
    return CY_U3P_SUCCESS;
}

CyU3PReturnStatus_t synth_pps_start(uint16_t period_ms)
{
    if (!glSynthPpsTimerCreated) {
        return CY_U3P_ERROR_NOT_STARTED;
    }
    /* Stop+modify+start is required by the SDK — CyU3PTimerModify
     * only works on a stopped timer. */
    (void)CyU3PTimerStop(&glSynthPpsTimer);
    uint32_t r = CyU3PTimerModify(&glSynthPpsTimer,
                                  (uint32_t)period_ms,
                                  (uint32_t)period_ms);
    if (r != CY_U3P_SUCCESS) {
        return (CyU3PReturnStatus_t)r;
    }
    r = CyU3PTimerStart(&glSynthPpsTimer);
    return (CyU3PReturnStatus_t)r;
}

CyU3PReturnStatus_t synth_pps_stop(void)
{
    if (!glSynthPpsTimerCreated) {
        return CY_U3P_SUCCESS;
    }
    uint32_t r = CyU3PTimerStop(&glSynthPpsTimer);
    return (CyU3PReturnStatus_t)r;
}

CyU3PReturnStatus_t synth_pps_oneshot(void)
{
    return synth_pps_commit_once();
}
