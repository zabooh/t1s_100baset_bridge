/*******************************************************************************
  Grid divider: LED1 blinks off the grid, with no CPU

  File Name:
    grid_div.h

  Summary:
    TC6 counts the grid events in hardware and flips LED1 every Nth one -
    PD10 keeps running unchanged at full rate, N is settable at runtime.

  Description:
    WHAT FOR, AND WHY NOT THE EXISTING TRIGGER ACTION.

    Action 4 toggles LED1 from the interrupt.  That is visible and good
    enough for a demonstration, but it is not the mechanism this project
    uses to generate signals: for PD10 the counter compare raises an event,
    the event system carries it, and the pin toggles itself - the CPU is no
    longer involved past the compare, so the edge is load-independent.

    The same path for the LED fails on a small thing: putting the same event
    on the LED too costs almost nothing (three register fields in the other
    PORT group, measured in test_results.md E63) - but the LED then toggles
    at the GRID RATE.  At a 100 us grid that is 5 kHz, which looks like
    "always on".

    So a DIVIDER, and it belongs in hardware too, or the CPU is back in the
    loop:

        TC1 MC0  (the grid)
          |- EVSYS channel 0, ASYNCHRONOUS   -> PORT_EV_0 -> group 3, PD10, TGL
          |                                     (unchanged, full rate)
          |- EVSYS channel 4, RESYNCHRONIZED -> TC6_EVU
          |      TC6: COUNT16, WAVEGEN = MFRQ, CC0 = N-1,
          |           EVCTRL = TCEI | EVACT_COUNT | OVFEO
          |      counts N grid points, resets, emits ONE event
          '- EVSYS channel 5, ASYNCHRONOUS   -> PORT_EV_2 -> group 2, PC21, TGL

    WHY TC6 NEEDS A CHANNEL OF ITS OWN: one channel has ONE path for all its
    users.  The PORT path is deliberately asynchronous (shortest way from
    compare to edge), while a TC event user runs resynchronized in this
    project (`peer_capture.c`) - and a resynchronized channel additionally
    needs its own GCLK.  Two channels on the SAME generator are the normal
    case.

    RATE: the LED toggles every Nth grid point, so a full blink cycle is
    2 * N * grid period.  At 100 us and N = 2500: a toggle every 250 ms,
    500 ms cycle, 2 Hz.  TC6 is 16-bit, N <= 65536, so up to a 6.5 s toggle
    interval.

    AND A PROPERTY THAT MUST BE STATED IN THE COMMAND: the divider counts
    GRID POINTS, not seconds.  If the grid period changes, the blink rate
    changes with it.  That is correct, but it looks like a defect if nobody
    says so.

    AND THE LED IS NOT A MIRROR OF PD10 - ONLY IN RATE, NOT IN PHASE.

    A wrong claim used to stand here ("the LED IS a mirror of the grid"),
    and it was caught at the bench: PD10 of both boards exactly matched,
    but the LEDs were visibly IN ANTIPHASE.  Dividing throws phase
    information away.  The pin action is literally a toggle (`EVACT = TGL`),
    a level is never written - so what determines the LED state are two
    purely LOCAL quantities: the state of `TC6.COUNT` and the level of PC21
    at the moment it was switched on.  Neither is a function of absolute
    time.  If two boards' COUNT states differ by N, the LEDs sit exactly in
    antiphase even though the grid is correct.

    HENCE THE ALIGNMENT: `GDIV_AlignRequest()` / `GDIV_AlignTick()` set the
    count phase AND the level from the GRID INDEX, i.e. from the same
    absolute quantity action 4 takes its parity from (E24).  After that, all
    boards with the same index have the same LED state.  It is requested at
    the three points where the reference changes: switching the divider on,
    arming the grid, and completing a phase pull-in.

    WHAT THE ALIGNMENT DOES NOT DO, and that is the difference from action 4:
    it runs only ON REQUEST, not at every grid point.  If TC6 loses a grid
    event, the offset stays until the next request.  With action 4 the
    self-healing is free (the level is computed anyway); here it would cost
    a 64-bit modulo per grid point - 10,000/s at a 100 us grid, and the
    re-arm path already sets the period floor (E3.1).  So deliberately not
    done.
*******************************************************************************/

#ifndef GRID_DIV_H
#define GRID_DIV_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set up the clock, the timer and the two event channels.  Once, at
   start-up.  The divider is OFF afterwards - the LED still belongs to
   `tbase led` until the first `tbase div <n>`. */
void GDIV_Initialize(void);

/* Set the divider and switch it on: LED1 flips every `n`th grid point.
   `n` = 1..65536.  Takes the claim on LED1; returns false if someone else
   already holds it (then NOTHING is written and nothing changes). */
bool GDIV_Set(uint32_t n);

/* Divider off, LED off, claim released.  `tbase led` is free again
   afterwards. */
void GDIV_Off(void);

/* REQUEST an alignment.  From task OR interrupt context, costs only a flag.
   It is carried out at the next suitable grid point, in GDIV_AlignTick(). */
void GDIV_AlignRequest(void);

/* From the grid interrupt, with the GRID INDEX of the point just armed.
   With no pending request it is a flag test and returns - so normal
   operation costs nothing.

   Applied only when `idx % n` is in the MIDDLE between two toggles: right
   at the overflow the own level write would race the hardware's toggle for
   the same instant, and the result would be inverted - exactly the kind of
   fault this function is meant to fix. */
void GDIV_AlignTick(uint64_t grid_idx);

/* Called from `trig_stop_clean()`: the trigger has stopped, so no more grid
   events arrive.  The divider stays SET, but the LED is switched off -
   otherwise it would stay in whatever state the last grid point left it in,
   and that looks like a stuck divider. */
void GDIV_GridStopped(void);

/* `tbase div [n|off]`.  Served from the 'tbase' group, because
   MAX_CMD_GROUP is exhausted and a group of its own would be SILENTLY
   refused via SYS_CMD_ADDGRP. */
bool GDIV_CliTry(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* GRID_DIV_H */
