/*******************************************************************************
  Corrected MCU timebase - grandmaster time from a local free-running counter

  File Name:
    ptp_timebase.h

  Summary:
    Maps the local SYS_TIME tick counter onto grandmaster nanoseconds, so the
    application can timestamp its own events and schedule future actions in a
    time base that every node on the T1S segment shares.

  Description:
    Phase B of PTP_TIMEBASE_PLAN.md.  The model is one affine map

        grandmaster_ns = anchor_ns + (local_ticks - anchor_L) * slope

    fitted from (L, t1) pairs, where L is a local tick count taken when a Sync
    frame reached the application and t1 is that Sync's egress time out of the
    grandmaster, carried in the following Follow_Up.

    Deliberately NOT fitted against t2 (our own hardware receive timestamp):
    t2 comes from the wall clock the servo steers, so every servo step would
    corrupt the fit.  t1 is never touched locally, which decouples this module
    from the servo completely - see plan section 0.1.

    The local counter is never adjusted, only converted.  Timers, timeouts and
    SYS_TIME itself hang off it; steering it would break them (plan 0.2).

    Source-agnostic on purpose: PTP_TB_SubmitPair() takes a pair and does not
    care where it came from.  Today that is the frame path; when a 1PPS wire
    exists, a capture-based provider submits better pairs through the same
    entry point and nothing here changes (plan 0.3).

  Measured on this hardware, 2026-08-12 (test_results.md, phase A):
    - local tick source vs master wall clock:  -62.7 ppm
    - handover delay after detrending:         median 14.9 us, p99 33.3 us
    - spread of min-filter winners:            9.1 us over a 172 s baseline
    - resulting rate uncertainty:              +/-0.053 ppm
    These numbers only hold with the clock patch in plib_clock.c (XOSC0 as the
    DPLL reference).  Without it the tick source wanders by tens of ppm per
    minute and nothing in this module can help.
*******************************************************************************/

#ifndef PTP_TIMEBASE_H
#define PTP_TIMEBASE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Servo-independent state of the fit. */
typedef enum
{
    PTP_TB_UNINIT = 0,   /* no pair yet - Convert() refuses                  */
    PTP_TB_ANCHORED,     /* offset known, slope still nominal                */
    PTP_TB_LOCKED,       /* slope fitted from two winners far apart          */
    PTP_TB_HOLDOVER      /* fit is stale: no fresh pair for too long         */
} PTP_TB_STATE;

typedef struct
{
    PTP_TB_STATE state;
    uint32_t     pairs;          /* pairs submitted since init               */
    uint32_t     winners;        /* min-filter winners kept                  */
    uint32_t     rejected;       /* pairs dropped as implausible             */
    uint32_t     reanchors;      /* anchor advanced without new data         */
    int32_t      slope_ppb;      /* fitted slope vs nominal, in ppb          */
    uint64_t     slope_q24;      /* ns per tick, Q24                         */
    uint64_t     baseline_ns;    /* span the slope was fitted over           */
    uint64_t     age_ms;         /* since the last accepted pair             */
    int64_t      last_resid_ns;  /* residual of the most recent pair         */
    uint64_t     win_spread_ns;  /* max-min residual across kept winners     */
} PTP_TB_STATUS;

void PTP_TB_Initialize(void);

/* Registers the 'tbase' command group.  Separate from Initialize() so the
   module can be used headless in another project. */
void PTP_TB_CliRegister(void);

/* Housekeeping: holdover detection and anchor refresh.  Must be called from
   the main loop; it does no I/O and never blocks. */
void PTP_TB_Tasks(void);

/* One (local ticks, reference ns) pair.  Called from task context, once per
   completed Sync/Follow_Up pair.  Cheap: a residual, a compare, occasionally a
   64/64 division. */
void PTP_TB_SubmitPair(uint64_t local_ticks, uint64_t ref_ns);

/* Convert any local tick value - past or future - into grandmaster ns.
   false while the model is UNINIT. */
bool PTP_TB_Convert(uint64_t local_ticks, uint64_t *ns);

/* Grandmaster time now.  Monotonic: never returns less than a previous call,
   so it is safe to order events by it across a refit. */
bool PTP_TB_Now(uint64_t *ns);

/* Inverse map, for scheduling: which local tick corresponds to this
   grandmaster time.  Needed by the trigger in phase C. */
bool PTP_TB_LocalFor(uint64_t ns, uint64_t *local_ticks);

void PTP_TB_StatusGet(PTP_TB_STATUS *out);

/* True when the model is good enough to act on - LOCKED and not stale.  The
   trigger refuses commands unless this holds (plan C.6). */
bool PTP_TB_IsUsable(void);

#ifdef __cplusplus
}
#endif

#endif /* PTP_TIMEBASE_H */
