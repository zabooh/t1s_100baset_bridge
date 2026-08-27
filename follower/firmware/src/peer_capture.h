/*******************************************************************************
  peer_capture.h - "how far is my neighbour from me", with no instrument

  WHAT THE MODULE DOES

  It captures the NEIGHBOUR BOARD's trigger edge in hardware and compares it
  against the grid point this board itself computed.  The result is the
  phase offset between two boards, in nanoseconds, on the console - the same
  quantity a logic analyser otherwise forms from two channels.

  Wiring (PEER_CAPTURE_PLAN.md 2), a crossed two-wire cable plus ground:

      Board A  EXT1 pin 5 (PD10, out) ----> EXT1 pin 6 (PD11, in)  Board B
      Board B  EXT1 pin 5 (PD10, out) ----> EXT1 pin 6 (PD11, in)  Board A
      Board A  EXT1 pin 2 (GND)       ----- EXT1 pin 2 (GND)       Board B

  In addition, and with NO wire at all: the phase of the own grid against
  the own second (`tbase sec`).  Both numbers sit in the same controller, it
  is a subtraction.  It answers the question the cross-measurement
  fundamentally cannot - WHICH of the two boards has moved -, because a
  common-mode error vanishes in the difference of two boards.

  THE ARITHMETIC, briefly.  Each board measures its neighbour plus the
  propagation delay of the path to it:

      phase_A = (t_B - t_A) + d_BA          phase_B = (t_A - t_B) + d_AB

  With equal-length paths (same firmware, same wire) that yields TWO numbers
  instead of one:

      (phase_A - phase_B) / 2  =  the true offset, with the delay removed
      (phase_A + phase_B) / 2  =  the delay itself, measured

  The same trick PTP uses to determine its line delay.  The price is
  symmetry: an asymmetry in the paths enters the measured offset at HALF its
  magnitude and cannot be told apart from a real offset.

  WHY NO INTERRUPT

  At a 100 us grid, 10,000 edges arrive per second.  An interrupt per edge
  would be the dominant disturbance - E41..E44 measured that two clock reads
  at 30 kHz worsened the phase error from 2.4 us to 10..45 us.  So the EIC
  only emits one EVSYS event, TC4 latches the count, and the value is
  fetched from the main loop.  It is SAMPLED, not counted: of 10,000 edges
  per second, any one is good enough, because they all carry the same
  phase.

  Hence the counter must be 32 bit.  16 bit at 60 MHz wraps every 1092 us,
  so a value read from the main loop could no longer be tied to one instant
  unambiguously.  TC4 in 32-bit mode (consumes TC5) does not wrap until
  71.6 s.

  WHAT THE MODULE DEPENDS ON - completely, so the encapsulation is checkable:

    hw_shared.h     claim the APB clock, pin mux, EIC (shared registers)
    board_pins.h    which pin (a board fact, not a controller fact)
    ptp_trigger.h   PTP_TRIG_GridRef()  - the grid point measured against
    pps_capture.h   PPS_LastEdge()      - the own second, for `tbase sec`
    SYS_TIME, SYS_CONSOLE, SYS_CMD

  It owns: EXTINT6, EVSYS channel 3, TC4+TC5, PD11, and all of its own
  statistics.  Nobody else touches these.  Conversely, this module reaches
  into nothing else - the two access functions above read, they set
  nothing.
*******************************************************************************/

#ifndef PEER_CAPTURE_H
#define PEER_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One evaluated measurement series.  `valid` is false as long as no usable
   sample is available - then the numbers are not zero, they are
   meaningless, and the caller must not display them. */
typedef struct
{
    bool     valid;
    int32_t  last_ns;
    int32_t  median_ns;
    int32_t  mad_ns;        /* median of the absolute deviations                 */
    int32_t  min_ns;
    int32_t  max_ns;
    uint32_t samples;       /* how many samples went into this evaluation        */
    uint32_t total;         /* how many there were in total                      */
    uint32_t age_ms;        /* how old the most recent sample is                 */
    bool     saturated;     /* one sample sat at half the grid period            */
} PEER_PHASE;

void PEER_Initialize(void);
void PEER_Tasks(void);

/* Subgroups of `tbase`: "peer" and "sec".  true = handled. */
bool PEER_CliTry(int argc, char **argv);

/* Programmatic access - for stage 2 of the plan, where the follower reports
   its measurement to the bridge over T1S and a table forms there. */
bool PEER_PeerPhase(PEER_PHASE *out);
bool PEER_SecondPhase(PEER_PHASE *out);

#ifdef __cplusplus
}
#endif

#endif /* PEER_CAPTURE_H */
