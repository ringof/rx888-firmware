/*
 * synth_pps.h — software-driven in-band PPS marker (issue #125)
 *
 * Forces a partial DMA commit on the active streaming buffer at a
 * scheduled cadence (or one-shot).  The resulting short USB bulk
 * transfer is detectable on the host as `actual_length < length`
 * on the IN endpoint, and locates the marker to within zero samples
 * in the sample stream — the in-band timing marker described in
 * docs/PLAN_SYNTH_PPS.md and issue #125.
 *
 * Mechanism is driven by an ARM-side CyU3PTimer plus
 * CyU3PDmaMultiChannelSetWrapUp() on the producer sockets of the
 * existing AUTO_MANY_TO_ONE DMA channel.  This is the canonical
 * partial-commit idiom documented in AN75779 §5.7.
 *
 * Permanent diagnostic, safe in production.  See protocol.h SYNTH_PPS
 * for the vendor-command wire format that drives this module.
 */

#ifndef _INCLUDED_SYNTH_PPS_H_
#define _INCLUDED_SYNTH_PPS_H_

#include "cyu3types.h"

/* One-time initialization.  Called once at firmware boot before any
 * other synth_pps_*() function.  Creates the ThreadX timer (initially
 * stopped) and zeroes the diagnostic counters.  Idempotent.
 *
 * Returns CY_U3P_SUCCESS on success; non-success if the underlying
 * CyU3PTimerCreate fails. */
CyU3PReturnStatus_t synth_pps_init(void);

/* Start the periodic synthetic-PPS timer with the given period in
 * milliseconds.  Accepted range 10..60000 ms; the vendor-command
 * handler is responsible for range-checking before calling this.
 * If the timer is already running, it is reconfigured to the new
 * period.
 *
 * The timer callback gates on glIsApplnActive — ticks fired while
 * streaming is stopped are no-ops, not faults.  Safe to start the
 * timer before STARTFX3.
 *
 * Returns CY_U3P_SUCCESS on success; non-success on SDK failure. */
CyU3PReturnStatus_t synth_pps_start(uint16_t period_ms);

/* Stop the periodic timer.  Safe to call when the timer isn't
 * running.  Returns CY_U3P_SUCCESS on success; non-success on SDK
 * failure. */
CyU3PReturnStatus_t synth_pps_stop(void);

/* Fire one immediate partial-commit from caller context (not via
 * timer).  Useful for bring-up and host-side verification.  Same
 * commit path as the periodic tick.  Returns CY_U3P_SUCCESS if at
 * least one producer socket accepted the wrap-up; CY_U3P_ERROR_NOT_STARTED
 * if streaming is not active. */
CyU3PReturnStatus_t synth_pps_oneshot(void);

/* Diagnostic counters exposed via GETSTATS.
 *   glPpsCount             — total successful partial commits since boot.
 *   glPpsCommitFailCount   — wrap-up attempts where both producer sockets
 *                            returned errors (= streaming idle between
 *                            buffers, or unexpected SDK fault). */
extern uint32_t glPpsCount;
extern uint32_t glPpsCommitFailCount;

/* INSTRUMENTATION (issue #125): last CyU3PReturnStatus_t returned by
 * each of the two CyU3PDmaMultiChannelSetWrapUp calls in commit_once.
 * Surfaced via GETSTATS (offsets 34, 35) so the host can see which
 * SDK error path we're hitting -- DebugPrint from CyU3PTimer callback
 * context is silently dropped (timer thread stack too small for
 * DebugPrint2USB's 256-byte scratch buf).  Revert these globals,
 * their GETSTATS slots, and host-side decoding before the actual
 * A3 mechanism fix lands. */
extern uint8_t glPpsLastWrapS0;
extern uint8_t glPpsLastWrapS1;

#endif /* _INCLUDED_SYNTH_PPS_H_ */
