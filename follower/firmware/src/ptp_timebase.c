/*******************************************************************************
  Corrected MCU timebase - implementation

  See ptp_timebase.h for what this is and why it fits against t1 rather than
  t2.  Phase B of PTP_TIMEBASE_PLAN.md.
*******************************************************************************/

#include <stdlib.h>
#include <string.h>
#include "ptp_timebase.h"
#include "ptp_trigger.h"
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

/* No fresh pair for this long -> HOLDOVER.  At a 100 ms interval that is 30
   missed cycles; at 500 ms it is 6.  Deliberately in ms, not in cycles, so it
   does not silently change meaning when the master's interval changes. */
#define TB_STALE_MS     3000u

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
static uint64_t s_mono_last_ns;

static uint32_t s_cnt_pairs;
static uint32_t s_cnt_rejected;
static uint32_t s_cnt_reanchor;
static int64_t  s_last_resid;
static uint64_t s_baseline_ns;

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

static uint64_t tb_model_ns(uint64_t L)
{
    if (L >= s_anchor_L)
    {
        return s_anchor_ns + tb_span_ns(L - s_anchor_L, s_slope_q24);
    }
    return s_anchor_ns - tb_span_ns(s_anchor_L - L, s_slope_q24);
}

/* Move the anchor forward along the current model without new data.  Keeps the
   model identical, only re-bases it - so it is safe in HOLDOVER too. */
static void tb_reanchor(uint64_t L)
{
    s_anchor_ns = tb_model_ns(L);
    s_anchor_L  = L;
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

    dL  = b->L - a->L;
    dns = b->ns - a->ns;

    /* slope = dns/dL in Q24.  dns is ns over the baseline (~5e10 for 50 s), so
       dns << 24 would overflow - divide first, then fold in the remainder. */
    {
        uint64_t q = dns / dL;
        uint64_t r = dns - q * dL;
        s_slope_q24 = (q << TB_Q) + ((r << TB_Q) / dL);
    }

    s_baseline_ns = dns;

    /* Re-base onto the newest winner so the anchor sits on a measured point
       rather than on an extrapolation. */
    s_anchor_L  = b->L;
    s_anchor_ns = b->ns;
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
    s_anchor_L = 0u;
    s_anchor_ns = 0u;
    s_state = PTP_TB_UNINIT;
    s_last_pair_tick = 0u;
    s_mono_last_ns = 0u;
    s_cnt_pairs = 0u;
    s_cnt_rejected = 0u;
    s_cnt_reanchor = 0u;
    s_last_resid = 0;
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

void PTP_TB_SubmitPair(uint64_t local_ticks, uint64_t ref_ns)
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
        s_anchor_L = p.L;
        s_anchor_ns = p.ns;
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

    if (resid > TB_RESID_MAX_NS || resid < -TB_RESID_MAX_NS)
    {
        /* Almost certainly a pair formed across a lost frame.  Feeding it to
           the fit would move the anchor by an interval, silently. */
        s_cnt_rejected++;
        return;
    }

    if (s_block_n == 0u || resid < s_block_best_resid)
    {
        s_block_best = p;
        s_block_best_resid = resid;
    }
    s_block_n++;

    if (s_block_n >= TB_BLOCK)
    {
        tb_winner_push(&s_block_best, s_block_best_resid);
        s_block_n = 0u;
        tb_refit();
    }
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

    /* Holdover: the model still answers, but on a slope that is getting old.
       Reported rather than hidden - a follower that looks locked while nothing
       arrives is exactly the trap the existing servo status falls into. */
    age_ticks = now - s_last_pair_tick;
    if (age_ticks > (uint64_t)TB_STALE_MS * s_ticks_per_ms)
    {
        if (s_state != PTP_TB_HOLDOVER)
        {
            s_state = PTP_TB_HOLDOVER;
        }
    }
    else if (s_state == PTP_TB_HOLDOVER)
    {
        /* Pairs are flowing again.  LOCKED only if a slope was ever fitted. */
        s_state = (s_win_cnt >= 2u) ? PTP_TB_LOCKED : PTP_TB_ANCHORED;
    }
    else
    {
        /* nothing to do */
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

bool PTP_TB_IsUsable(void)
{
    return (s_state == PTP_TB_LOCKED);
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
    if (PTP_TRIG_CliTry(argc, argv))
    {
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

    if (argc >= 2 && !strcmp(argv[1], "reset"))
    {
        PTP_TB_Initialize();
        SYS_CONSOLE_PRINT("[TBASE] model cleared\r\n");
        return;
    }

    PTP_TB_StatusGet(&st);
    SYS_CONSOLE_PRINT("[TBASE] state: %s   usable: %s   age: %llu ms\r\n",
                      tb_state_name(st.state), PTP_TB_IsUsable() ? "yes" : "no",
                      (unsigned long long)st.age_ms);
    SYS_CONSOLE_PRINT("[TBASE] pairs: %lu   winners: %lu/%u   rejected: %lu   reanchors: %lu\r\n",
                      (unsigned long)st.pairs, (unsigned long)st.winners, (unsigned)TB_WINNERS,
                      (unsigned long)st.rejected, (unsigned long)st.reanchors);
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
