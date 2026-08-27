/*******************************************************************************
  PTP follower on the 10BASE-T1S segment: receive and measure

  File Name:
    ptp_follower.h

  Summary:
    Receives the grandmaster's Sync + Follow_Up on eth0, pairs them by
    sequenceId, computes the offset between this device's wall clock and the
    master's, and steers the clock onto it: rate through MAC_TI/MAC_TISUBN, phase
    through one-shot MAC_TA adjustments. Measured on the bench: FINE reached in
    about four seconds and held, residual offset -189 to +249 ns.

  Description:
    One-way sync: this device sends nothing. What arrives is

      Sync       - carries no useful time in two-step mode, but its ARRIVAL is
                   timestamped by the LAN8651 at the end of the SFD on the MDI.
                   That instant is t2.
      Follow_Up  - carries the master's egress instant t1 of the matching Sync in
                   preciseOriginTimestamp, paired by sequenceId.

    From those two, and a path delay that is a constant by design rather than
    measured (LAN8651_TIME_SYNC.md 11.4):

      offset = t2 - t1 - D_const

    The servo has five states (LAN8651_TIME_SYNC.md 7). The order matters: the rate
    is corrected BEFORE the clock is set, because a clock set on a wrong rate walks
    away again immediately.

      UNINIT     nominal increment, collect samples, estimate the rate
      MATCHFREQ  first rate correction applied, then hard-set the clock
      HARDSYNC   large one-shot MAC_TA steps, plus rate trim
      COARSE     |offset| < 300 ns, FIR3-filtered half steps
      FINE       |offset| <= 150 ns

    "ptpf servo off" leaves everything measuring but never writes the clock, which
    is the mode to use when the numbers are the point.

    A large absolute offset before the clock is set is expected and not an error:
    both clocks count from their own power-up, so the difference is essentially the
    difference in uptime. MATCHFREQ removes it in one write. What remains after
    that, and what "ptpf status" reports, is the offset's change per sample - the
    frequency error - and the residual spread once locked.

    What the servo cannot remove is the constant part of the assumed path delay: it
    drives the MEASURED offset to zero, and that measurement is shifted by
    (real delay - D_const) by construction. That is the deliberate cost of one-way
    sync, not a control error.

  Two things worth knowing about the receive path:
    - The timestamp only exists inside the driver's receive callback and is only
      unambiguously paired with its frame there, so a small hook in
      drv_lan865x_api.c hands frame and timestamp to this module together. That
      hook is a patch in GENERATED code: an MCC "Generate Code" run removes it
      silently, and the symptom is that Sync frames still arrive while every
      timestamp is missing. Same trap as the bridge's port mirror - see CLAUDE.md
      section 6.
    - Receive timestamping needs FTSE *and* FTSS in OA_CONFIG0. FTSE alone makes
      the receive path eat four bytes of payload, silently
      (LAN8651_TIME_SYNC.md 10.3). This module sets both when it starts.
 *******************************************************************************/

#ifndef PTP_FOLLOWER_H
#define PTP_FOLLOWER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PTP over Ethernet, IEEE 1588 Annex F. */
#define PTP_FOL_ETHERTYPE       0x88F7u

/* Assumed master->follower path delay. Deliberately a constant, never measured:
   no Delay_Req/Delay_Resp is implemented at any stage. 3788 ns is the reference
   figure for this silicon pair (LAN8651_TIME_SYNC.md 11.4), of which only about
   2.5 ns is the 50 cm of cable - the rest is PHY and MAC latency, so it travels
   with the parts rather than with the topology. */
#define PTP_FOL_D_CONST_NS      3788

/* Register the console command group ("ptpf", plus "ptpfhelp"). Call once, after
   SYS_CMD is up. */
void PTP_FOL_Initialize(void);

/* Enable/disable frame timestamping and drain the receive queue. Call from the
   main loop, in a state where LAN865x register access is serviced. */
void PTP_FOL_Tasks(void);

/* Start/stop listening. Start enables FTSE+FTSS; stop clears them again, so a
   stopped follower leaves the device as it was found. */
bool PTP_FOL_Start(void);
void PTP_FOL_Stop(void);
bool PTP_FOL_IsRunning(void);

/* Hook called from the LAN865x driver's receive callback with the frame and its
   receive timestamp (NULL if the frame carried none). Runs in driver context:
   it filters, copies and returns - no printing, no SPI, no blocking. */
void ptp_follower_rx_hook(const uint8_t *frame, uint16_t len, const uint64_t *rxTimestamp);

#ifdef __cplusplus
}
#endif

#endif /* PTP_FOLLOWER_H */
