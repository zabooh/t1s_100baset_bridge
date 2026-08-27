/*******************************************************************************
  Time-triggered actions on the shared grandmaster timebase

  File Name:
    ptp_trigger.h

  Summary:
    Runs a registered handler at a commanded grandmaster instant, so several
    followers can act at the same moment.  Phase C of PTP_TIMEBASE_PLAN.md.

  Description:
    A handler cannot be called by hardware - a callback is software, so the
    interrupt path is in the budget: exception entry on this Cortex-M4 at
    120 MHz is ~100 ns, plus ISR prologue, plus in the worst case the longest
    critical section anywhere in the firmware.  That last term lives in the
    TCP/IP stack and the LAN865x driver and is not estimable, only measurable -
    which is why every handler is handed the lateness of its own call.

    Two layers, so precision and work do not have to compete (plan C.1):
      - the hardware pin (phase E) fires at the instant, with no software
      - the handler runs as soon after as the CPU manages, and is told how late

    Deliberately NOT built here: a queue of pending triggers.  One compare, one
    target (plan C.8).  The periodic variant re-arms itself instead, and its
    phase is defined in ABSOLUTE grandmaster time - "toggle every 100 ms from
    now" would never align two nodes, however good their clocks, because the
    phase would depend on when each one heard the command (plan G.1).

  Which time source:
    Everything here goes through ptp_timebase.h.  Measured round trip of that
    model on this hardware: LocalFor() and Convert() agree to -8 ns one second
    out and -4 ns sixty seconds out, so the conversion is not the error term.
    The error term is the callback path, and PTP_TRIG_STATUS reports it.
*******************************************************************************/

#ifndef PTP_TRIGGER_H
#define PTP_TRIGGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTP_TRIG_ACTIONS        4u      /* registerable action slots          */

/* Minimum lead time a command must leave.  The master should plan far more
   generously (200 ms is suggested); this is the floor below which the follower
   refuses, because arming needs the model, a division and a SYS_TIME call. */
#define PTP_TRIG_MIN_LEAD_MS    50u

/* Shortest period the re-arm path sustains.
 *
 * The earlier numbers from E3.1 (20 us delivers 49,377 of 50,000 triggers,
 * 10 us only 7,006 of 100,000) are NOT wrong, but they answer the wrong
 * question: they count the triggers of ONE board.  What counts is the
 * comparison of two.
 *
 * MEASURED 2026-08-16, previously estimated at 20 us (S2.1,
 * reports/someip_S2_*).
 *
 * The old value came from E3.1, which only counted the trigger count on
 * ONE board.  With two channels on the analyser you see what that cannot
 * see - the boards lose periods AGAINST EACH OTHER:
 *
 *   grid     1PPS source                        PTP source
 *   100 us   A=B=70,862, 0 dropouts, p99 4.2 us    p99 6.4 us            both good
 *    80 us   A=B=87,741, 0 dropouts, p99 2.0 us    offset 6.3 us         1PPS only
 *    60 us   B loses 16,698 edges (14 %)           offset 12.2 us        neither
 *    50 us   4 dropouts, p99 31 us                 -39.9 us              neither
 *    20 us   half-period +998,000 ppm - the pin no longer follows the grid
 *
 * So the floor is SOURCE-DEPENDENT: 80 us on 1PPS, 100 us on the PTP
 * pairs (the seven-times-worse source, E13).  What is defined here is the
 * value that holds on BOTH, and it happens to be the project's target
 * (10 kHz) at the same time.
 *
 * Deliberately NO source-dependent bound: it would be a trap of the "it
 * worked yesterday" kind, because the source is switchable at runtime.
 * Whoever needs the 80 us on 1PPS has it measured right here and changes
 * one line.
 *
 * Why this is more than tidiness: below the floor the damage STAYS.
 * After a trip down to 20...80 us, the offset at 100 us sat at
 * +26,715 ns instead of +2200 ns - a factor of twelve, with no dropout,
 * so invisible except in comparison (S2.3).  An accepted command that
 * leaves the setup damaged afterwards is worse than a refused one.
 *
 * 2026-08-19 - THE FLOOR IS NOW AT 40 us, AND IT IS SAMPLED.
 *
 * The table above stays valid for the state it was measured against.  It
 * was measured BEFORE the three fixes made this day, and those fixes hit
 * exactly the mechanisms it was seeing:
 *
 *   - The timebase's anchor pair (s_anchor_L / s_anchor_ns) used to be
 *     written in TWO assignments and read from the trigger ISR.  Whoever
 *     hit the gap computed off by exactly one refit interval, and the
 *     catch-up loop then skipped 10,000 grid points.  Those were the
 *     "dropouts".
 *   - TC1 kept running after disarming and passed its stale CC0 on every
 *     wrap - one edge every 1092.27 us, outside any grid.
 *   - The lost SYS_TIME overflow shifted time by a permanent 65,536
 *     ticks.
 *
 * All three are source-independent and therefore affected EVERY row of
 * the old table.  Lowering the bound is therefore a testable claim ("the
 * causes are gone"), not a relaxation ("the bound was too strict").
 *
 * THE SAMPLING SERIES.  The task was "allow 50 us"; to find out whether
 * 50 IS the right value, the lockout was temporarily opened to 5 us.  One
 * fleet reset per point (a trip below the floor leaves damage that would
 * otherwise ruin the next point), then a 12 s capture with two channels
 * at 50 MS/s.  Both followers on 1PPS.
 *
 *   period   skipped A/B         channels agree   median gap    p90/p99
 *    50 us      0 / 0                 yes           0.050 ms      1440/1820 ns
 *    40 us      0 / 0                 yes           0.040 ms       600/ 840 ns
 *    40 us      0 / 0                 yes           0.040 ms       600/ 840 ns   (run 2)
 *    35 us      4 / 8                 yes           0.035 ms      2060/2460 ns
 *    35 us      6 / 10                NO (1 off)    0.035 ms       360/ 640 ns   (run 2)
 *    30 us     14 / 26                NO (2 off)    0.030 ms      1280/1700 ns
 *    25 us     12 / 25                yes           0.025 ms      3000/3260 ns
 *    20 us   317,972 / 632,327        yes           0.040 ms(!)   1300/1620 ns
 *
 * THE LOAD-BEARING CRITERION IS THE MECHANISM, NOT p99: "no skipped
 * period" and "both channels carry the same edge count".  At 40 us both
 * hold, twice; at 35 us neither ever does.  Going by p99 alone would have
 * been misleading - the second 35 us run has the BEST value of the whole
 * series at 640 ns while skipping 6 and 10 periods respectively.
 *
 * The 20 us row is the clearest: the median gap is 0.040 ms against a
 * commanded period of 0.020 ms - the pin only manages every SECOND grid
 * point and no longer follows the grid.  That matches the old measurement
 * ("half-period +998,000 ppm").
 *
 * WHAT p99 CANNOT BE READ AS: at 40 us it sits (840 ns, twice) below the
 * value at 50 us (1820 ns), and that does NOT mean 40 us is better than
 * 50 us.  Twelve seconds is shorter than the wander that dominates the
 * result - a spread without its observation duration is not a statement,
 * and over 30 s, 50 us produced 1720 ns.  The series decides the FLOOR,
 * not the quality.
 *
 * RE-MEASURE with
 *   python scripts/saleae_skew.py --ch-a 1 --ch-b 0 --ch-master 3 --period-ms 0.04 --edges all
 * and read p99, not peak-to-peak: below ~1 ms period the spread
 * saturates at the pairing bound of half a period.  `--edges all` is
 * mandatory - the pin TOGGLES, and the polarity carries the parity of the
 * trigger counter, so two boards can be trigger-synchronous and still out
 * of phase.
 *
 * WHAT STILL HOLDS: below the floor the damage STAYS (S2.3).  That is why
 * it is a hard bound and not a warning, and why the fleet is reset
 * between two measurement points.  An accepted command that leaves the
 * setup damaged is worse than a refused one. */
#include "trig_someip.h"        /* the number lives in the protocol contract */
#define PTP_TRIG_MIN_PERIOD_US  TRIG_MIN_PERIOD_US

/* Upper bound on how far ahead a target may sit.  Not a hardware limit at this
   stage - it is the point beyond which extrapolation of the fitted slope stops
   being trustworthy, and it keeps the phase-E compare window honest later. */
#define PTP_TRIG_MAX_LEAD_MS    600000u  /* 10 minutes */

/* Lead time `tbase burst` adds to the current time before rounding up to
   the next grid multiple.  Same value as the master's default
   (TRIGM_DEFAULT_LEAD_MS), so a locally started burst and a commanded one
   sit on the same timescale. */
#define PTP_TRIG_BURST_LEAD_MS  200u

typedef enum
{
    PTP_TRIG_OK = 0,
    PTP_TRIG_ERR_NO_ACTION,     /* nothing registered under that id           */
    PTP_TRIG_ERR_NO_TIME,       /* timebase not usable (or 'free' not allowed) */
    PTP_TRIG_ERR_PAST,          /* target already gone, or too close           */
    PTP_TRIG_ERR_TOO_FAR,       /* beyond PTP_TRIG_MAX_LEAD_MS                 */
    PTP_TRIG_ERR_DUPLICATE,     /* same action id + sequence seen before       */
    PTP_TRIG_ERR_BUSY,          /* a trigger is already armed                  */
    PTP_TRIG_ERR_ARM,           /* SYS_TIME refused the timer                  */
    /* Appended at the END on purpose: this enum travels on the wire in the
       STATUS reply, so the existing codes must keep their numbers. */
    PTP_TRIG_ERR_TOO_FAST,      /* period below what the re-arm path sustains  */
    PTP_TRIG_ERR_NO_START       /* count != 0 without an absolute start (E2)   */
} PTP_TRIG_RESULT;

/* Mode for the "timebase not usable" case (plan C.6).  strict refuses; free
   fires anyway on whatever the model says and reports the state, which phase G
   needs to show two nodes drifting apart before synchronization is switched on.
   The mode MUST be visible from outside - otherwise a free run is later mistaken
   for a synchronized one, the most expensive measurement error this feature can
   produce. */
typedef enum
{
    PTP_TRIG_MODE_STRICT = 0,
    PTP_TRIG_MODE_FREE
} PTP_TRIG_MODE;

/* scheduled_ns is the commanded instant, so a handler that timestamps or
   interpolates can work from the intended time rather than the actual one.
   late_ticks is SIGNED: arming has microsecond granularity, so firing a
   fraction early is normal and must not wrap to a huge positive number. */
typedef void (*PTP_TRIG_Handler)(uint64_t scheduled_ns, int32_t late_ticks, uintptr_t ctx);

typedef struct
{
    bool          armed;
    PTP_TRIG_MODE mode;
    uint16_t      action_id;      /* of the armed trigger                     */
    uint64_t      target_ns;
    uint64_t      period_ns;      /* 0 = one-shot                             */
    uint32_t      fired;
    uint32_t      refused;
    uint32_t      missed;         /* periods skipped after a late re-arm      */
    int32_t       late_last_ticks;
    int32_t       late_max_ticks;
    int32_t       late_min_ticks;
    uint64_t      late_sum_ticks; /* for a mean; ticks, not ns                */
    uint32_t      late_n;
    uint32_t      ticks_per_us;
    /* Internal only, not part of the wire format - hence cheap.  end_ns is
       UINT64_MAX as long as the trigger runs unlimited. */
    uint64_t      end_ns;
    uint32_t      burst_done;     /* bursts that ended regularly              */
} PTP_TRIG_STATUS;

void PTP_TRIG_Initialize(void);

/* Registers the built-in demo actions.  There is deliberately NO command group
   of its own: MAX_CMD_GROUP in the generated sys_command.h is 8 and the project
   is already at the ceiling, with no MCC symbol to raise it.  The subcommands
   are therefore served from the 'tbase' group - see PTP_TRIG_CliTry(). */
void PTP_TRIG_CliInit(void);

/* Handles the trigger subcommands of 'tbase'.  Returns true if it consumed the
   command, so the caller can fall through to its own parsing. */
bool PTP_TRIG_CliTry(int argc, char **argv);

/* Runs deferred (task-context) handlers.  Call from the main loop. */
void PTP_TRIG_Tasks(void);

/* isr_ctx = true  -> handler runs in the SYS_TIME callback: fast, but NO SPI,
                      no frame send, no SYS_CONSOLE, no blocking (plan C.3).
   isr_ctx = false -> handler is deferred to PTP_TRIG_Tasks(): anything goes,
                      at main-loop jitter. */
bool PTP_TRIG_Register(uint16_t action_id, PTP_TRIG_Handler h, uintptr_t ctx, bool isr_ctx);

/* Registered action ids as a bitmask (bit N = id N), for the STATUS reply.
   Only ids below 32 are represented. */
uint32_t PTP_TRIG_ActionMask(void);

PTP_TRIG_RESULT PTP_TRIG_ScheduleAt(uint16_t action_id, uint32_t cmd_seq, uint64_t tx_ns);

/* Periodic, phase in ABSOLUTE grandmaster time: fires at every instant where
   (t - phase_ns) is a whole multiple of period_ns.  See plan G.1 for why the
   phase cannot be relative. */
/* start_ns names the FIRST trigger point and comes from the master, so it
   does not follow from the local `now` - rationale in trig_someip.h.  0 =
   choose it yourself as before, which keeps an old master working. */
/* count = number of grid points, 0 = unlimited (today's behaviour).  The
   end is an ABSOLUTE instant start_ns + count * period_ns, computed once
   at arm time - not a counter.  Consequence, and an intended one: a board
   that skips a period emits N-1 pulses instead of pushing the end back.
   That way all boards end at the SAME grid point, and the pulse count
   becomes a verification tool (PULSE_TRAIN_PLAN.md E1).
   count != 0 requires start_ns != 0 and is otherwise refused with
   PTP_TRIG_ERR_NO_START: a self-chosen start makes the end board-dependent,
   and a burst without a shared start is not a fleet action (E2). */
PTP_TRIG_RESULT PTP_TRIG_SchedulePeriodic(uint16_t action_id, uint32_t cmd_seq,
                                          uint64_t period_ns, uint64_t phase_ns,
                                          uint64_t start_ns, uint32_t count);

/* Toggles PD10 (EXT1 pin 5) as the first act of every fire, before statistics
   and before any handler.  That edge is what the logic analyser sees, so it must
   carry as little software as possible. */
/* Toggle PD10 as a scope marker from a DEFERRED action, which runs in the main
   loop and is therefore invisible to the ISR-driven pin.  Requires `tbase pin
   off`, otherwise the ISR and this fight over the same pin. */
/* The grid event's EVSYS GENERATOR ID (today `TC1_MC_0`).
 *
 * Why: the grid divider in `grid_div.c` hangs a SECOND channel off the
 * same generator, because one channel has only ONE path - the PORT path
 * wants asynchronous, a TC event consumer here runs resynchronised.  The
 * id belongs where TC1 is configured; a second copy of
 * `EVENT_ID_GEN_TC1_MC_0` in another module would be a second source of
 * truth, and it would go silently wrong at the next timer change. */
uint8_t PTP_TRIG_GridEventGen(void);

/* Index of the most recently armed grid point.
 *
 * Why: an action that derives a LEVEL from grid position must not compute
 * it as `scheduled_ns / period`.  That is only correct as long as the
 * phase is 0 - while pulling in an own phase, the instants shift, and the
 * quotient is then NOT the index.  The index is the quantity the parity
 * depends on, so it belongs queried rather than recomputed. */
uint64_t PTP_TRIG_GridIndex(void);

void PTP_TRIG_PinMark(void);

/* DWT->CYCCNT, the debug core's cycle counter - a time source independent
   of TC0 and every peripheral (FIRMWARE_SELF_DEBUGGING.md, section 8).
   `DwtInit` turns it on AND checks that it is running; `DwtOk` reports
   that, so a silent zero does not pass as a measurement. */
void PTP_TRIG_DwtInit(void);
bool PTP_TRIG_DwtOk(void);
/* DWT->CYCCNT extended to 64 bits - the reference clock for the wrap
   correction in pps_capture.c.  Must be called more often than every 35 s;
   plentiful in the 1PPS path. */
uint64_t PTP_TRIG_Dwt64(void);

/* Root fix for E41-E45: makes up for a lost 16-bit overflow of SYS_TIME,
   using the debug core's cycle counter as an independent witness.  Called
   from `SYS_TIME_PLIBCallback()` - the one place where the loss becomes
   permanent. */
uint32_t PTP_TRIG_FixElapsed(uint32_t elapsed);
uint32_t PTP_TRIG_SysTimeRepairs(void);
/* How often TRCENA/CYCCNTENA had to be re-set - nonzero means something
   outside this firmware turned off the debug core. */
uint32_t PTP_TRIG_DwtRearms(void);

bool PTP_TRIG_ArmPin(bool enable);

/* Firing backend.  true = phase E1: SYS_TIME gets within a millisecond, then a
   dedicated TC1 compare does the last hop, retriggered with interrupts off.
   false = phase C: SYS_TIME alone, which measured 25.5 us of spread.
   Switchable at runtime so both can be measured from one flash. */
/* Pulse model: the pin is SET and CLEARED rather than toggled.  CC0
   produces the rising edge (PORT action SET), CC1 the falling one (CLR),
   both from the same retrigger.  That way a lost period costs one pulse
   instead of the polarity - the difference that turned E24 into a
   PERMANENT fault -, and it is the shape the endpoint protocol requires
   (PulseTime/IdleTime).  duty_pct 1..99, out of range is ignored. */
void PTP_TRIG_PulseSet(bool on, uint32_t duty_pct);
bool PTP_TRIG_PulseGet(uint32_t *duty_pct);

void PTP_TRIG_HwSet(bool enable);
bool PTP_TRIG_HwGet(void);

/* One grid point in local ticks plus the grid period, read atomically.
   false if nothing is armed or no period is running - then there is no
   reference, and a caller that computes anyway is inventing a phase.  For
   peer_capture.c; see the rationale at the implementation. */
bool PTP_TRIG_GridRef(uint64_t *ref_tick, uint64_t *period_ticks);

void PTP_TRIG_Cancel(void);
void PTP_TRIG_ModeSet(PTP_TRIG_MODE mode);
void PTP_TRIG_StatusGet(PTP_TRIG_STATUS *out);
const char *PTP_TRIG_ResultName(PTP_TRIG_RESULT r);

#ifdef __cplusplus
}
#endif

#endif /* PTP_TRIGGER_H */
