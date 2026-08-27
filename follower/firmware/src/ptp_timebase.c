/*******************************************************************************
  Corrected MCU timebase - implementation

  See ptp_timebase.h for what this is and why it fits against t1 rather than
  t2.  Phase B of PTP_TIMEBASE_PLAN.md.
*******************************************************************************/

#include <stdlib.h>
#include <string.h>
#include "ptp_timebase.h"
#include "ptp_trigger.h"
#include "pps_capture.h"
#include "peer_capture.h"
#include "trig_cmd.h"
#include "button.h"
#include "grid_div.h"
#include "definitions.h"

/* --------------------------------------------------------------------------- */
/* tuning                                                                      */
/* --------------------------------------------------------------------------- */

/* Min-filter block size.  One winner per block; at a 100 ms Sync interval a
   block of 32 spans 3.2 s.  Bigger blocks find a lower floor but hand the fit
   fewer points; measured winner spread at 32 was 9.1 us (test_results.md). */
#define TB_BLOCK        32u

/* Winners kept.  16 x 3.2 s = ~50 s baseline.  Upper bound is set by thermal
   drift of the oscillators, lower bound by the residual spread: the rate
   uncertainty is spread/baseline, so a longer baseline is better right up to
   the point where the rate itself has moved (plan B.3). */
/* Stays at 16.  Raising it to 48 was TRIED on 2026-08-12 and made things slightly
   WORSE, which is worth recording because the reasoning that motivated it is
   wrong: if the rate uncertainty were simply winner_spread / baseline, tripling
   the baseline to 77 s should have divided the measured 0.091 ppm of relative
   drift by three.  Measured instead: 0.183 ppm, p-p 12.1 us over a minute against
   9.3 us at 16 winners (test_results.md E5.3).  So the floor is not estimator
   noise but the LAG of the fit behind a real rate change of the oscillator - and
   a longer baseline increases lag.  This is exactly the upper bound plan B.3
   named; 16 is on the useful side of it, 48 is past it. */
#define TB_WINNERS      16u

/* Nominal ns per tick in Q24.  SYS_TIME runs on TC0 at 60 MHz -> 16.666.. ns.
   Computed at init from SYS_TIME_FrequencyGet() so a clock change cannot leave
   a stale constant behind. */
#define TB_Q            24u
#define TB_ONE          (1ULL << TB_Q)

/* A pair whose residual is absurd is a lost frame paired across a gap, or a
   corrupted t1 - not a slow handover.  Dropped rather than fitted.  Generous:
   the measured p99 is 33 us. */
#define TB_RESID_MAX_NS 50000000LL      /* 50 ms */

/* Refit gains, applied only once the winner ring is full so locking stays fast.
   A refit lands every TB_BLOCK * interval (~3.2 s at 100 ms), and replacing the
   model outright each time stepped it by up to the winner spread.  Dividing the
   correction by four gives a time constant of about four refits - roughly 13 s,
   comfortably slower than the filter that feeds it and far quicker than the
   thermal drift it has to track. */
#define TB_SLOPE_DIV    4

/* Offset gain.  Was 4, i.e. a quarter of the offset error applied as a STEP at
   every refit - and with a refit every 3.2 s that step is what a logic analyser
   sees as a jump: measured +1.0 to +1.4 us (test_results.md E5).  At 32 the same
   correction is spread over roughly eight refits, so the step falls to ~150 ns
   while the convergence time goes from ~13 s to ~100 s.  That trade is right for
   this application: the phase has to be quiet, and there is nothing that needs
   the offset to settle in ten seconds. */
#define TB_OFFSET_DIV   32

/* BOTH GAINS ARE RUNTIME-ADJUSTABLE - `tbase gain <s> <o>`.
 *
 * Reason: attributing the measured edge noise to these loops so far rests only
 * on matching time constants (E47: spectral corner at 32 s against 32 refits
 * of offset gain at 1 Hz, autocorrelation 13.6 s against a 16-winner fit
 * baseline).  The proof is to CHANGE a gain and see whether the corner moves
 * with it - and that is only cheap if it costs no flash per value.
 *
 * WHY THE SLOPE MUST NOT SIMPLY BE HELD FIXED: it is master-ns per local TC0
 * tick, and the MCU crystal sits ~62 ppm off nominal (`slope: 61941 ppb`).  A
 * fixed nominal value would be a 62 us error per second.  It MUST be
 * estimated - just not with a 4 s time constant, where the crystal drifts
 * thermally over minutes. */
static uint32_t s_slope_div  = TB_SLOPE_DIV;
static uint32_t s_offset_div = TB_OFFSET_DIV;

/* No fresh pair for this long -> HOLDOVER.  At a 100 ms interval that is 30
   missed cycles; at 500 ms it is 6.  Deliberately in ms, not in cycles, so it
   does not silently change meaning when the master's interval changes. */
#define TB_STALE_MS     3000u

/* How old the last usable MASTER reference is allowed to be before the time
 * is merely "uncertain".  At `ptp interval 100`, 3 s is thirty missed
 * Syncs - wide enough that a single dropout does not stop anything (S1.5
 * checks exactly that), narrow enough that a real outage shows up within
 * seconds instead of never.
 *
 * Deliberately the same number as TB_STALE_MS, but its own define: the two
 * measure different things (age of the pairs fed in vs. age of the master
 * reference) and are allowed to move independently. */
#define TB_REF_STALE_MS 3000u

/* How far a PTP reference is allowed to deviate from the running model and
 * still count as "the same timeline".  Same value as TB_RESID_MAX_NS, and
 * for the same reason: beyond it, it is no longer jitter but a different
 * time.  This is the half of the criterion that catches a grandmaster
 * RESTART - there the pairs keep arriving on time, just with a t1 that jumps
 * back by the master's previous uptime (measured -86,747 s). */
#define TB_REF_MAX_NS   50000000LL

/* Tight outlier filter - the long rationale sits at the check site.
   30 us: twice the worst undisturbed residual (15.6 us) and a fifth of the
   smallest observed outlier (147 us). */
#define TB_OUTLIER_NS   30000LL
/* After this many consecutive rejections it is assumed: this is not an
   outlier, the reference has genuinely moved elsewhere.  8 pairs at 1 Hz is
   eight seconds - short enough that a real jump is followed quickly, long
   enough that a single outlier never gets through. */
#define TB_OUTLIER_RUN  8u

/* Re-anchor before (L - anchor_L) * slope_q24 can overflow 64 bits.  With
   slope_q24 ~2.8e8 the product passes 1.8e19 at ~6.6e10 ticks, about 18
   minutes at 60 MHz.  Re-anchoring every 60 s of ticks keeps three orders of
   magnitude of headroom and costs nothing. */
#define TB_REANCHOR_S   60u

/* --------------------------------------------------------------------------- */
/* state                                                                       */
/* --------------------------------------------------------------------------- */

typedef struct
{
    uint64_t L;      /* local ticks  */
    uint64_t ns;     /* reference ns */
} tb_pair_t;

static uint64_t s_anchor_L;
static uint64_t s_anchor_ns;
static uint64_t s_slope_q24;          /* ns per tick, Q24 */
static uint64_t s_slope_nominal_q24;

static PTP_TB_STATE s_state = PTP_TB_UNINIT;

/* current block */
static uint32_t  s_block_n;
static tb_pair_t s_block_best;
static int64_t   s_block_best_resid;

/* winner ring, oldest at s_win_tail */
static tb_pair_t s_win[TB_WINNERS];
static int64_t   s_win_resid[TB_WINNERS];
static uint32_t  s_win_head;
static uint32_t  s_win_cnt;

static uint64_t s_ticks_per_ms;
static uint64_t s_last_pair_tick;

/* The master reference, independent of the source being fed (see
   PTP_TB_RefAgeMs in the header).  `s_ref_ok_tick` is the last PTP timestamp
   that matched the running model; `s_ref_seen`/`s_ref_bad` count everything
   that arrived. */
static uint64_t s_ref_ok_tick;
static bool     s_ref_ok_valid;
static uint32_t s_ref_seen;
static uint32_t s_ref_bad;
static int64_t  s_ref_last_dev_ns;
static uint64_t s_mono_last_ns;

static uint32_t s_cnt_pairs;
static uint32_t s_cnt_rejected;
static uint32_t s_cnt_reanchor;

/* --- EMERGENCY EXIT AT THE 50 ms BOUND (2026-08-19) -----------------------------
 *
 * The bound below (TB_RESID_MAX_NS) discards a pair whose residual against
 * the model is too large - correctly so, because such a pair is almost
 * always formed across a lost frame and would silently drag the anchor by
 * one interval.
 *
 * BUT IT HAD NO WAY BACK, and that becomes a trap as soon as the MODEL is
 * the wrong one: then EVERY future - correct - pair lies past the bound,
 * gets discarded, produces no winner, no winner means no refit, and without
 * a refit the wrong anchor stays.  A deadlock that only a restart could fix.
 *
 * MEASURED 2026-08-19: after a follower reset, the 1PPS starts feeding from
 * its first edge before the servo has set the PHY wall clock.  The first
 * pair anchored the model 132 s away from master time; after that `rejected
 * 107 of 109`, state stuck at ANCHORED, `usable no`, while the servo sat at
 * FINE and was doing everything right.  The timebase could no longer reach
 * it.
 *
 * The way out is the same idea as TB_OUTLIER_RUN one level down: a real
 * jump has to be followed.  ONLY CONSECUTIVE rejections count - a single
 * accepted pair resets the counter.  That leaves the case the bound was
 * built for (an occasional pair across a lost frame) rejected exactly as
 * before: good pairs always lie in between.  Only once ALL pairs in a row
 * disagree is it the model that is wrong, not the measurement.
 *
 * At 1 Hz pairs that is eight seconds to self-heal; the slope survives it
 * unchanged, because it is measured from intervals and unaffected by an
 * offset error. */
#define TB_RESID_RUN    8u
static uint32_t s_resid_run;         /* consecutive rejections           */
static uint32_t s_cnt_poisoned;      /* how often it re-anchored         */
static int64_t  s_poison_last_ns;    /* the residual that triggered it   */
/* The tight filter: how often it struck, how large the outliers were, and
   how often it gave up.  Without the third number there would be no way to
   tell whether it is blocking a real jump - and that is the case where it
   would itself be the defect. */
static uint32_t s_cnt_outlier;
static uint32_t s_cnt_outlier_forced;
static uint32_t s_outlier_run;
static int64_t  s_outlier_max_ns;
static int64_t  s_outlier_min_ns;
/* THE OPENING RESIDUAL OF A RUN - and why `min`/`max` above cannot answer
 * that question.
 *
 * `resid` is model minus reference.  Once the fit has been bent by ONE bad
 * pair, the residuals of ALL following pairs sit above the threshold, even
 * the clean ones - `s_outlier_run` is only reset by a good pair.  Measured
 * 2026-08-17: 40 rejected against 271 let through, i.e. runs far beyond
 * TB_OUTLIER_RUN = 8.  `s_outlier_min_ns`/`_max_ns` are therefore extrema of
 * the AFTERMATH (down to -1.09 ms), not of the trigger.
 *
 * From that I once derived an error magnitude of 1 ms and went looking for
 * it - the ISR latency (max 1.68 us over 301 samples, E38) and the
 * SYS_TIME race (0 misreads over 20.9 million accesses, E36) were checked
 * against this wrong target and ruled out, while a candidate at 16.7 us was
 * dismissed as "too small".
 *
 * So this records what the pair carried that OPENED a run (`s_outlier_run`
 * going 0 -> 1).  That is the triggering quantity. */
static uint32_t s_outlier_runs;        /* number of runs, not of rejections */
static int64_t  s_outlier_open_first;  /* opening residual of the FIRST run  */
static int64_t  s_outlier_open_last;   /* ... of the last                    */
static int64_t  s_outlier_open_min;
static int64_t  s_outlier_open_max;
static bool     s_outlier_open_seen;
static int64_t  s_last_resid;

/* --- State messages: sync lost / sync back / synchronised again -----------
 *
 * For the drift demo, the console should narrate what the oscilloscope
 * shows.  Anchored on `master ref age` and NOT on holdover: holdover
 * depends on the last pair that went into the timebase, and with the 1PPS
 * source, pps_capture.c keeps feeding its own pairs - so the follower would
 * never go into holdover even though sync has stopped (measured
 * 2026-08-23).
 *
 * Exactly ONE message per transition.  Printed in PTP_TB_Tasks(), i.e. in
 * the main loop; the pair evaluation only sets a flag, because it partly
 * runs from a driver callback. */
#define TB_SYNCMSG_RUN      8u        /* consecutive samples for "synchronised again" */
/* TWO thresholds, because "synchronised" means something different per
   source.  Measured 2026-08-23: with 1PPS the residual error sits at 1-2 us,
   with PTP pairs it varies between 3 and 95 us.  A fixed 5 us bound is
   therefore right for 1PPS and unreachable for PTP pairs - the third
   message would have stayed silent in the drift demo, exactly where it is
   needed. */
#define TB_SYNCMSG_NS_PPS   5000      /* source: 1PPS                           */
#define TB_SYNCMSG_NS_PTP   50000     /* source: PTP pairs                      */

static bool     s_syncmsg = true;     /* on: a demo needs it                    */
static int32_t  s_syncmsg_ns = TB_SYNCMSG_NS_PPS;
static bool     s_syncmsg_user;       /* set by hand -> do not overwrite        */
static bool     s_sync_lost;          /* "lost" has been reported               */
static uint64_t s_sync_lost_tick;
static bool     s_sync_watch;         /* waiting for re-lock                    */
static uint32_t s_sync_good;          /* consecutive samples within the bound   */
static uint32_t s_sync_gap_ms;        /* duration of the last gap               */
static uint8_t  s_sync_pending;       /* 0 none, 1 lost, 2 back, 3 synchronised */
static int64_t  s_last_offset_err;   /* model minus newest winner, at refit */
static uint64_t s_baseline_ns;

/* --------------------------------------------------------------------------- */
/* trace: the model's state, recorded rather than queried                     */
/*                                                                             */
/* WHY A RING BUFFER AND NOT A CONSOLE QUERY.  What is being looked for is a
 * TRANSIENT phase excursion of half a grid period (~48 us) that runs across
 * a few consecutive blocks and then disappears completely - measured
 * 2026-08-16 in S3.1 (pulse mode, +-45 us) and in W2 pass 2 (switchover
 * operation, p99 48.8 us, k(trans) = 0).  It leaves NO trace in any existing
 * counter: dropouts 0, holdover 0, off-timeline 0, reanchors 0, and the
 * residual afterwards 32 ns.
 *
 * Querying during the measurement is not a way around this, it IS the
 * disturbance: 5 to 8 console lines cost ~48 ms of main loop (E26), i.e.
 * more than the quantity being chased.  So record it and print it
 * AFTERWARDS.
 *
 * WHAT THE BUFFER IS MEANT TO ANSWER - and it can also CLEAR the model: if
 * `q24`, anchor and residual stay unchanged across the excursion, the model
 * has not moved, and the cause sits downstream of it (in LocalFor() or in
 * arming).  A trace that shows nothing is a result here. */
#define TB_TRACE_N      256u        /* 256 x 200 ms = 51 s of history         */
#define TB_TRACE_MS     200u        /* default; changeable at runtime         */

typedef struct
{
    uint32_t t_ms;          /* local time, ms since recording started        */
    uint32_t q24_lo;        /* rate correction, lower 32 bits - the jump candidate */
    uint32_t anchor_L_lo;   /* anchor tick, lower 32 bits                     */
    uint32_t anchor_ns_lo;  /* anchor time, lower 32 bits                     */
    int32_t  resid_ns;      /* residual of the last pair                      */
    uint32_t spread_ns;     /* winner spread                                  */
    uint32_t pairs;         /* pair counter - says whether one was added      */
    /* THE REAL re-anchor counter.  The first build instead compared
       `anchor_L` of two points - and thereby reported a re-anchor at EVERY
       point, because the anchor advances by 6e7 ticks every second as a
       matter of course.  "The value changed" is not "the event happened";
       that is what this counter is for. */
    uint32_t reanchors;
    uint8_t  state;
    uint8_t  quality;
} tb_trace_t;

static tb_trace_t s_trace[TB_TRACE_N];
static uint32_t   s_trace_head;     /* next write slot                        */
static uint32_t   s_trace_n;        /* how many are valid                     */
static bool       s_trace_on = true;
static uint64_t   s_trace_due;
static uint64_t   s_trace_t0;
/* The interval is adjustable because 256 slots at 200 ms is only 51 s of
   history - a measurement run takes minutes.  The trade-off belongs to
   whoever is measuring, not to the code: fine resolution for a short window
   or coarse for the whole run. */
static uint32_t   s_trace_ms = TB_TRACE_MS;

/* THE INSTRUMENT'S OWN COUNTERS.  The first run delivered 29 points for
 * 300 s of measurement time, and without these two numbers there was no way
 * to tell whether (a) sampling stopped, (b) printing broke off, or (c) the
 * buffer was cleared.  Three causes, one symptom - the same class of bug as
 * the three closed doors in the confirmation path, and the same fix.
 *
 * `calls` counts every entry into sampling, `writes` every point actually
 * written.  calls without writes means "the interval has not elapsed yet";
 * no calls at all means "PTP_TB_Tasks() is no longer running". */
static uint32_t   s_trace_calls;
static uint32_t   s_trace_writes;

/* Holdover bookkeeping.  Cumulative and never reset except by PTP_TB_Initialize,
   so a query AFTER a measurement can still say whether the window was clean -
   querying DURING one perturbs it (GPIO_SYNC_TESTS.md 2.5). */
static uint32_t s_ho_entries;
static uint64_t s_ho_ticks;
static uint64_t s_ho_since;
static uint64_t s_ho_longest;

/* --------------------------------------------------------------------------- */
/* helpers                                                                     */
/* --------------------------------------------------------------------------- */

/* ns = (dL * slope_q24) >> 24.  dL is bounded by the re-anchor policy, so this
   cannot overflow; see TB_REANCHOR_S. */
static inline uint64_t tb_span_ns(uint64_t dL, uint64_t slope_q24)
{
    return (dL * slope_q24) >> TB_Q;
}

static inline uint64_t tb_span_ticks(uint64_t dns, uint64_t slope_q24)
{
    /* Inverse of tb_span_ns.  Shifting the numerator by TB_Q would overflow for
       large spans, so divide first and correct with the remainder - keeps full
       precision without a 128-bit intermediate (which xc32 on 32-bit ARM does
       not have). */
    uint64_t q = dns / slope_q24;
    uint64_t r = dns - q * slope_q24;
    return (q << TB_Q) + ((r << TB_Q) / slope_q24);
}

/* THE ANCHOR PAIR IS INDIVISIBLE - and it used to be written in two
 * assignments.
 *
 * `s_anchor_L` (local tick) and `s_anchor_ns` (master time to go with it)
 * are ONE value in two variables: only together do they name a point in
 * time.  They are written from the main loop (refit, re-anchor, first
 * pair), but read from the TRIGGER ISR.  If the ISR hits the gap between
 * the two assignments, it computes with the NEW tick anchor and the OLD
 * time - and ends up off by exactly one refit interval.
 *
 * MEASURED 2026-08-19 from the ring buffer in ptp_trigger.c: at the
 * dropout, the local tick advanced by 60,002,226 (= 1.000 s), but the
 * converted master time advanced by 2,000,099,956 ns (= 2.000 s).  The
 * excess is exactly one second, i.e. the refit period of the 1PPS source;
 * under PTP feeding it was 3.2 s accordingly.  The catch-up loop in the
 * trigger then sees a deadline that lies one second in the past, skips
 * 10,000 grid points - and that is the block-wide silence.
 *
 * A PRIMASK section around the two assignments is enough and is the
 * cheapest protection conceivable: the ISR then runs either entirely
 * before or entirely after, never in between.  The cost is two memory
 * accesses' worth of interrupt lockout, i.e. a few nanoseconds - against
 * 10,000 lost grid points.  A seqlock would be the alternative, but costs
 * code and a retry loop in the reader without gaining anything here: there
 * is no second writer, and the lock is tiny. */
static inline void tb_anchor_set(uint64_t L, uint64_t ns)
{
    uint32_t pm = __get_PRIMASK();

    __disable_irq();
    s_anchor_L  = L;
    s_anchor_ns = ns;
    __set_PRIMASK(pm);
}

static uint64_t tb_model_ns(uint64_t L)
{
    /* Both anchors ONCE into locals - otherwise the expression below reads
       them more than once.  Today that does no harm (the only writer is the
       main loop, and it cannot interrupt itself), but a future writer in an
       ISR would reopen the same tear, invisibly. */
    const uint64_t a_L  = s_anchor_L;
    const uint64_t a_ns = s_anchor_ns;

    if (L >= a_L)
    {
        return a_ns + tb_span_ns(L - a_L, s_slope_q24);
    }
    return a_ns - tb_span_ns(a_L - L, s_slope_q24);
}

/* Move the anchor forward along the current model without new data.  Keeps the
   model identical, only re-bases it - so it is safe in HOLDOVER too. */
static void tb_reanchor(uint64_t L)
{
    tb_anchor_set(L, tb_model_ns(L));
    s_cnt_reanchor++;
}

/* Two-point fit across the winner ring: oldest and newest.  The result is
   dominated by the quality of the two endpoints, and the min-filter has already
   made those the best samples available - an ordinary least-squares fit would
   pull worse samples back in (plan B.3). */
static void tb_refit(void)
{
    uint32_t oldest;
    const tb_pair_t *a;
    const tb_pair_t *b;
    uint64_t dL;
    uint64_t dns;

    if (s_win_cnt < 2u)
    {
        return;
    }

    oldest = (s_win_head + TB_WINNERS - s_win_cnt) % TB_WINNERS;
    a = &s_win[oldest];
    b = &s_win[(s_win_head + TB_WINNERS - 1u) % TB_WINNERS];

    if (b->L <= a->L || b->ns <= a->ns)
    {
        return;
    }

    /* Fit between the CENTROIDS of the two ring halves, not between the two end
     * points.
     *
     * The two-point version was chosen because the min-filter already makes the
     * endpoints the best samples available, and that reasoning still holds - but
     * it has two costs the logic analyser made visible over a 60 s window
     * (test_results.md E5).  Each endpoint carries the full winner spread, so the
     * rate error is spread/baseline and stays CONSTANT until that endpoint is
     * replaced - which showed up as smooth drift segments of 10-20 s at up to
     * 500 ns/s.  And when the ring rotates, one endpoint is replaced wholesale,
     * which steps the slope - the +1.0 to +1.4 us jumps.
     *
     * Averaging eight winners per half divides the endpoint noise by sqrt(8) and,
     * more importantly, makes the fit CONTINUOUS: a rotation changes each mean by
     * one eighth instead of replacing it.  Arithmetic stays in the same range as
     * before because the sums are taken relative to the oldest winner, so the
     * existing overflow reasoning carries over unchanged.  A full least-squares
     * fit would be better still, but its cross products reach 1e19 and do not fit
     * in 64 bits without a scaling scheme that costs more accuracy than it buys.
     */
    if (s_win_cnt >= 4u)
    {
        uint32_t half = s_win_cnt / 2u;
        uint32_t i;
        uint64_t sLa = 0u, sna = 0u, sLb = 0u, snb = 0u;
        uint64_t Lo, no, Ln, nn;

        for (i = 0u; i < s_win_cnt; i++)
        {
            const tb_pair_t *p = &s_win[(oldest + i) % TB_WINNERS];
            uint64_t dLi = p->L  - a->L;
            uint64_t dni = p->ns - a->ns;
            if (i < half) { sLa += dLi; sna += dni; }
            else          { sLb += dLi; snb += dni; }
        }
        Lo = a->L  + sLa / half;
        no = a->ns + sna / half;
        Ln = a->L  + sLb / (s_win_cnt - half);
        nn = a->ns + snb / (s_win_cnt - half);

        if (Ln <= Lo || nn <= no)
        {
            return;
        }
        dL  = Ln - Lo;
        dns = nn - no;
    }
    else
    {
        dL  = b->L - a->L;
        dns = b->ns - a->ns;
    }

    /* slope = dns/dL in Q24.  dns is ns over the baseline (~5e10 for 50 s), so
       dns << 24 would overflow - divide first, then fold in the remainder. */
    {
        uint64_t q = dns / dL;
        uint64_t r = dns - q * dL;
        uint64_t measured = (q << TB_Q) + ((r << TB_Q) / dL);

        if (s_win_cnt < TB_WINNERS)
        {
            /* Still filling the ring: take the measurement outright so locking
               stays fast (about ten seconds). */
            s_slope_q24 = measured;
        }
        else
        {
            /* Steady state: move a fraction of the way.  A refit every ~3.2 s
               that replaced the slope outright stepped the model each time, and
               those steps were the dominant term in the 21.5 us of cross-board
               skew the logic analyser measured - the 5 s capture (one or two
               refits) showed 4.7 us, the 30 s capture (about ten) showed 21.5. */
            int64_t d = (int64_t)measured - (int64_t)s_slope_q24;
            s_slope_q24 = (uint64_t)((int64_t)s_slope_q24 + d / (int64_t)s_slope_div);
        }
    }

    s_baseline_ns = dns;

    /* Re-anchor onto the newest winner's tick, but do NOT swallow its whole
       residual: that winner carries up to the winner spread (~9 us measured), and
       adopting it wholesale is exactly what made the model jump.  Correct only a
       fraction of the offset error and let the rest converge over a few refits.
       The model stays continuous in value; the offset still converges, just
       without a step. */
    {
        uint64_t model_at_b = tb_model_ns(b->L);
        int64_t  err = (int64_t)model_at_b - (int64_t)b->ns;   /* + = model ahead */

        /* Determine the target value FIRST, THEN set the pair in one go - the
           branch must not sit between the two assignments. */
        uint64_t new_ns = (s_win_cnt < TB_WINNERS)
                        ? b->ns              /* full correction while locking */
                        : (uint64_t)((int64_t)model_at_b - err / (int64_t)s_offset_div);

        tb_anchor_set(b->L, new_ns);
        s_last_offset_err = err;
    }
    s_state = PTP_TB_LOCKED;
}

static void tb_winner_push(const tb_pair_t *p, int64_t resid)
{
    s_win[s_win_head] = *p;
    s_win_resid[s_win_head] = resid;
    s_win_head = (s_win_head + 1u) % TB_WINNERS;
    if (s_win_cnt < TB_WINNERS)
    {
        s_win_cnt++;
    }
}

/* --------------------------------------------------------------------------- */
/* public                                                                      */
/* --------------------------------------------------------------------------- */

void PTP_TB_Initialize(void)
{
    uint32_t hz = SYS_TIME_FrequencyGet();

    memset(s_win, 0, sizeof(s_win));
    memset(s_win_resid, 0, sizeof(s_win_resid));
    s_win_head = 0u;
    s_win_cnt = 0u;
    s_block_n = 0u;
    s_block_best_resid = 0;
    tb_anchor_set(0u, 0u);
    s_state = PTP_TB_UNINIT;
    s_last_pair_tick = 0u;
    /* The reference is reset ALONG with it: `tbase reset` and a source
       switch discard the model, and a reference without a model is not
       one. */
    s_ref_ok_tick = 0u;
    s_ref_ok_valid = false;
    s_ref_seen = 0u;
    s_ref_bad = 0u;
    s_ref_last_dev_ns = 0;
    s_mono_last_ns = 0u;
    s_cnt_pairs = 0u;
    s_cnt_rejected = 0u;
    s_cnt_reanchor = 0u;
    s_resid_run = 0u;
    s_cnt_poisoned = 0u;
    s_poison_last_ns = 0;
    s_cnt_outlier = 0u;
    s_cnt_outlier_forced = 0u;
    s_outlier_run = 0u;
    s_outlier_max_ns = 0;
    s_outlier_min_ns = 0;
    s_outlier_runs = 0u;
    s_outlier_open_first = 0;
    s_outlier_open_last = 0;
    s_outlier_open_min = 0;
    s_outlier_open_max = 0;
    s_outlier_open_seen = false;
    s_ho_entries = 0u;
    s_sync_lost = false;
    s_sync_watch = false;
    s_sync_good = 0u;
    s_sync_pending = 0u;
    s_ho_ticks = 0u;
    s_ho_longest = 0u;
    s_last_resid = 0;
    s_last_offset_err = 0;
    s_baseline_ns = 0u;

    if (hz == 0u)
    {
        hz = 60000000u;                     /* should not happen; stay sane */
    }
    s_ticks_per_ms = (uint64_t)hz / 1000u;
    if (s_ticks_per_ms == 0u)
    {
        s_ticks_per_ms = 1u;
    }

    /* Nominal ns per tick in Q24, rounded: (1e9 << 24) / hz. */
    s_slope_nominal_q24 = ((uint64_t)1000000000u << TB_Q) / (uint64_t)hz;
    s_slope_q24 = s_slope_nominal_q24;
}

/* Which source may feed the model.  Only one at a time: PTP pairs carry a
 * SOFTWARE capture of Sync arrival (spread 14-28 us), 1PPS pairs a hardware edge
 * (the wall clocks themselves are 40 ns apart, test_results.md E6).  Mixing them
 * would let the min-filter choose between two populations that differ by three
 * orders of magnitude, and the model would jump whenever the winner came from the
 * other one. */
static PTP_TB_SRC s_src = PTP_TB_SRC_PTP;

static void tb_submit_pair(uint64_t local_ticks, uint64_t ref_ns);

/* Block size is a property of the SOURCE, not a constant.
 *
 * The min-filter exists to find the least-delayed sample out of many, because
 * software capture latency is one-sided noise.  A 1PPS edge has no such latency,
 * so filtering buys nothing - and it would cost dearly: 1PPS delivers ONE pair
 * per second against PTP's ten, so a block of 32 would take 32 s and the
 * 16-winner ring 8 minutes to fill.  With a block of 1 every pair is a winner and
 * the ring spans 16 s. */
static uint32_t tb_block_size(void)
{
    return (s_src == PTP_TB_SRC_PPS) ? 1u : TB_BLOCK;
}

void PTP_TB_SourceSet(PTP_TB_SRC src)
{
    if (src != s_src)
    {
        s_src = src;
        /* The reference changes meaning - PTP pairs are against the master's t1,
           1PPS pairs against this board's own wall clock - so the old model must
           not be carried over, however close the two are.  Initialize() is what
           "tbase reset" uses, and it deliberately does NOT touch s_src. */
        PTP_TB_Initialize();
    }
}

PTP_TB_SRC PTP_TB_SourceGet(void)
{
    return s_src;
}

/* The gate must sit in front of BOTH public entries, not just the tagged one.
 * The first version had PTP_TB_SubmitPairFrom() call PTP_TB_SubmitPair(), which
 * left the untagged entry - the one ptp_follower.c uses - completely ungated: the
 * model then took 643 PTP pairs alongside 65 from 1PPS, mixing a reference of
 * master t1 with a reference of this board's own wall clock.  The symptom was a
 * winner spread of 295 us, twenty times WORSE than either source alone, and a
 * baseline of 0.7 s where 16 s was expected.  Two references averaged together are
 * not a better reference. */
void PTP_TB_SubmitPairFrom(PTP_TB_SRC src, uint64_t local_ticks, uint64_t ref_ns)
{
    /* THE MASTER REFERENCE IS EVALUATED BEFORE THE GATE, not behind it.
     *
     * That is the whole point of stage 1: the PTP pairs keep arriving even
     * while 1PPS is feeding - the gate below discards them, not the
     * network.  Their arrival is therefore the proof that a master is
     * present, and their distance from the running model is the proof that
     * it is the SAME master on the SAME timeline.
     *
     * Together the two cover three kinds of disturbance with one
     * criterion: on `ptp stop` and with a dead sender no more pairs arrive
     * at all (arrival goes stale); on a grandmaster restart they arrive on
     * time but do not match (the deviation trips).  Before this, both were
     * invisible, because the state only knew the FED source. */
    if (src == PTP_TB_SRC_PTP)
    {
        s_ref_seen++;
        if (s_state == PTP_TB_UNINIT)
        {
            /* Without a model there is nothing to compare against - the
               first reference simply counts. */
            s_ref_ok_tick = local_ticks;
            s_ref_ok_valid = true;
        }
        else
        {
            int64_t dev = (int64_t)tb_model_ns(local_ticks) - (int64_t)ref_ns;
            s_ref_last_dev_ns = dev;
            if (dev > TB_REF_MAX_NS || dev < -TB_REF_MAX_NS)
            {
                s_ref_bad++;
            }
            else
            {
                s_ref_ok_tick = local_ticks;
                s_ref_ok_valid = true;
            }
        }
    }

    if (src == s_src)
    {
        tb_submit_pair(local_ticks, ref_ns);
    }
}

/* Untagged entry, kept so ptp_follower.c needs no change: by definition its pairs
   are the PTP ones. */
void PTP_TB_SubmitPair(uint64_t local_ticks, uint64_t ref_ns)
{
    PTP_TB_SubmitPairFrom(PTP_TB_SRC_PTP, local_ticks, ref_ns);
}

static void tb_submit_pair(uint64_t local_ticks, uint64_t ref_ns)
{
    tb_pair_t p;
    int64_t resid;

    p.L = local_ticks;
    p.ns = ref_ns;
    s_cnt_pairs++;
    s_last_pair_tick = local_ticks;

    if (s_state == PTP_TB_UNINIT)
    {
        /* First pair defines the offset; the slope stays nominal until two
           winners are far enough apart to measure it. */
        tb_anchor_set(p.L, p.ns);
        s_state = PTP_TB_ANCHORED;
        s_block_n = 1u;
        s_block_best = p;
        s_block_best_resid = 0;
        s_last_resid = 0;
        return;
    }

    /* Residual against the CURRENT model, not against the raw difference.  On
       the raw difference the drift within a block would decide which sample
       looks smallest, so the filter would systematically pick the first or last
       sample of every block depending on the drift sign (plan B.2). */
    resid = (int64_t)tb_model_ns(p.L) - (int64_t)p.ns;
    s_last_resid = resid;

    /* Watch for re-locking ONLY once a gap has been reported.  What is
       counted is samples IN A ROW - a single good sample would not be a
       lock, just a coincidence on the way there. */
    if (s_syncmsg && s_sync_watch)
    {
        int64_t mag = (resid < 0) ? -resid : resid;
        if (mag <= (int64_t)s_syncmsg_ns)
        {
            s_sync_good++;
            if (s_sync_good >= TB_SYNCMSG_RUN)
            {
                s_sync_watch = false;
                s_sync_pending = 3u;
            }
        }
        else
        {
            s_sync_good = 0u;
        }
    }

    if (resid > TB_RESID_MAX_NS || resid < -TB_RESID_MAX_NS)
    {
        /* Almost certainly a pair formed across a lost frame.  Feeding it to
           the fit would move the anchor by an interval, silently. */
        s_cnt_rejected++;
        s_resid_run++;
        if (s_resid_run < TB_RESID_RUN)
        {
            return;
        }

        /* EMERGENCY EXIT: TB_RESID_RUN pairs in a row disagree with the
           model - so the model is wrong, not the measurement.  Re-anchor
           and say so OUT LOUD; a rescue path that stays silent hides
           exactly the defect it is meant to catch (CLAUDE.md). */
        s_cnt_poisoned++;
        s_poison_last_ns = resid;
        s_resid_run = 0u;
        SYS_CONSOLE_PRINT("[TBASE] model was %+lld ns off the reference -"
                          " re-anchored after %u rejections.\r\n",
                          (long long)resid, (unsigned)TB_RESID_RUN);

        /* Discard BOTH the ring and the block: the old winners sit on the
           old timeline, mixing them with the new anchor would be nonsense.
           LOCKED has to be earned again afterwards - exactly right, because
           the timebase is only usable again once it has proven itself on
           the new line. */
        memset(s_win, 0, sizeof s_win);
        memset(s_win_resid, 0, sizeof s_win_resid);
        s_win_head = 0u;
        s_win_cnt = 0u;
        tb_anchor_set(p.L, p.ns);
        s_state = PTP_TB_ANCHORED;
        s_block_n = 1u;
        s_block_best = p;
        s_block_best_resid = 0;
        s_last_resid = 0;
        /* The old reference verdict no longer applies - it was made
           against the old model.  The next reference decides afresh. */
        s_ref_ok_valid = false;
        s_mono_last_ns = 0u;
        return;
    }
    s_resid_run = 0u;

    /* THE TIGHT OUTLIER FILTER.  The 50 ms bound above catches garbage; it
     * is 300 times too loose to catch what causes the phase excursion.
     *
     * MEASURED 2026-08-16 (E30): during an excursion, one board's residual
     * sat at +147 us and decayed monotonically to 30 us over ~70 s, while
     * the other board stayed at +2.7 us.  The winner spread went from 2 us
     * to 65 us - the signature of ONE outlier inside the ring of 16
     * winners.  The model itself was innocent: delta q24 = -82 out of 279
     * million (0.3 ppb), no re-anchor.  It correctly tracked a jump in the
     * INPUT DATA, and that tracking is exactly the excursion: the ring
     * holds an outlier for ~50 s (16 winners at 3.2 s each), and the offset
     * follow-up with /32 takes even longer.
     *
     * The threshold: normally |residual| sits at a median of 1 us, at
     * 15.6 us in the worst undisturbed case.  30 us is therefore twice the
     * worst normal value and a fifth of the smallest observed outlier.
     *
     * APPLIES ONLY WHILE LOCKED AND WITH A FULL RING.  While locking, large
     * residuals are normal and must be accepted, otherwise it never locks -
     * a bound that prevents the good case is worse than the outlier.
     *
     * AND IT GIVES UP AFTER N CONSECUTIVE REJECTIONS.  A real jump of the
     * reference (grandmaster restart, source switchover) looks like an
     * outlier at first; rejecting it forever freezes the model on a
     * timeline that no longer exists.  That is the lesson of "a rescue path
     * can become the defect itself": after TB_OUTLIER_RUN rejections it is
     * accepted, and counted. */
    if (s_state == PTP_TB_LOCKED && s_win_cnt >= TB_WINNERS
        && (resid > TB_OUTLIER_NS || resid < -TB_OUTLIER_NS))
    {
        s_outlier_run++;
        if (s_outlier_run == 1u)
        {
            /* Entering a run: THIS residual is the triggering
               quantity.  Everything after it is the aftermath of the bent fit. */
            s_outlier_runs++;
            if (!s_outlier_open_seen)
            {
                s_outlier_open_first = resid;
                s_outlier_open_min = resid;
                s_outlier_open_max = resid;
                s_outlier_open_seen = true;
            }
            else
            {
                if (resid > s_outlier_open_max) { s_outlier_open_max = resid; }
                if (resid < s_outlier_open_min) { s_outlier_open_min = resid; }
            }
            s_outlier_open_last = resid;
        }
        if (s_outlier_run <= TB_OUTLIER_RUN)
        {
            s_cnt_outlier++;
            if (resid > s_outlier_max_ns) { s_outlier_max_ns = resid; }
            if (resid < s_outlier_min_ns) { s_outlier_min_ns = resid; }
            return;
        }
        /* Gave up: the reference has genuinely moved elsewhere. */
        s_cnt_outlier_forced++;
    }
    else
    {
        s_outlier_run = 0u;
    }

    if (s_block_n == 0u || resid < s_block_best_resid)
    {
        s_block_best = p;
        s_block_best_resid = resid;
    }
    s_block_n++;

    if (s_block_n >= tb_block_size())
    {
        tb_winner_push(&s_block_best, s_block_best_resid);
        s_block_n = 0u;
        tb_refit();
    }
}

/* Write one point to the trace.  Deliberately WITHOUT PTP_TB_StatusGet():
   that computes the winner spread via a loop, and this function runs five
   times a second in the main loop.  So the spread is tracked cheaply here
   instead, from the same values. */
static void tb_trace_sample(uint64_t now)
{
    tb_trace_t *e;
    int64_t lo, hi;
    uint32_t i;

    s_trace_calls++;
    if (!s_trace_on || s_ticks_per_ms == 0u) { return; }
    if (s_trace_t0 == 0u) { s_trace_t0 = now; }
    if (now < s_trace_due) { return; }
    s_trace_due = now + (uint64_t)s_trace_ms * s_ticks_per_ms;

    e = &s_trace[s_trace_head];
    e->t_ms        = (uint32_t)((now - s_trace_t0) / s_ticks_per_ms);
    e->q24_lo      = (uint32_t)s_slope_q24;
    e->anchor_L_lo = (uint32_t)s_anchor_L;
    e->anchor_ns_lo = (uint32_t)s_anchor_ns;
    e->resid_ns    = (int32_t)s_last_resid;
    e->pairs       = s_cnt_pairs;
    e->reanchors   = s_cnt_reanchor;
    e->state       = (uint8_t)s_state;
    e->quality     = (uint8_t)PTP_TB_QualityGet();

    e->spread_ns = 0u;
    if (s_win_cnt > 0u)
    {
        uint32_t oldest = (s_win_head + TB_WINNERS - s_win_cnt) % TB_WINNERS;
        lo = s_win_resid[oldest];
        hi = lo;
        for (i = 1u; i < s_win_cnt; i++)
        {
            int64_t v = s_win_resid[(oldest + i) % TB_WINNERS];
            if (v < lo) { lo = v; }
            if (v > hi) { hi = v; }
        }
        e->spread_ns = (uint32_t)(hi - lo);
    }

    s_trace_head = (s_trace_head + 1u) % TB_TRACE_N;
    if (s_trace_n < TB_TRACE_N) { s_trace_n++; }
    s_trace_writes++;
}

void PTP_TB_Tasks(void)
{
    uint64_t now;
    uint64_t age_ticks;

    if (s_state == PTP_TB_UNINIT)
    {
        return;
    }

    now = SYS_TIME_Counter64Get();
    tb_trace_sample(now);

    /* Holdover: the model still answers, but on a slope that is getting old.
       Reported rather than hidden - a follower that looks locked while nothing
       arrives is exactly the trap the existing servo status falls into. */
    age_ticks = now - s_last_pair_tick;
    if (age_ticks > (uint64_t)TB_STALE_MS * s_ticks_per_ms)
    {
        if (s_state != PTP_TB_HOLDOVER)
        {
            s_state = PTP_TB_HOLDOVER;
            /* Counted, because until now a holdover episode left no trace at all.
               An episode that falls inside a measurement window invalidates it -
               the boards then drift apart at the raw difference of their crystals,
               11.2 us per second, which buries an hour of residual drift in ten
               seconds.  That is the leading suspect for the 3-4x run-to-run
               scatter this bench shows (PHASE_DRIFT_THESEN.md thesis 4a), and
               without a counter one cannot even tell a clean window from a dirty
               one after the fact. */
            s_ho_entries++;
            s_ho_since = now;
        }
    }
    else if (s_state == PTP_TB_HOLDOVER)
    {
        /* Pairs are flowing again.  LOCKED only if a slope was ever fitted. */
        uint64_t dur = now - s_ho_since;
        s_ho_ticks += dur;
        if (dur > s_ho_longest) { s_ho_longest = dur; }
        s_state = (s_win_cnt >= 2u) ? PTP_TB_LOCKED : PTP_TB_ANCHORED;
    }
    else
    {
        /* nothing to do */
    }

    /* --- State messages, exactly one per transition -------------------------
     *
     * `s_ref_seen != 0` as the condition for the first message: a board that
     * has never seen a master has not LOST sync - it never had it.  Without
     * this condition every cold start would report "SYNC LOST", and a
     * message that always comes says nothing. */
    if (s_syncmsg)
    {
        uint64_t ra = PTP_TB_RefAgeMs();
        bool stale = (ra == UINT64_MAX) || (ra > (uint64_t)TB_REF_STALE_MS);

        if (stale && !s_sync_lost && (s_ref_seen != 0u))
        {
            s_sync_lost = true;
            s_sync_watch = false;
            s_sync_good = 0u;
            s_sync_lost_tick = now;
            s_sync_pending = 1u;
        }
        else if (!stale && s_sync_lost)
        {
            uint64_t d = (now > s_sync_lost_tick) ? (now - s_sync_lost_tick) : 0u;
            s_sync_gap_ms = (uint32_t)(d / s_ticks_per_ms);
            s_sync_lost = false;
            s_sync_watch = true;
            s_sync_good = 0u;
            s_sync_pending = 2u;
            /* Adjust the threshold to the source, unless it was set by hand.
               Here and not at the source switch, because only now is it
               decided against what "synchronised" is measured. */
            if (!s_syncmsg_user)
            {
                s_syncmsg_ns = (PTP_TB_SourceGet() == PTP_TB_SRC_PPS)
                             ? TB_SYNCMSG_NS_PPS : TB_SYNCMSG_NS_PTP;
            }
        }
    }
    if (s_sync_pending == 1u)
    {
        s_sync_pending = 0u;
        SYS_CONSOLE_PRINT("[TBASE] *** SYNC LOST *** no master reference for"
                          " %u ms.  The timebase now runs free at the held"
                          " rate - the pins are drifting.\r\n",
                          (unsigned)TB_REF_STALE_MS);
    }
    else if (s_sync_pending == 2u)
    {
        s_sync_pending = 0u;
        SYS_CONSOLE_PRINT("[TBASE] *** SYNC BACK *** after %u ms without a"
                          " reference.  The servo is now catching up - NOT"
                          " synchronised yet.  'Synchronised' here means: %ld ns, %u"
                          " samples in a row.\r\n", (unsigned)s_sync_gap_ms,
                          (long)s_syncmsg_ns, (unsigned)TB_SYNCMSG_RUN);
    }
    else if (s_sync_pending == 3u)
    {
        s_sync_pending = 0u;
        SYS_CONSOLE_PRINT("[TBASE] *** SYNCHRONISED AGAIN *** residual under"
                          " %ld ns for %u samples in a row.\r\n",
                          (long)s_syncmsg_ns, (unsigned)TB_SYNCMSG_RUN);
    }

    if ((now - s_anchor_L) > (uint64_t)TB_REANCHOR_S * 1000u * s_ticks_per_ms)
    {
        tb_reanchor(now);
    }
}

bool PTP_TB_Convert(uint64_t local_ticks, uint64_t *ns)
{
    if (s_state == PTP_TB_UNINIT || ns == NULL)
    {
        return false;
    }
    *ns = tb_model_ns(local_ticks);
    return true;
}

bool PTP_TB_Now(uint64_t *ns)
{
    uint64_t v;

    if (!PTP_TB_Convert(SYS_TIME_Counter64Get(), &v))
    {
        return false;
    }

    /* Monotonic guard.  A refit can move the model slightly backwards, and a
       caller that orders events by this value would see time run backwards
       (plan B.6).  Convert() is deliberately NOT guarded - it must stay a pure
       function of its argument so past timestamps convert reproducibly. */
    if (v < s_mono_last_ns)
    {
        v = s_mono_last_ns;
    }
    s_mono_last_ns = v;

    *ns = v;
    return true;
}

bool PTP_TB_LocalFor(uint64_t ns, uint64_t *local_ticks)
{
    if (s_state == PTP_TB_UNINIT || local_ticks == NULL)
    {
        return false;
    }

    if (ns >= s_anchor_ns)
    {
        *local_ticks = s_anchor_L + tb_span_ticks(ns - s_anchor_ns, s_slope_q24);
    }
    else
    {
        *local_ticks = s_anchor_L - tb_span_ticks(s_anchor_ns - ns, s_slope_q24);
    }
    return true;
}

void PTP_TB_StatusGet(PTP_TB_STATUS *out)
{
    uint64_t now;
    int64_t  lo;
    int64_t  hi;
    uint32_t i;

    if (out == NULL)
    {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->state     = s_state;
    out->pairs     = s_cnt_pairs;
    out->winners   = s_win_cnt;
    out->rejected  = s_cnt_rejected;
    out->reanchors = s_cnt_reanchor;
    out->slope_q24 = s_slope_q24;
    out->baseline_ns = s_baseline_ns;
    out->last_resid_ns = s_last_resid;

    /* Slope deviation from nominal, in ppb.  This is the local tick source
       against the grandmaster - the number that was +783 ppm on the open-loop
       DFLL and -62.7 ppm after the clock patch. */
    if (s_slope_nominal_q24 != 0u)
    {
        int64_t d = (int64_t)s_slope_q24 - (int64_t)s_slope_nominal_q24;
        out->slope_ppb = (int32_t)((d * 1000000000LL) / (int64_t)s_slope_nominal_q24);
    }

    if (s_state != PTP_TB_UNINIT)
    {
        now = SYS_TIME_Counter64Get();
        out->age_ms = (now - s_last_pair_tick) / s_ticks_per_ms;
    }

    if (s_win_cnt >= 2u)
    {
        uint32_t oldest = (s_win_head + TB_WINNERS - s_win_cnt) % TB_WINNERS;
        lo = s_win_resid[oldest];
        hi = lo;
        for (i = 1u; i < s_win_cnt; i++)
        {
            int64_t v = s_win_resid[(oldest + i) % TB_WINNERS];
            if (v < lo) { lo = v; }
            if (v > hi) { hi = v; }
        }
        out->win_spread_ns = (uint64_t)(hi - lo);
    }
}

uint32_t PTP_TB_TraceCount(void)
{
    return s_trace_n;
}

void PTP_TB_TraceSet(bool on)
{
    s_trace_on = on;
}

void PTP_TB_TraceClear(void)
{
    s_trace_n = 0u;
    s_trace_head = 0u;
    s_trace_t0 = 0u;
    s_trace_due = 0u;
    s_trace_calls = 0u;
    s_trace_writes = 0u;
}

uint32_t PTP_TB_TraceDump(uint32_t from, uint32_t count)
{
    uint32_t oldest;
    uint32_t i;

    /* THE BUFFER FIRST REPORTS ON ITSELF.  The first run delivered 29
       points for 300 s, and without this line "sampling stopped" could not
       be told apart from "printing broke off" or "somebody cleared it".
       Printed on EVERY call, not just the first chunk: someone checking in
       the middle of the series needs it just as much. */
    {
        uint64_t now = SYS_TIME_Counter64Get();
        SYS_CONSOLE_PRINT("[TRACE] rec %s   interval %lu ms   samples %lu of %u"
                          "   calls %lu   writes %lu   due in %ld ms\r\n",
                          s_trace_on ? "on" : "OFF", (unsigned long)s_trace_ms,
                          (unsigned long)s_trace_n, (unsigned)TB_TRACE_N,
                          (unsigned long)s_trace_calls,
                          (unsigned long)s_trace_writes,
                          (s_ticks_per_ms == 0u) ? 0L
                            : (long)(((int64_t)s_trace_due - (int64_t)now)
                                     / (int64_t)s_ticks_per_ms));
    }
    if (s_trace_n == 0u)
    {
        SYS_CONSOLE_PRINT("[TRACE] empty - nothing recorded yet\r\n");
        return 0u;
    }
    if (from >= s_trace_n) { return 0u; }
    oldest = (s_trace_head + TB_TRACE_N - s_trace_n) % TB_TRACE_N;

    if (from == 0u)
    {
        /* The header line names the units, so the output is readable without
           this file - it gets parsed by a script, not just read. */
        SYS_CONSOLE_PRINT("[TRACE] %lu samples every %u ms; q24 is the rate"
                          " correction, a JUMP there moves the phase\r\n",
                          (unsigned long)s_trace_n, (unsigned)TB_TRACE_MS);
        SYS_CONSOLE_PRINT("[TRACE] idx t_ms q24 anchorL anchorNs resid spread"
                          " pairs reanch state qual\r\n");
    }
    for (i = 0u; (i < count) && ((from + i) < s_trace_n); i++)
    {
        const tb_trace_t *e = &s_trace[(oldest + from + i) % TB_TRACE_N];
        SYS_CONSOLE_PRINT("[TRACE] %lu %lu %lu %lu %lu %ld %lu %lu %lu %u %u\r\n",
                          (unsigned long)(from + i), (unsigned long)e->t_ms,
                          (unsigned long)e->q24_lo, (unsigned long)e->anchor_L_lo,
                          (unsigned long)e->anchor_ns_lo, (long)e->resid_ns,
                          (unsigned long)e->spread_ns, (unsigned long)e->pairs,
                          (unsigned long)e->reanchors, e->state, e->quality);
    }
    return ((from + i) < s_trace_n) ? (from + i) : 0u;
}

uint64_t PTP_TB_RefAgeMs(void)
{
    uint64_t now;

    if (!s_ref_ok_valid || s_ticks_per_ms == 0u)
    {
        return UINT64_MAX;
    }
    now = SYS_TIME_Counter64Get();
    if (now <= s_ref_ok_tick)
    {
        return 0u;
    }
    return (now - s_ref_ok_tick) / s_ticks_per_ms;
}

PTP_TB_QUALITY PTP_TB_QualityGet(void)
{
    /* THE MAPPING IS A DECISION (some_ip.md 8.6 item 3), and the
     * 2026-08-16 measurement made it:
     *
     *   UNINIT           -> unsynced   there is no model
     *   HOLDOVER         -> INVALID    stage 0 halts the trigger here; a
     *                                  receiver that still fires, fires blind
     *   reference stale  -> uncertain  a time exists (1PPS keeps feeding), but
     *                                  it cannot be shown to be the master's
     *   ANCHORED         -> uncertain  offset known, slope still nominal
     *   LOCKED + fresh   -> CERTAIN    only here is acting allowed
     */
    if (s_state == PTP_TB_UNINIT)
    {
        return PTP_TB_Q_UNSYNCED;
    }
    if (s_state == PTP_TB_HOLDOVER)
    {
        return PTP_TB_Q_INVALID;
    }
    if (PTP_TB_RefAgeMs() > (uint64_t)TB_REF_STALE_MS)
    {
        return PTP_TB_Q_UNCERTAIN;
    }
    return (s_state == PTP_TB_LOCKED) ? PTP_TB_Q_CERTAIN : PTP_TB_Q_UNCERTAIN;
}

bool PTP_TB_IsUsable(void)
{
    /* Used to be `s_state == PTP_TB_LOCKED`.  That was correct on the PTP
       timebase, because there the loss of the source and the loss of sync
       are the same event - with 1PPS they are two, and only this version
       knows both. */
    return (PTP_TB_QualityGet() == PTP_TB_Q_CERTAIN);
}

/* --------------------------------------------------------------------------- */
/* CLI                                                                         */
/* --------------------------------------------------------------------------- */

static const char *tb_state_name(PTP_TB_STATE s)
{
    switch (s)
    {
        case PTP_TB_UNINIT:   return "UNINIT";
        case PTP_TB_ANCHORED: return "ANCHORED";
        case PTP_TB_LOCKED:   return "LOCKED";
        case PTP_TB_HOLDOVER: return "HOLDOVER";
        default:              return "?";
    }
}

static void cmd_tbase(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv)
{
    PTP_TB_STATUS st;
    uint64_t v;
    (void)pCmdIO;

    /* The trigger lives in the same group because MAX_CMD_GROUP is exhausted;
       it gets first refusal on the subcommand. */
    if (PPS_CliTry(argc, argv))
    {
        return;
    }

    /* `tbase peer` and `tbase sec` - the cross-measurement.  Its own module,
       its own registers, its own statistics; only the subgroup attaches
       here. */
    if (PEER_CliTry(argc, argv))
    {
        return;
    }

    if (TRIGCMD_CliTry(argc, argv))
    {
        return;
    }

    /* `tbase btn` - the buttons.  Attaches here and not in its own group,
       because MAX_CMD_GROUP is exhausted and a SYS_CMD_ADDGRP would then be
       SILENTLY refused. */
    if (BTN_CliTry(argc, argv))
    {
        return;
    }

    /* `tbase div` - der Gitterteiler der LED.  Eigenes Modul, eigene Register;
       hier haengt nur die Untergruppe ein. */
    if (GDIV_CliTry(argc, argv))
    {
        return;
    }

    if (PTP_TRIG_CliTry(argc, argv))
    {
        return;
    }

    if (argc >= 3 && !strcmp(argv[1], "inject"))
    {
        /* FEED IN A DELIBERATELY WRONG PAIR - the proof for the outlier
         * filter.
         *
         * The filter is plausibly reasoned and UNPROVEN: the run afterwards
         * was clean, but it never triggered (`outliers: 0`), because no
         * outlier occurred in 884 s.  A clean result without a single
         * rejection says nothing about effectiveness.
         *
         * Waiting costs an hour per attempt (an outlier roughly every
         * 900 s).  So the case is MANUFACTURED - the same reasoning as
         * `trigfan phantom`: a fault case that cannot be manufactured is
         * one that cannot be signed off.
         *
         * What gets fed in is a pair with the CURRENT local tick and a
         * reference time that is off by `ns` - exactly the shape a real
         * outlier has.  If the filter works, `outliers` rises and the phase
         * stays quiet; if it does not, `pairs` rises and the residual
         * jumps.  Both are readable from `tbase`. */
        int64_t off = (int64_t)strtoll(argv[2], NULL, 0);
        /* SECOND PARAMETER: HOW MANY IN A ROW.  Without it, the give-up
         * bound (`TB_OUTLIER_RUN`) cannot be tested - about a second passes
         * between two CLI commands, and in that time a real 1PPS pair
         * arrives and resets the run.  Measured: 12 individually sent
         * outliers produced 12 rejections and `forced through: 0`.
         *
         * That is CORRECT behaviour - as long as good pairs keep arriving,
         * it should filter without limit.  It just leaves the counter-test
         * untested, and a path that blocks a real reference jump would be
         * the defect itself.  So do it in ONE pass, with no real pair in
         * between. */
        uint32_t n = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 0) : 1u;
        uint64_t l = SYS_TIME_Counter64Get();
        uint64_t ns = 0u;

        if (n == 0u || n > 64u) { n = 1u; }
        if (!PTP_TB_Convert(l, &ns))
        {
            SYS_CONSOLE_PRINT("[TBASE] inject: no model yet - nothing to disturb\r\n");
            return;
        }
        SYS_CONSOLE_PRINT("[TBASE] inject: %lu pair(s) at L=%llu with %+lld ns error"
                          " (source %s)\r\n", (unsigned long)n,
                          (unsigned long long)l, (long long)off,
                          (PTP_TB_SourceGet() == PTP_TB_SRC_PPS) ? "1PPS" : "PTP");
        for (uint32_t k = 0u; k < n; k++)
        {
            /* The local tick advances along with it, otherwise it would be
               the same pair twice, and the residual would have a different
               value at every step. */
            uint64_t lk = l + (uint64_t)k * s_ticks_per_ms;
            uint64_t nk = 0u;
            if (!PTP_TB_Convert(lk, &nk)) { break; }
            PTP_TB_SubmitPairFrom(PTP_TB_SourceGet(), lk, (uint64_t)((int64_t)nk + off));
        }
        return;
    }

    if (argc >= 2 && !strcmp(argv[1], "trace"))
    {
        /* `tbase trace` prints in CHUNKS, because 256 lines is over a
           second of line time - and a second of blocked main loop is
           itself exactly the disturbance this buffer is meant to
           uncover.  Without an argument: the next 32 from the start.
           `tbase trace <n>`: from entry n.  "done" comes after the last
           one. */
        if (argc >= 3 && !strcmp(argv[2], "off"))
        {
            PTP_TB_TraceSet(false);
            SYS_CONSOLE_PRINT("[TRACE] recording off (buffer kept)\r\n");
            return;
        }
        if (argc >= 3 && !strcmp(argv[2], "on"))
        {
            PTP_TB_TraceSet(true);
            SYS_CONSOLE_PRINT("[TRACE] recording on\r\n");
            return;
        }
        if (argc >= 4 && !strcmp(argv[2], "ms"))
        {
            uint32_t v = (uint32_t)strtoul(argv[3], NULL, 0);
            /* Lower bound because sampling itself costs main-loop time; no
               upper bound because a long run needs a long interval. */
            if (v < 20u)
            {
                SYS_CONSOLE_PRINT("[TRACE] %lu ms is too fast - 20 ms minimum\r\n",
                                  (unsigned long)v);
                return;
            }
            s_trace_ms = v;
            PTP_TB_TraceClear();
            SYS_CONSOLE_PRINT("[TRACE] interval %lu ms -> %lu s of history,"
                              " buffer cleared\r\n",
                              (unsigned long)v,
                              (unsigned long)((v * TB_TRACE_N) / 1000u));
            return;
        }
        if (argc >= 3 && !strcmp(argv[2], "clear"))
        {
            PTP_TB_TraceClear();
            SYS_CONSOLE_PRINT("[TRACE] cleared\r\n");
            return;
        }
        {
            uint32_t from = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0u;
            uint32_t next = PTP_TB_TraceDump(from, 32u);
            if (next == 0u)
            {
                SYS_CONSOLE_PRINT("[TRACE] done (%lu samples)\r\n",
                                  (unsigned long)PTP_TB_TraceCount());
            }
            else
            {
                SYS_CONSOLE_PRINT("[TRACE] next: tbase trace %lu\r\n",
                                  (unsigned long)next);
            }
        }
        return;
    }

    if (argc >= 2 && !strcmp(argv[1], "now"))
    {
        uint64_t l = SYS_TIME_Counter64Get();
        if (PTP_TB_Now(&v))
        {
            SYS_CONSOLE_PRINT("[TBASE] L=%llu -> %llu ns  (%llu.%09llu s)\r\n",
                              (unsigned long long)l, (unsigned long long)v,
                              (unsigned long long)(v / 1000000000ULL),
                              (unsigned long long)(v % 1000000000ULL));
        }
        else
        {
            SYS_CONSOLE_PRINT("[TBASE] no model yet\r\n");
        }
        return;
    }

    if (argc >= 3 && !strcmp(argv[1], "at"))
    {
        /* Round trip through the inverse map: a good check that Convert and
           LocalFor agree, which is what the trigger in phase C relies on. */
        uint64_t want = strtoull(argv[2], NULL, 0);
        uint64_t lt;
        if (PTP_TB_LocalFor(want, &lt) && PTP_TB_Convert(lt, &v))
        {
            SYS_CONSOLE_PRINT("[TBASE] %llu ns -> L=%llu -> %llu ns  (error %lld ns)\r\n",
                              (unsigned long long)want, (unsigned long long)lt,
                              (unsigned long long)v, (long long)((int64_t)v - (int64_t)want));
        }
        else
        {
            SYS_CONSOLE_PRINT("[TBASE] no model yet\r\n");
        }
        return;
    }

    if (argc >= 2 && !strcmp(argv[1], "gain"))
    {
        if (argc == 2)
        {
            SYS_CONSOLE_PRINT("[TBASE] gain: slope div %lu   offset div %lu"
                              "   (default %u / %u)\r\n",
                              (unsigned long)s_slope_div, (unsigned long)s_offset_div,
                              TB_SLOPE_DIV, TB_OFFSET_DIV);
            return;
        }
        if (argc >= 4)
        {
            long sd = strtol(argv[2], NULL, 0);
            long od = strtol(argv[3], NULL, 0);
            /* Zero would be a division by zero, one means "replace the model
               every time" - exactly the state that produced the +1.0..1.4 us
               jumps (E5).  Both are refused rather than silently tolerated. */
            if (sd < 1 || od < 1 || sd > 100000 || od > 100000)
            {
                SYS_CONSOLE_PRINT("[TBASE] gain: expected 1..100000, got %ld %ld\r\n",
                                  sd, od);
                return;
            }
            s_slope_div  = (uint32_t)sd;
            s_offset_div = (uint32_t)od;
            SYS_CONSOLE_PRINT("[TBASE] gain: slope div %lu   offset div %lu   set\r\n",
                              (unsigned long)s_slope_div, (unsigned long)s_offset_div);
            return;
        }
        SYS_CONSOLE_PRINT("[TBASE] gain [<slope_div> <offset_div>]\r\n");
        return;
    }

    if (argc >= 2 && !strcmp(argv[1], "syncmsg"))
    {
        /* Switchable off, because a long measurement should have no lines
           in between - a choked console looks like hung firmware (CLAUDE.md
           section 6).  Default is ON, because the demo is the more common
           case. */
        if (argc >= 3)
        {
            if (!strcmp(argv[2], "on"))
            {
                s_syncmsg = true;
            }
            else if (!strcmp(argv[2], "off"))
            {
                s_syncmsg = false;
            }
            else
            {
                long v = strtol(argv[2], NULL, 0);
                if (v < 100 || v > 10000000)
                {
                    SYS_CONSOLE_PRINT("[TBASE] syncmsg <ns> from 100 to"
                                      " 10000000 - refused rather than"
                                      " guessed.\r\n");
                    return;
                }
                s_syncmsg_ns = (int32_t)v;
                s_syncmsg_user = true;   /* takes precedence over the source default */
            }
        }
        SYS_CONSOLE_PRINT("[TBASE] syncmsg: %s   threshold %ld ns,"
                          " %u samples in a row   'lost' threshold %u ms\r\n",
                          s_syncmsg ? "on" : "off", (long)s_syncmsg_ns,
                          (unsigned)TB_SYNCMSG_RUN, (unsigned)TB_REF_STALE_MS);
        return;
    }

    if (argc >= 2 && !strcmp(argv[1], "reset"))
    {
        PTP_TB_Initialize();
        SYS_CONSOLE_PRINT("[TBASE] model cleared\r\n");
        return;
    }

    /* Anything with arguments that got this far is a typo, and printing status
       for it is worse than useless: "tbase 20 1" (a forgotten "per") looked like
       it had been accepted, and the board sat there with armed: no while the
       other one ran.  Same lesson the mirror command learned in cff7cdf -
       refuse a typo, do not quietly do something else. */
    if (argc >= 2)
    {
        SYS_CONSOLE_PRINT("[TBASE] unknown subcommand '%s'\r\n", argv[1]);
        SYS_CONSOLE_PRINT("usage: tbase | now | at <ns> | reset | trig | fire <ms> [id]"
                          " | per <ms|Nms|Nus> [id] | cancel | hw on|off | pin on|off"
                          " | mode strict|free | syncmsg [on|off|<ns>] | led on|off|blink [1|2]\r\n");
        return;
    }

    PTP_TB_StatusGet(&st);
    SYS_CONSOLE_PRINT("[TBASE] state: %s   usable: %s   age: %llu ms\r\n",
                      tb_state_name(st.state), PTP_TB_IsUsable() ? "yes" : "no",
                      (unsigned long long)st.age_ms);
    /* THE LINE THAT HAS TOLD THE TRUTH SINCE 2026-08-16.  `usable`/`state`
       above describe the model; this states whether it is bound to the
       master.  A follower that reports `LOCKED / usable yes` and
       `quality: uncertain` has A time, not A SHARED one - exactly the
       state that used to be invisible. */
    {
        static const char *qn[4] = { "unsynced", "uncertain", "certain", "invalid" };
        uint64_t ra = PTP_TB_RefAgeMs();
        SYS_CONSOLE_PRINT("[TBASE] quality: %s (%u)   master ref age: ",
                          qn[(unsigned)PTP_TB_QualityGet()],
                          (unsigned)PTP_TB_QualityGet());
        if (ra == UINT64_MAX)
        {
            SYS_CONSOLE_PRINT("never");
        }
        else
        {
            SYS_CONSOLE_PRINT("%llu ms", (unsigned long long)ra);
        }
        SYS_CONSOLE_PRINT("   seen: %lu   off-timeline: %lu   last dev: %lld ns\r\n",
                          (unsigned long)s_ref_seen, (unsigned long)s_ref_bad,
                          (long long)s_ref_last_dev_ns);
    }
    SYS_CONSOLE_PRINT("[TBASE] pairs: %lu   winners: %lu/%u   rejected: %lu   reanchors: %lu\r\n",
                      (unsigned long)st.pairs, (unsigned long)st.winners, (unsigned)TB_WINNERS,
                      (unsigned long)st.rejected, (unsigned long)st.reanchors);
    /* The emergency re-anchor belongs in the output even at 0: otherwise a
       healed model cannot be told apart from one that was never sick - and
       the question comes back up at every excursion. */
    SYS_CONSOLE_PRINT("[TBASE] emergency re-anchors: %lu   rejections in a row: %lu/%u"
                      "   last triggering residual: %+lld ns\r\n",
                      (unsigned long)s_cnt_poisoned, (unsigned long)s_resid_run,
                      (unsigned)TB_RESID_RUN, (long long)s_poison_last_ns);
    /* THE LINE THAT MAKES THE PHASE EXCURSION VISIBLE.  Before the tight
       filter there was no number for this: a 147 us outlier ran unnoticed
       into the winner ring, distorted the fit for ~70 s, and left nothing
       behind afterwards.  `forced` is the counter-check - if it triggers,
       the filter is blocking a REAL jump of the reference, and then it is
       the defect itself. */
    SYS_CONSOLE_PRINT("[TBASE] outliers: %lu rejected (%+lld .. %+lld ns)"
                      "   forced through: %lu   limit %lld ns\r\n",
                      (unsigned long)s_cnt_outlier,
                      (long long)s_outlier_min_ns, (long long)s_outlier_max_ns,
                      (unsigned long)s_cnt_outlier_forced,
                      (long long)TB_OUTLIER_NS);
    /* AND THE NUMBER THE LINE ABOVE CANNOT DELIVER.  The extrema above
       arise in the aftermath of a bent fit; this states what a run STARTED
       with.  That is the quantity a cause should be sought against - and
       the wrong target quantity once checked two candidates and dismissed
       one prematurely (E38). */
    SYS_CONSOLE_PRINT("[TBASE] outlier runs: %lu   opening resid first %+lld"
                      "   last %+lld   min %+lld   max %+lld ns\r\n",
                      (unsigned long)s_outlier_runs,
                      (long long)s_outlier_open_first,
                      (long long)s_outlier_open_last,
                      (long long)s_outlier_open_min,
                      (long long)s_outlier_open_max);
    /* Read this AFTER a measurement, never during: it is the line that says
       whether the window was clean.  entries != 0 invalidates any drift figure
       taken over it. */
    SYS_CONSOLE_PRINT("[TBASE] holdover: entries %lu   total %lu ms   longest %lu ms\r\n",
                      (unsigned long)s_ho_entries,
                      (unsigned long)(s_ho_ticks / s_ticks_per_ms),
                      (unsigned long)(s_ho_longest / s_ticks_per_ms));
    /* The gains belong next to the slope: a measurement run that does not
       log its own controller settings cannot be attributed later - and
       these exact two values are what is now being varied. */
    SYS_CONSOLE_PRINT("[TBASE] gain: slope div %lu   offset div %lu\r\n",
                      (unsigned long)s_slope_div, (unsigned long)s_offset_div);
    SYS_CONSOLE_PRINT("[TBASE] slope: %ld ppb vs nominal   q24: %llu   baseline: %llu ms\r\n",
                      (long)st.slope_ppb, (unsigned long long)st.slope_q24,
                      (unsigned long long)(st.baseline_ns / 1000000ULL));
    SYS_CONSOLE_PRINT("[TBASE] last residual: %lld ns   winner spread: %llu ns\r\n",
                      (long long)st.last_resid_ns, (unsigned long long)st.win_spread_ns);
    SYS_CONSOLE_PRINT("[TBASE] block: %lu/%u samples\r\n",
                      (unsigned long)s_block_n, (unsigned)TB_BLOCK);
}

static const SYS_CMD_DESCRIPTOR tbase_cmd_tbl[] = {
    {"tbase", (SYS_CMD_FNC)cmd_tbase, ": timebase + trigger (now|at <ns>|reset|trig|fire <ms>|per <ms>|cancel|mode)"},
};

void PTP_TB_CliRegister(void)
{
    if (!SYS_CMD_ADDGRP(tbase_cmd_tbl, (int)(sizeof tbase_cmd_tbl / sizeof *tbase_cmd_tbl),
                        "tbase", ": corrected MCU timebase"))
    {
        SYS_CONSOLE_PRINT("TBASE: SYS_CMD_ADDGRP failed\r\n");
    }
}
