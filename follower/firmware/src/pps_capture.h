/*******************************************************************************
  1PPS capture - feed the MCU timebase from a hardware edge

  File Name:
    pps_capture.h

  Summary:
    Turns the LAN8651's 1PPS output, routed to PC12 by the hardware modification
    in LAN8651_1PPS_HARDWARE.md, into (local tick, wall clock second) pairs for
    ptp_timebase.c.

  Description:
    Why this exists.  The timebase has until now been fed with a SOFTWARE capture
    of Sync arrival - SYS_TIME_Counter64Get() called from the RX hook - whose
    spread is SPI, TC6 and interrupt latency: 14 to 28 us measured.  That spread,
    divided by the fit's baseline, is the whole residual drift of this project
    (PHASE_DRIFT_THESEN.md thesis 3).

    On 2026-08-12 the two boards' wall clocks were compared directly over their
    1PPS outputs: 40 ns apart, drift 0.000 ppm (test_results.md E6).  So the
    clocks underneath were never the problem - the software path above them is.
    This module removes that path from the measurement.

    STAGE A of that removal, deliberately: the edge is taken by an EIC INTERRUPT
    and the tick is read in the handler.  That is still software, and it still
    carries interrupt latency - but tens to hundreds of nanoseconds instead of
    tens of microseconds.  It exists to prove the whole chain end to end (edge,
    second number, submission, effect on winner spread) before EVSYS and a TC
    capture replace the handler and take it to ~10 ns.

    The wall clock second is read over SPI afterwards, without any timing
    pressure: the pulse marks a second boundary, so the second is unambiguous for
    a whole second afterwards.
*******************************************************************************/

#ifndef PPS_CAPTURE_H
#define PPS_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configures PC12 as EIC/EXTINT12 and registers the CLI. Does NOT start feeding
   the timebase - use "pps on" or PPS_FeedSet(true). */
void PPS_Initialize(void);

/* Hung off the "tbase" command group - MAX_CMD_GROUP is 8 and the project is at
   the limit.  Returns true if it handled the arguments. */
bool PPS_CliTry(int argc, char **argv);

/* Main loop: reads the wall clock for a pending edge and submits the pair. */
void PPS_Tasks(void);

/* Feed the timebase from 1PPS instead of from PTP pairs.  Switching either way
   clears the model, because the two references mean different things. */
void PPS_FeedSet(bool on);
bool PPS_FeedGet(void);

/* Edges seen since start, and edges dropped because the main loop was too slow
   to name the second unambiguously. */
uint32_t PPS_EdgeCount(void);
uint32_t PPS_DropCount(void);

/* The last second edge: tick in SYS_TIME ticks and its running number.
   false as long as no edge has arrived.  The number lets the caller tell a
   new edge apart from re-reading the same old one. */
bool PPS_LastEdge(uint64_t *tick, uint32_t *seq);

#ifdef __cplusplus
}
#endif

#endif /* PPS_CAPTURE_H */
