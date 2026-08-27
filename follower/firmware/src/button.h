/*******************************************************************************
  Button press, dated on the master's time

  File Name:
    button.h

  Summary:
    Capture SW1 (PD00) and SW2 (PD01) via EXTINT, convert the moment to
    grandmaster time, and report it to the bridge unsolicited.

  Description:
    THE LOAD-BEARING IDEA: the interrupt does NOT ask the master's clock, it
    only takes the local tick.  The conversion happens afterwards, in the
    main loop.

    This is not an economy measure, it is the only way.  The wall clock sits
    in the LAN8651 behind SPI - a register access takes milliseconds and is
    refused if one is already running.  The follower does not read it at
    all; it has a MODEL of it (anchor plus slope, fed from the Sync/Follow_Up
    pairs or the 1PPS).  `PTP_TB_Convert()` is the mapping local tick ->
    grandmaster ns, valid for any tick past or future, and is pure
    arithmetic with no I/O.

    And `PTP_TB_Now()` in the interrupt would be the time of HANDLER ENTRY,
    not of the edge - exactly the latency one is trying to get rid of.

    `pps_capture.c` uses the same pattern once a second; the latency is
    measured there too (median 575 ns, spread 950 ns, test_results.md E9).
    For a button this is moot: mechanical bounce is milliseconds, three
    orders of magnitude above that.  What it needs instead is DEBOUNCING -
    without it the timestamp dates the bounce.

    WHAT THIS MODULE DOES NOT DO: it sends nothing itself.  The wire format,
    the raw sender, the node number and the master MAC live in `trig_cmd.c`,
    and stay there - this module recognises and dates, that one sends
    (`TRIGCMD_ButtonEvent()`).
*******************************************************************************/

#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Claim the pins, pull-ups and the two EXTINTs.  Once, at start-up.
 *
 * DELIBERATELY NOT AT RUNTIME: a write to EIC_CONFIG briefly takes the
 * whole EIC instance offline and hits EVERY other EXTINT while doing so -
 * including the 1PPS on EXTINT12.  The reasoning is at HW_EicSense() in
 * hw_shared.h. */
void BTN_Initialize(void);

/* Main loop: take a pending press, debounce it, convert it and have it
   reported.  Never blocks, does no I/O in interrupt context. */
void BTN_Tasks(void);

/* Served from the 'tbase' group, like the other submodules: MAX_CMD_GROUP
   in the generated sys_command.h is 8 and both projects sit at the ceiling,
   so a group of its own would be refused - silently.  Returns true if it
   consumed the arguments. */
bool BTN_CliTry(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_H */
