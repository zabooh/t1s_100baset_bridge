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

/* Upper bound on how far ahead a target may sit.  Not a hardware limit at this
   stage - it is the point beyond which extrapolation of the fitted slope stops
   being trustworthy, and it keeps the phase-E compare window honest later. */
#define PTP_TRIG_MAX_LEAD_MS    600000u  /* 10 minutes */

typedef enum
{
    PTP_TRIG_OK = 0,
    PTP_TRIG_ERR_NO_ACTION,     /* nothing registered under that id           */
    PTP_TRIG_ERR_NO_TIME,       /* timebase not usable (or 'free' not allowed) */
    PTP_TRIG_ERR_PAST,          /* target already gone, or too close           */
    PTP_TRIG_ERR_TOO_FAR,       /* beyond PTP_TRIG_MAX_LEAD_MS                 */
    PTP_TRIG_ERR_DUPLICATE,     /* same action id + sequence seen before       */
    PTP_TRIG_ERR_BUSY,          /* a trigger is already armed                  */
    PTP_TRIG_ERR_ARM            /* SYS_TIME refused the timer                  */
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

PTP_TRIG_RESULT PTP_TRIG_ScheduleAt(uint16_t action_id, uint32_t cmd_seq, uint64_t tx_ns);

/* Periodic, phase in ABSOLUTE grandmaster time: fires at every instant where
   (t - phase_ns) is a whole multiple of period_ns.  See plan G.1 for why the
   phase cannot be relative. */
PTP_TRIG_RESULT PTP_TRIG_SchedulePeriodic(uint16_t action_id, uint32_t cmd_seq,
                                          uint64_t period_ns, uint64_t phase_ns);

void PTP_TRIG_Cancel(void);
void PTP_TRIG_ModeSet(PTP_TRIG_MODE mode);
void PTP_TRIG_StatusGet(PTP_TRIG_STATUS *out);
const char *PTP_TRIG_ResultName(PTP_TRIG_RESULT r);

#ifdef __cplusplus
}
#endif

#endif /* PTP_TRIGGER_H */
