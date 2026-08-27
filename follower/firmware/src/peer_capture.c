/*******************************************************************************
  peer_capture.c - implementation.  The WHAT and WHY are in peer_capture.h.
*******************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "definitions.h"
#include "config/default/system/console/sys_console.h"
#include "system/command/sys_command.h"
#include "peer_capture.h"
#include "board_pins.h"
#include "hw_shared.h"
#include "ptp_trigger.h"
#include "pps_capture.h"
#include "pin_table.h"

/* ------------------------------------------------------------------ Stage 1
 * REGISTER ACCESS SIGN-OFF (REGISTERZUGRIFF_PRINZIP.md 5, the same bracket
 * as in pps_capture.c).  Every address, access width and field position is
 * pinned against the DFP header at compile time.  The reason is not
 * tidiness: four times in this project a hand-written offset was wrong, and
 * a wrong offset reads a neighbouring, plausible-looking register.  While
 * writing this file it happened a fifth time - PINCFG sits at +0x40, not at
 * +0x08 (that is DIRSET) -, found by exactly this kind of assertion.
 * Source of the expected values: component/tc.h, component/eic.h,
 * instance/tc4.h.
 */
#define PC_ADDR_OF(x)  ((uintptr_t)&(x))

_Static_assert(PC_ADDR_OF(TC4_REGS->COUNT32.TC_CTRLA)    == 0x42001400u, "TC4_CTRLA");
_Static_assert(PC_ADDR_OF(TC4_REGS->COUNT32.TC_CTRLBSET) == 0x42001405u, "TC4_CTRLBSET");
_Static_assert(PC_ADDR_OF(TC4_REGS->COUNT32.TC_EVCTRL)   == 0x42001406u, "TC4_EVCTRL");
_Static_assert(PC_ADDR_OF(TC4_REGS->COUNT32.TC_INTFLAG)  == 0x4200140Au, "TC4_INTFLAG");
_Static_assert(PC_ADDR_OF(TC4_REGS->COUNT32.TC_STATUS)   == 0x4200140Bu, "TC4_STATUS");
_Static_assert(PC_ADDR_OF(TC4_REGS->COUNT32.TC_SYNCBUSY) == 0x42001410u, "TC4_SYNCBUSY");
_Static_assert(PC_ADDR_OF(TC4_REGS->COUNT32.TC_COUNT)    == 0x42001414u, "TC4_COUNT32");
_Static_assert(PC_ADDR_OF(TC4_REGS->COUNT32.TC_CC[0])    == 0x4200141Cu, "TC4_CC0");
_Static_assert(PC_ADDR_OF(TC4_REGS->COUNT32.TC_CCBUF[0]) == 0x42001430u, "TC4_CCBUF0");
_Static_assert(sizeof(TC4_REGS->COUNT32.TC_COUNT)  == 4u, "COUNT is 32 bit in 32-bit mode");
_Static_assert(sizeof(TC4_REGS->COUNT32.TC_CC[0])  == 4u, "CC0 is 32 bit in 32-bit mode");
/* EXTINT6 sits in CONFIG[0] (channels 0..7), not CONFIG[1] like EXTINT12.
   HW_EicClaim() works that out itself - the assertion is here because
   exactly this mix-up once swapped EVCTRL for CONFIG[1] in pps_capture.c. */
_Static_assert(PC_ADDR_OF(EIC_REGS->EIC_CONFIG[0]) == 0x4000281Cu, "EIC_CONFIG0");
_Static_assert(BOARD_PEER_EXTINT < 8u, "EXTINT6 must sit in CONFIG[0]");

/* --- Stage 1b: the event counter (TC3) -----------------------------------
 * Counts how many EVENTS the chain receives - against the number of pulses
 * the neighbour demonstrably sent.  This makes bounce decidable, and
 * without a reference window: exactly N in, exactly N out.
 * TC3 is free (TC2 runs in COUNT16, so the pair is not bound) and shares
 * GCLK channel 26 with TC2. */
_Static_assert(PC_ADDR_OF(TC3_REGS->COUNT16.TC_CTRLA)    == 0x4101C000u, "TC3_CTRLA");
_Static_assert(PC_ADDR_OF(TC3_REGS->COUNT16.TC_EVCTRL)   == 0x4101C006u, "TC3_EVCTRL");
_Static_assert(PC_ADDR_OF(TC3_REGS->COUNT16.TC_COUNT)    == 0x4101C014u, "TC3_COUNT16");
_Static_assert(PC_ADDR_OF(EVSYS_REGS->EVSYS_USER[47])    == 0x4100E1DCu, "EVSYS_USER[TC3]");
_Static_assert(TC3_GCLK_ID == 26u, "TC3 shares its GCLK channel with TC2");
_Static_assert(MCLK_APBBMASK_TC3_Msk == (1u << 14), "APBBMASK.TC3");
_Static_assert(EVENT_ID_USER_TC3_EVU == 47, "EVUSER TC3");
_Static_assert(TC_EVCTRL_EVACT_COUNT == 2u, "EVACT = COUNT");

_Static_assert(TC_CTRLA_CAPTEN0_Pos == 16u, "CAPTEN0 is bit 16, not 24");
_Static_assert(MCLK_APBCMASK_TC4_Msk == (1u << 5), "APBCMASK.TC4");
_Static_assert(MCLK_APBCMASK_TC5_Msk == (1u << 6), "APBCMASK.TC5");
_Static_assert(TC4_GCLK_ID == 30u, "TC4 GCLK channel");
_Static_assert(TC5_GCLK_ID == 30u, "TC4 and TC5 share the GCLK channel (32-bit pair)");
_Static_assert(EVENT_ID_GEN_EIC_EXTINT_6 == 24, "EVGEN EXTINT6");
_Static_assert(EVENT_ID_USER_TC4_EVU == 48, "EVUSER TC4");
/* --------------------------------------------------------------------------- */

/* EVSYS channel 3.  0 and 2 belong to the trigger (rising and falling
   edge), 1 to 1PPS capture - the occupancy table is in ptp_trigger.c and is
   the source of this number.  PEER_CAPTURE_PLAN.md named 2, which was
   wrong. */
#define PEER_EVSYS_CH       3u
#define EVSYS_CH_GCLK_BASE  EVSYS_GCLK_ID_0     /* EVSYS_CHANNEL_n = PCHCTRL[11+n] */

/* Errata DS80000748 2.20.1: the chip reports the capture done before the
   register holds it - CC0 then reads its reset value.  So wait on the
   VALUE, not on a flag.  Without this loop, 0 of 46 captures were usable
   on the 1PPS path; with it, 100 %.  A price, named rather than hidden: a
   capture that genuinely lands on 0xFFFFFFFF is discarded - one edge out
   of 4.3 billion. */
#define PEER_CAP_INVALID    0xFFFFFFFFu
#define PEER_CAP_SPINS      4000u

/* Calibration samples for the offset between the two time axes.  Odd, so
   the median is a real measurement and not the average of two
   neighbours. */
#define PEER_CAL_N          33u

/* This many samples sit in the ring buffer and thereby behind the median
   and MAD.  At a 10 kHz grid and one pickup per main-loop pass that is a
   fraction of a second - short enough that the number describes the
   CURRENT state, and long enough that a median means something. */
#define PEER_SAMPLES        64u

/* From when on a measurement series counts as stale.  A reading with no
   proof of freshness is a trap - the same class as the `armed: yes` that
   once lied, because the counter behind it was never printed. */
#define PEER_STALE_MS       2000u

typedef struct
{
    int32_t  buf[PEER_SAMPLES];
    uint32_t n;                 /* slots occupied (<= PEER_SAMPLES)            */
    uint32_t idx;               /* write pointer                               */
    uint32_t total;             /* all samples ever taken                      */
    int32_t  last;
    int32_t  min;
    int32_t  max;
    bool     have;
    uint64_t last_tick;         /* SYS_TIME tick of the most recent sample     */
    uint32_t sat;               /* samples at the edge of the measuring range  */
    uint64_t period_ticks;      /* grid period the measurement was taken with  */
} stats_t;

static stats_t  s_peer;
static stats_t  s_sec;

static bool     s_on = true;
static uint32_t s_ticks_per_us;
static uint64_t s_ticks_per_ms;

static uint32_t s_delta32;          /* (uint32)SYS_TIME - TC4 counter state     */
static bool     s_cal_ok;
static uint32_t s_cal_spread_tk;    /* spread of the KEPT samples                */
static uint32_t s_cal_wild;         /* discarded samples (read with no count)    */
static uint32_t s_cal_n;            /* how many samples carry the median         */
static uint32_t s_cal_gap_cyc;      /* largest measured read gap, CPU cycles     */

static uint32_t s_caps;             /* captures read                             */
static uint32_t s_no_grid;          /* capture with no grid reference -> discarded */
static uint32_t s_notready;         /* errata re-reads                           */
static uint32_t s_wait_max;
static uint32_t s_sec_seq;          /* number of the last processed second       */
static bool     s_hw_ok;            /* chain is up (otherwise: reported once)    */

/* Event counter: the reference state counting is done against.  What gets
   reset is not the counter (it runs in hardware), but this reference - a
   register write to a running TC would be the unnecessarily sharp way. */
static uint16_t s_ev_base;
static bool     s_ev_ok;

/* --------------------------------------------------------------------------- */
/* Computation                                                                 */
/* --------------------------------------------------------------------------- */

/* Bring a difference into [-period/2, +period/2).  That is also the range
   of applicability: if the boards are more than half a grid period apart,
   the measurement saturates - and then it must SAY so, or someone reads a
   saturated value as a measurement (exactly what once tripped up the
   evaluation in E24). */
static int64_t wrap_half(int64_t d, uint64_t p)
{
    int64_t period = (int64_t)p;
    int64_t half;

    if (period <= 0)
    {
        return 0;
    }
    half = period / 2;
    d %= period;                    /* C99: the remainder carries the dividend's sign */
    if (d > half)
    {
        d -= period;
    }
    else if (d < -half)
    {
        d += period;
    }
    return d;
}

static int32_t ticks_to_ns(int64_t ticks)
{
    if (s_ticks_per_us == 0u)
    {
        return 0;
    }
    return (int32_t)((ticks * 1000) / (int64_t)s_ticks_per_us);
}

static void st_reset(stats_t *s)
{
    memset(s, 0, sizeof(*s));
}

static void st_add(stats_t *s, int64_t phase_tk, uint64_t period_tk, uint64_t now)
{
    int32_t ns = ticks_to_ns(phase_tk);
    int64_t half = (int64_t)(period_tk / 2u);

    /* Periodenwechsel macht alte Proben bedeutungslos: sie wurden gegen ein anderes
       Gitter gemessen.  Verwerfen statt mischen. */
    if (s->period_ticks != period_tk)
    {
        uint32_t keep = s->total;
        st_reset(s);
        s->total = keep;
        s->period_ticks = period_tk;
    }

    if (half > 1 && (phase_tk >= half - 1 || phase_tk <= -half + 1))
    {
        s->sat++;
    }

    s->buf[s->idx] = ns;
    s->idx = (s->idx + 1u) % PEER_SAMPLES;
    if (s->n < PEER_SAMPLES)
    {
        s->n++;
    }
    s->total++;
    s->last = ns;
    if (!s->have)
    {
        s->min = ns;
        s->max = ns;
        s->have = true;
    }
    else
    {
        if (ns < s->min) { s->min = ns; }
        if (ns > s->max) { s->max = ns; }
    }
    s->last_tick = now;
}

static void isort(int32_t *a, uint32_t n)
{
    uint32_t i, j;
    for (i = 1u; i < n; i++)
    {
        int32_t v = a[i];
        for (j = i; j > 0u && a[j - 1u] > v; j--)
        {
            a[j] = a[j - 1u];
        }
        a[j] = v;
    }
}

static void st_eval(const stats_t *s, PEER_PHASE *out)
{
    int32_t tmp[PEER_SAMPLES];
    uint32_t i;
    uint64_t now;

    memset(out, 0, sizeof(*out));
    if (!s->have || s->n == 0u)
    {
        return;
    }

    memcpy(tmp, s->buf, sizeof(int32_t) * s->n);
    isort(tmp, s->n);
    out->median_ns = tmp[s->n / 2u];

    for (i = 0u; i < s->n; i++)
    {
        int32_t d = tmp[i] - out->median_ns;
        tmp[i] = (d < 0) ? -d : d;
    }
    isort(tmp, s->n);
    out->mad_ns = tmp[s->n / 2u];

    out->valid = true;
    out->last_ns = s->last;
    out->min_ns = s->min;
    out->max_ns = s->max;
    out->samples = s->n;
    out->total = s->total;
    out->saturated = (s->sat != 0u);

    now = SYS_TIME_Counter64Get();
    out->age_ms = (s_ticks_per_ms != 0u && now > s->last_tick)
                  ? (uint32_t)((now - s->last_tick) / s_ticks_per_ms) : 0u;
}

/* --------------------------------------------------------------------------- */
/* TC4                                                                         */
/* --------------------------------------------------------------------------- */

/* The counter state RIGHT NOW.  COUNT is read-synchronized, so the command
   must come before the read and the handshake must be waited out. */
static uint32_t tc4_count(void)
{
    TC4_REGS->COUNT32.TC_CTRLBSET = TC_CTRLBSET_CMD_READSYNC;
    while ((TC4_REGS->COUNT32.TC_SYNCBUSY & TC_SYNCBUSY_COUNT_Msk) != 0u)
    {
    }
    return TC4_REGS->COUNT32.TC_COUNT;
}

/* Pick up a new capture, if one is there.
 *
 * MC0 is the right signal here, unlike on the 1PPS path: there it was read
 * IN THE ISR, a few hundred nanoseconds after the edge, and the question
 * was whether the value had already propagated through.  Here the main
 * loop sits between the edge and the read, and the only question is
 * WHETHER an edge came since last time - exactly what MC0 (write-1-clear)
 * says.
 *
 * A capture overrun (several edges between two pickups) is NOT a fault in
 * this design: it is sampled, not counted.  At 10 kHz, almost every edge is
 * necessarily lost, and each one carries the same phase. */
static bool cap_read(uint32_t *cap)
{
    uint32_t v;
    uint16_t w = 0u;

    if ((TC4_REGS->COUNT32.TC_INTFLAG & TC_INTFLAG_MC0_Msk) == 0u)
    {
        return false;
    }

    v = TC4_REGS->COUNT32.TC_CC[0];
    while (v == PEER_CAP_INVALID && w < PEER_CAP_SPINS)
    {
        w++;
        v = TC4_REGS->COUNT32.TC_CC[0];
    }
    TC4_REGS->COUNT32.TC_INTFLAG = TC_INTFLAG_MC0_Msk;

    if (w != 0u)
    {
        s_notready++;
        if (w > s_wait_max) { s_wait_max = w; }
    }
    if (v == PEER_CAP_INVALID)
    {
        return false;                   /* errata did not resolve - discard */
    }
    *cap = v;
    return true;
}

/* The offset between the two time axes, determined once.
 *
 * TC4 and the TC0 behind SYS_TIME both hang off GCLK1, so the offset is a
 * CONSTANT, not a drift - hence averaging over many samples is allowed, and
 * hence the jitter of the two reads enters only as a constant share instead
 * of into every measurement.  That is exactly the gain over the 1PPS path,
 * whose residual gap is measured at 158..442 cycles per edge.
 *
 * MEDIAN, not mean: a lost SYS_TIME overflow shifts individual samples by
 * exactly 65536 ticks (E41..E43).  A mean carries that along, a median does
 * not.
 *
 * And the computation runs over SIGNED distances to one reference sample,
 * not over the raw values: if the offset sits near the 32-bit wraparound,
 * 0xFFFFFFFF and 0x00000001 are two ticks apart but numerically the
 * farthest values from each other.  A median of the raw values would be
 * badly wrong there. */
/* How far apart two samples may sit and still belong to the same cloud.
   60,000 ticks is 1 ms - three orders of magnitude above the expected
   spread (tens of ticks) and well below what a wild sample delivers
   (measured at 1.6 billion ticks, i.e. a read that held no counter state at
   all). */
#define PEER_CAL_CLOUD_TK   60000

static void calibrate(void)
{
    uint32_t raw[PEER_CAL_N];
    int32_t  off[PEER_CAL_N];
    uint32_t i, j, n = 0u;
    uint32_t ref = 0u;
    bool     have_ref = false;

    s_cal_wild = 0u;
    s_cal_gap_cyc = 0u;

    for (i = 0u; i < PEER_CAL_N; i++)
    {
        uint32_t c, g0, gap = 0u;
        uint64_t syst;

        /* TC4 first, then SYS_TIME, immediately back to back - everything
           in between lands in the result.  Exactly this ordering brought
           the 1PPS path's timestamp jitter from 4167 down to 1572 ns on
           2026-08-19.
           BUT: "immediately" is not "simultaneously".  The residual gap is
           measured on the 1PPS path at 163..180 CPU cycles, and it enters
           delta32 as a SYSTEMATIC offset - with its own value on each
           board.  In the half-difference of two boards, HALF of that
           asymmetry remains, and that was exactly the +119 ns offset
           against the Saleae (A2, 2026-08-20): after recalibrating it
           jumped to -80 ns, so it tracked the calibration.
           Hence the gap is MEASURED and subtracted, instead of swallowed.
           The DWT runs at double the clock (120 vs 60 MHz), so ticks =
           cycles / 2. */
        c = tc4_count();
        g0 = DWT->CYCCNT;
        syst = SYS_TIME_Counter64Get();
        if (PTP_TRIG_DwtOk())
        {
            gap = DWT->CYCCNT - g0;
            if (gap > s_cal_gap_cyc) { s_cal_gap_cyc = gap; }
        }
        raw[i] = (uint32_t)syst - c - (gap / 2u);
    }

    /* A REFERENCE SAMPLE WITH A NEIGHBOUR.  Taking sample 0 as the
       reference is wrong if sample 0 happens to be the wild one - then
       every other sample looks wild and the calibration adopts garbage.
       So the reference is chosen such that at least one second sample sits
       close to it. */
    for (i = 0u; i < PEER_CAL_N && !have_ref; i++)
    {
        for (j = 0u; j < PEER_CAL_N; j++)
        {
            int32_t o;
            if (j == i) { continue; }
            o = (int32_t)(raw[j] - raw[i]);
            if (o <= PEER_CAL_CLOUD_TK && o >= -PEER_CAL_CLOUD_TK)
            {
                ref = raw[i];
                have_ref = true;
                break;
            }
        }
    }
    if (!have_ref)
    {
        /* No two samples close together - then the chain is broken, and
           printing a number would be worse than none. */
        s_cal_ok = false;
        s_cal_wild = PEER_CAL_N;
        return;
    }

    for (i = 0u; i < PEER_CAL_N; i++)
    {
        int32_t o = (int32_t)(raw[i] - ref);
        if (o > PEER_CAL_CLOUD_TK || o < -PEER_CAL_CLOUD_TK)
        {
            s_cal_wild++;               /* counted, not silently discarded */
            continue;
        }
        off[n++] = o;
    }

    isort(off, n);
    s_delta32 = ref + (uint32_t)off[n / 2u];
    /* The spread ONLY over the kept samples - this used to be min..max over
       all of them, and a single wild sample turned that into 1.6 billion
       ticks. */
    s_cal_spread_tk = (uint32_t)(off[n - 1u] - off[0]);
    s_cal_n = n;
    s_cal_ok = true;
}

/* --------------------------------------------------------------------------- */
/* Setup                                                                       */
/* --------------------------------------------------------------------------- */

static bool peer_pin_and_eic_init(void)
{
    /* PD11 -> peripheral function A (EIC), input buffer on.  PMUX is one
       byte for TWO pins, hence via hw_shared.c. */
    HW_PinMux(BOARD_PEER_GROUP, BOARD_PEER_PIN, 0u /* peripheral A */, true);

    /* PULL-DOWN.  Without it the input is an antenna when the cable is
       unplugged or the neighbour is off, and noise turns into edges - a
       phase made of randomness.  With it the pin reads a stable 0 and the
       measurement honestly reports "no edge".
       Direction is chosen by the OUT bit as long as DIR = 0 and PULLEN = 1.
       PINCFG is one byte per pin and therefore exclusive - a
       read-modify-write on it shares nothing. */
    PORT_REGS->GROUP[BOARD_PEER_GROUP].PORT_OUTCLR = BOARD_PEER_MASK;
    PORT_REGS->GROUP[BOARD_PEER_GROUP].PORT_PINCFG[BOARD_PEER_PIN] |=
        PORT_PINCFG_PULLEN_Msk;

    if (!HW_EicClaim(BOARD_PEER_EXTINT, 1u /* RISE */, true /* ASYNCH */))
    {
        SYS_CONSOLE_PRINT("[PEER] EXTINT%u could not be claimed -"
                          " already taken (mask 0x%08lX).  The cross-measurement will NOT run.\r\n",
                          (unsigned)BOARD_PEER_EXTINT,
                          (unsigned long)HW_EicClaimed());
        return false;
    }
    if (!HW_EicEventEnable(BOARD_PEER_EXTINT, true))
    {
        SYS_CONSOLE_PRINT("[PEER] EXTINT%u cannot emit an event -"
                          " the cross-measurement will NOT run.\r\n",
                          (unsigned)BOARD_PEER_EXTINT);
        return false;
    }

    /* NO NVIC_EnableIRQ - and that is the load-bearing design decision, not
       an omission.  At a 100 us grid that would be 10,000 interrupts a
       second, and a measuring tool that degrades the quantity it measures
       is not one (E41..E44).  A consequence worth knowing: EIC_INTFLAG for
       this channel sets on every edge and is never cleared.  Harmless as
       long as the NVIC channel is off - whoever enables it later gets an
       interrupt immediately. */
    return true;
}

static void peer_tc4_init(void)
{
    /* TC4 and TC5: in 32-bit mode, TC5 provides the upper 16 bits.  Enable
       both bus clocks, one GCLK channel for the pair
       (TC4_GCLK_ID == TC5_GCLK_ID == 30).  Read-modify-write via
       hw_shared.c - a bare write to APBCMASK would switch off the rest of
       this bus's peripherals. */
    HW_ApbcClockEnable(MCLK_APBCMASK_TC4_Msk | MCLK_APBCMASK_TC5_Msk);

    /* GCLK1 = 60 MHz, the same source as TC0 behind SYS_TIME.  A different
       source would make it a ratio instead of an offset, and then the
       calibration above would not be a constant. */
    GCLK_REGS->GCLK_PCHCTRL[TC4_GCLK_ID] = GCLK_PCHCTRL_GEN_GCLK1 | GCLK_PCHCTRL_CHEN_Msk;
    while ((GCLK_REGS->GCLK_PCHCTRL[TC4_GCLK_ID] & GCLK_PCHCTRL_CHEN_Msk) == 0u)
    {
    }

    TC4_REGS->COUNT32.TC_CTRLA = 0u;
    while ((TC4_REGS->COUNT32.TC_SYNCBUSY & TC_SYNCBUSY_ENABLE_Msk) != 0u)
    {
    }
    TC4_REGS->COUNT32.TC_CTRLA = TC_CTRLA_MODE_COUNT32 | TC_CTRLA_CAPTEN0_Msk;
    TC4_REGS->COUNT32.TC_EVCTRL = TC_EVCTRL_TCEI_Msk | TC_EVCTRL_EVACT_STAMP;
    TC4_REGS->COUNT32.TC_CTRLA |= TC_CTRLA_ENABLE_Msk;
    while ((TC4_REGS->COUNT32.TC_SYNCBUSY & TC_SYNCBUSY_ENABLE_Msk) != 0u)
    {
    }
}

/* --------------------------------------------------------------------------- */
/* TC3: count events instead of ticks                                          */
/* --------------------------------------------------------------------------- */

/* WHY IN HARDWARE AND NOT IN AN ISR.
 *
 * An ISR on the EXTINT could LOSE EXACTLY WHAT is being looked for: the EIC
 * flag is write-1-clear, and if a bounce's second pulse sets the flag while
 * the ISR is clearing it, the clear wins - the double pulse then becomes
 * invisible.  On top of that would come the interrupt load at 10 kHz, which
 * is exactly what the whole design is built against (E41..E44).
 * `EVACT = COUNT` counts in hardware and cannot merge anything the EVSYS
 * channel lets through.
 *
 * TC3 hangs off the SAME channel 3 as TC4 - one EVSYS channel may have
 * several consumers.  So it counts exactly the events the capture chain
 * receives, not a second, separately running view.
 *
 * A LIMIT that belongs with the result: the channel runs resynchronized at
 * 60 MHz, and two edges closer than ~17 ns merge into one event there.
 * Bounce that fast is invisible - and harmless, because whatever produces
 * no second event produces no second capture.  For the electrical truth at
 * the pin, the analyser on EXT1 pin 6 is needed. */
static void peer_tc3_count_init(void)
{
    HW_ApbbClockEnable(MCLK_APBBMASK_TC3_Msk);
    /* The same channel as TC2, the same value - pps_capture.c has usually
       already turned it on.  A second write with the same content has no
       effect, and relying on the order of two modules would be worse. */
    GCLK_REGS->GCLK_PCHCTRL[TC3_GCLK_ID] = GCLK_PCHCTRL_GEN_GCLK1 | GCLK_PCHCTRL_CHEN_Msk;
    while ((GCLK_REGS->GCLK_PCHCTRL[TC3_GCLK_ID] & GCLK_PCHCTRL_CHEN_Msk) == 0u)
    {
    }

    TC3_REGS->COUNT16.TC_CTRLA = 0u;
    while ((TC3_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_ENABLE_Msk) != 0u)
    {
    }
    TC3_REGS->COUNT16.TC_CTRLA = TC_CTRLA_MODE_COUNT16;
    TC3_REGS->COUNT16.TC_EVCTRL = TC_EVCTRL_TCEI_Msk | TC_EVCTRL_EVACT_COUNT;
    TC3_REGS->COUNT16.TC_CTRLA |= TC_CTRLA_ENABLE_Msk;
    while ((TC3_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_ENABLE_Msk) != 0u)
    {
    }
    EVSYS_REGS->EVSYS_USER[EVENT_ID_USER_TC3_EVU] = PEER_EVSYS_CH + 1u;

    s_ev_ok = (TC3_REGS->COUNT16.TC_EVCTRL
               == (TC_EVCTRL_TCEI_Msk | TC_EVCTRL_EVACT_COUNT));
    s_ev_base = 0u;
}

/* COUNT is read-synchronized - and there is a trap here this project has
 * already paid for once with `CC0`: WAITING ON A FLAG THAT IS NOT SET YET.
 *
 * The first build wrote `READSYNC` and then ran into
 *     while (SYNCBUSY.COUNT) { }
 * Immediately after the command, `SYNCBUSY.COUNT` has NOT appeared yet (the
 * write itself must first cross the clock boundary), so the loop finished
 * at once, and the state read was from BEFORE the synchronization.
 *
 * MEASURED 2026-08-20, and the finding is unambiguous: within the same
 * output line, the raw-count difference lagged by exactly one call -
 *     count: since reference 2000, raw state 9000
 *     100 pulses sent
 *     count: since reference 2000, raw state 9100     <- first read stale
 *     count: since reference 2100, raw state 9100
 * Both numbers come from the same peer_events_raw(); the first call
 * returned the old state, the second the fresh one.
 *
 * The correct way is TWO-STAGE: first wait for SYNCBUSY to APPEAR (bounded -
 * the synchronization may already be done), then for it to disappear.
 *
 * THE SAME STRUCTURE APPEARS IN TWO MORE PLACES: `tc4_count()` below and
 * the TC2 read in `pps_capture.c`.  There, a value stale by one
 * synchronization is a CONSTANT offset and vanishes into the anchor or into
 * `delta32` - harmless, but the same latent trap. */
#define PEER_SYNC_SPINS 64u

static uint16_t peer_events_raw(void)
{
    uint32_t w;

    TC3_REGS->COUNT16.TC_CTRLBSET = TC_CTRLBSET_CMD_READSYNC;
    for (w = 0u; w < PEER_SYNC_SPINS; w++)
    {
        if ((TC3_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_COUNT_Msk) != 0u)
        {
            break;                  /* synchronization running - now wait */
        }
    }
    while ((TC3_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_COUNT_Msk) != 0u)
    {
    }
    return TC3_REGS->COUNT16.TC_COUNT;
}

static uint16_t peer_events_since_base(void)
{
    return (uint16_t)(peer_events_raw() - s_ev_base);   /* correct across a 16-bit wrap */
}

static void peer_evsys_init(void)
{
    /* ITS OWN GENERIC CLOCK FOR THE CHANNEL.  The resynchronized path needs
       it; forgetting it is SILENT - the channel accepts the configuration,
       reads back correctly, and never delivers.  The 1PPS chain once
       failed exactly this way. */
    GCLK_REGS->GCLK_PCHCTRL[EVSYS_CH_GCLK_BASE + PEER_EVSYS_CH] =
        GCLK_PCHCTRL_GEN_GCLK1 | GCLK_PCHCTRL_CHEN_Msk;
    while ((GCLK_REGS->GCLK_PCHCTRL[EVSYS_CH_GCLK_BASE + PEER_EVSYS_CH]
            & GCLK_PCHCTRL_CHEN_Msk) == 0u)
    {
    }

    /* RESYNCHRONIZED, not ASYNCHRONOUS: a TC capture user samples the
       event on ITS OWN clock, so on the asynchronous path the capture
       would never happen - CC0 would then keep its value from bring-up,
       and the computation delivers a number that looks plausible and is
       wrong by a whole second modulo the counter width.  EDGSEL is the
       second half of the same trap: its reset value is NO_EVT_OUTPUT, i.e.
       nothing. */
    EVSYS_REGS->CHANNEL[PEER_EVSYS_CH].EVSYS_CHANNEL =
        EVSYS_CHANNEL_EVGEN(EVENT_ID_GEN_EIC_EXTINT_6)
        | EVSYS_CHANNEL_PATH_RESYNCHRONIZED
        | EVSYS_CHANNEL_EDGSEL_RISING_EDGE;
    EVSYS_REGS->EVSYS_USER[EVENT_ID_USER_TC4_EVU] = PEER_EVSYS_CH + 1u;
}

void PEER_Initialize(void)
{
    uint32_t f = SYS_TIME_FrequencyGet();

    st_reset(&s_peer);
    st_reset(&s_sec);
    s_ticks_per_us = (f != 0u) ? (f / 1000000u) : 1u;
    s_ticks_per_ms = (f != 0u) ? (uint64_t)(f / 1000u) : 1u;
    if (s_ticks_per_us == 0u) { s_ticks_per_us = 1u; }
    if (s_ticks_per_ms == 0u) { s_ticks_per_ms = 1u; }

    s_hw_ok = peer_pin_and_eic_init();
    if (!s_hw_ok)
    {
        return;
    }
    peer_tc4_init();
    peer_evsys_init();
    peer_tc3_count_init();
    calibrate();

    /* Discard a capture left pending from setup. */
    TC4_REGS->COUNT32.TC_INTFLAG = TC_INTFLAG_MC0_Msk;
}

/* --------------------------------------------------------------------------- */
/* Operation                                                                   */
/* --------------------------------------------------------------------------- */

void PEER_Tasks(void)
{
    uint64_t ref, period;
    uint32_t cap;

    if (!s_on || !s_hw_ok)
    {
        return;
    }

    /* Stage 1 - the neighbour's edge. */
    if (cap_read(&cap))
    {
        s_caps++;
        if (s_cal_ok && PTP_TRIG_GridRef(&ref, &period))
        {
            uint32_t expected = (uint32_t)ref - s_delta32;
            int32_t  d = (int32_t)(cap - expected);
            st_add(&s_peer, wrap_half((int64_t)d, period), period,
                   SYS_TIME_Counter64Get());
        }
        else
        {
            /* No grid reference means no phase.  Discarded and COUNTED -
               otherwise the user sees zero samples and goes looking for
               the fault at the cable. */
            s_no_grid++;
        }
    }

    /* Stage 0 - the own second against the own grid.  No wire involved. */
    {
        uint64_t tick;
        uint32_t seq;

        if (PPS_LastEdge(&tick, &seq) && seq != s_sec_seq)
        {
            s_sec_seq = seq;
            if (PTP_TRIG_GridRef(&ref, &period))
            {
                int64_t d = (int64_t)tick - (int64_t)ref;
                st_add(&s_sec, wrap_half(d, period), period, SYS_TIME_Counter64Get());
            }
        }
    }
}

bool PEER_PeerPhase(PEER_PHASE *out)
{
    st_eval(&s_peer, out);
    return out->valid;
}

bool PEER_SecondPhase(PEER_PHASE *out)
{
    st_eval(&s_sec, out);
    return out->valid;
}

/* --------------------------------------------------------------------------- */
/* Mirroring: how long from the edge into the ISR?                             */
/* --------------------------------------------------------------------------- */

/* THE QUESTION, and why it cannot be answered without this mode.
 *
 * The whole design avoids an interrupt per edge (E41..E44), so the latency
 * "edge -> ISR entry" is not measured anywhere here.  But it is the number
 * that decides what an interrupt-based design could even achieve - and it
 * cannot be read from inside the controller, because every timestamp the
 * ISR itself takes already lies AFTER the entry.
 *
 * So it is done from outside: the ISR sets PD10 as its FIRST instruction,
 * the analyser sees stimulus (PD11 = the neighbour's PD10, the same net)
 * and response on two channels, and the difference is the latency - with
 * no clock inside the controller involved.
 *
 * OUTSET IS THE FIRST INSTRUCTION, deliberately: anything before it would
 * be measured along with it.  The rest (clear the flag, take back the
 * level) sits after it and does not falsify the rising edge.
 *
 * THE PIN BELONGS TO THE TRIGGER - `tbase pin off` must have released it,
 * or two sources would drive the same pin.  As with the generator, it is
 * taken over the regular way, via PIN_Claim(PIN_OWNER_GPIO).
 *
 * THIS IS A DIAGNOSTIC MODE, not an operating mode: it enables the
 * interrupt the module deliberately does not otherwise have.  While it
 * runs, this board's cross-measurement is not meaningful (the interrupt
 * disturbs exactly what it measures).  Hence the status output says so,
 * and `off` takes everything back. */
/* THE ONE-BIT SHIFT IS A BOARD PROPERTY, NOT A CONSTANT OF NATURE.
 * PD11 (input) happens to sit right next to PD10 (output) in the same PORT
 * group - hence one shift operation is enough to move the level from one
 * bit position to the other.  On a different board that does not hold, and
 * then the compiler must say so, not the hardware. */
_Static_assert(BOARD_PEER_GROUP == BOARD_TRIG_GROUP,
               "mirroring requires the same PORT group");
_Static_assert(BOARD_PEER_PIN == BOARD_TRIG_PIN + 1u,
               "mirroring shifts by exactly one bit (PD11 -> PD10)");

#define EIC_SENSE_RISE  1u
#define EIC_SENSE_BOTH  3u

static volatile bool     s_mirror;
static volatile uint32_t s_mirror_edges;

void EIC_EXTINT_6_Handler(void)
{
    /* COPY THE LEVEL, DO NOT REMEMBER AN EDGE - and that is the load-bearing
     * difference from the first version (which produced a 1 us pulse):
     *
     *   - A pulse remembers one event.  If an interrupt is lost, the pulse
     *     is missing, and the fault stays.  A copied level is IDEMPOTENT
     *     and self-healing: the ISR reads the CURRENT state, not the
     *     memory of an edge.  If it runs late, it still reads correctly
     *     (the level holds for 50 us), and a completely missed interrupt
     *     is corrected by the next one.
     *   - No busy-wait.  The first version spun for 1 us in this ISR,
     *     blocking TC1 with it (same priority = no preemption) and would
     *     have spun forever with the DWT stopped - pyOCD clears DEMCR,
     *     which was a real hang risk inside an interrupt.
     *
     * BOTH WRITES ARE UNCONDITIONAL and branch-free: if the level is high,
     * OUTSET fires and OUTCLR gets a 0 (no effect); if it is low, the other
     * way round.  No glitch in either direction, and no timing spread from
     * branch prediction.
     *
     * The `s_mirror` test stays nonetheless: NVIC_DisableIRQ does not clear
     * a PENDING flag, so the handler can still run once more - and by then
     * PD10 already belongs to the trigger again.  An unconditional write
     * there would be a fight over the same pin.  Cost: one load and one
     * branch, ~17 ns, and that is baked into the measured latency. */
    if (s_mirror)
    {
        uint32_t v = (PORT_REGS->GROUP[BOARD_PEER_GROUP].PORT_IN >> 1)
                     & BOARD_TRIG_MASK;
        PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUTSET = v;
        PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUTCLR = v ^ BOARD_TRIG_MASK;
        s_mirror_edges++;
    }

    /* UNCONDITIONAL.  A flag left uncleared makes the NVIC re-enter at
       once: interrupt storm, the main loop starves, the board looks dead.
       Only the own bit - a full mask would also clear EXTINT12's flag. */
    EIC_REGS->EIC_INTFLAG = (1u << BOARD_PEER_EXTINT);
}

static bool peer_mirror_set(bool on)
{
    if (on)
    {
        if (!PIN_Claim(BOARD_TRIG_PIN_INDEX, PIN_OWNER_GPIO))
        {
            SYS_CONSOLE_PRINT("[PEER] PD10 is held by '%s' - `tbase pin off` first.\r\n",
                              PIN_OwnerName(PIN_OwnerGet(BOARD_TRIG_PIN_INDEX)));
            return false;
        }
        /* BOTH EDGES, so the level is copied and not an event.
         *
         * TWO PRICES that come with it.  (1) `CONFIG` is enable-protected,
         * so the EIC instance briefly goes offline - that hits EXTINT12
         * too, a 1PPS pulse can be lost.  The time base can absorb it
         * (holdover only kicks in after 3 s), but it happens once per
         * switch on and off.  (2) TC4 afterwards captures BOTH edges.  In
         * toggle mode that is harmless, because every transition is a grid
         * point; in pulse mode the falling edge would be the pulse width
         * and would feed in false phases.  This is precisely why this
         * board's cross-measurement is not meaningful in mirror mode
         * anyway - the status output says so. */
        if (!HW_EicSenseSet(BOARD_PEER_EXTINT, EIC_SENSE_BOTH))
        {
            SYS_CONSOLE_PRINT("[PEER] EXTINT%u cannot be set to both edges"
                              " - mirroring NOT enabled.\r\n",
                              (unsigned)BOARD_PEER_EXTINT);
            PIN_Release(BOARD_TRIG_PIN_INDEX, PIN_OWNER_GPIO);
            return false;
        }
        PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUTCLR = BOARD_TRIG_MASK;
        PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_DIRSET = BOARD_TRIG_MASK;
        s_mirror_edges = 0u;
        s_mirror = true;
        /* Same priority as EXTINT12 and TC1 (all 0).  Same priority means
           NO preemption - so the measured latency holds for the case where
           no other ISR of this level is running at the time.  Whoever
           wants the loaded case leaves this board's trigger armed. */
        NVIC_SetPriority(EIC_EXTINT_6_IRQn, 0);
        NVIC_ClearPendingIRQ(EIC_EXTINT_6_IRQn);
        NVIC_EnableIRQ(EIC_EXTINT_6_IRQn);
        return true;
    }

    NVIC_DisableIRQ(EIC_EXTINT_6_IRQn);
    s_mirror = false;
    PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUTCLR = BOARD_TRIG_MASK;
    PIN_Release(BOARD_TRIG_PIN_INDEX, PIN_OWNER_GPIO);
    /* Back to RISING edge - otherwise TC4 keeps capturing both, and the
       cross-measurement would be silently wrong in pulse mode.  Order:
       interrupt off first, then switch; the instance briefly goes offline
       during it. */
    (void)HW_EicSenseSet(BOARD_PEER_EXTINT, EIC_SENSE_RISE);
    return true;
}

/* --------------------------------------------------------------------------- */
/* Pulse generator - the other half of the count check                         */
/* --------------------------------------------------------------------------- */

/* EXACTLY N RISING EDGES ON PD10, so the neighbour must count exactly N.
 *
 * Why in the firmware and not via debugger: an SWD write takes around a
 * millisecond, which can neither reproduce the real grid rate nor measure
 * much in a reasonable time.  Here one edge costs one register access.
 *
 * THE PIN BELONGS TO THE TRIGGER, and that is not worked around: `PIN_Claim`
 * with PIN_OWNER_GPIO only succeeds once `tbase pin off` has released the
 * trigger's claim.  Whoever forgets it gets a message instead of a silent
 * fight over the same pin.
 *
 * BLOCKING, deliberately: the loop stalls the main loop for n * us.  At
 * 1000 pulses of 100 us that is 100 ms - fine for a test command, and the
 * exact count is the point.  Interrupts stay on, so the spacing jitters if
 * the TC1 ISR steps in; the COUNT is unaffected by that. */
#define PEER_GEN_MAX        20000u
#define PEER_GEN_MIN_US     4u

static uint32_t s_gen_sent;

static void dwt_wait_cycles(uint32_t cyc)
{
    uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < cyc)
    {
    }
}

static bool peer_gen(uint32_t n, uint32_t us)
{
    uint32_t cyc_per_us = (SYS_TIME_FrequencyGet() / 1000000u) * 2u;   /* DWT: double the clock */
    uint32_t half;
    uint32_t i;

    if (!PTP_TRIG_DwtOk())
    {
        SYS_CONSOLE_PRINT("[PEER] the cycle counter (DWT) is not running - without it"
                          " there is no time base for the pulse width.\r\n");
        return false;
    }
    if (cyc_per_us == 0u)
    {
        return false;
    }
    if (!PIN_Claim(BOARD_TRIG_PIN_INDEX, PIN_OWNER_GPIO))
    {
        SYS_CONSOLE_PRINT("[PEER] PD10 is held by '%s' - `tbase pin off` first, then retry.\r\n",
                          PIN_OwnerName(PIN_OwnerGet(BOARD_TRIG_PIN_INDEX)));
        return false;
    }

    half = (us / 2u) * cyc_per_us;
    if (half == 0u) { half = cyc_per_us; }

    PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUTCLR = BOARD_TRIG_MASK;
    PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_DIRSET = BOARD_TRIG_MASK;
    dwt_wait_cycles(half);

    for (i = 0u; i < n; i++)
    {
        PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUTSET = BOARD_TRIG_MASK;
        dwt_wait_cycles(half);
        PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUTCLR = BOARD_TRIG_MASK;
        dwt_wait_cycles(half);
    }
    s_gen_sent = n;
    PIN_Release(BOARD_TRIG_PIN_INDEX, PIN_OWNER_GPIO);
    return true;
}

/* --------------------------------------------------------------------------- */
/* Console                                                                     */
/* --------------------------------------------------------------------------- */

/* Under `tbase`, not as a group of its own: MAX_CMD_GROUP in the generated
   sys_command.h is 8 and the project is at the ceiling - a ninth
   SYS_CMD_ADDGRP fails, and silently so from the caller's point of view.
   Same reasoning as in pps_capture.c and the trigger. */

static void print_phase(const char *tag, const PEER_PHASE *p, uint64_t period_tk)
{
    if (!p->valid)
    {
        SYS_CONSOLE_PRINT("[%s] no sample yet.\r\n", tag);
        return;
    }
    SYS_CONSOLE_PRINT("[%s] Phase: last %+ld ns   Median %+ld   MAD %ld ns\r\n",
                      tag, (long)p->last_ns, (long)p->median_ns, (long)p->mad_ns);
    SYS_CONSOLE_PRINT("[%s]   min %+ld  max %+ld ns   samples %lu of %lu"
                      "   age %lu ms%s\r\n",
                      tag, (long)p->min_ns, (long)p->max_ns,
                      (unsigned long)p->samples, (unsigned long)p->total,
                      (unsigned long)p->age_ms,
                      (p->age_ms > PEER_STALE_MS) ? "  << STALE" : "");
    if (period_tk != 0u && s_ticks_per_us != 0u)
    {
        unsigned long half_us = (unsigned long)((period_tk / 2u) / s_ticks_per_us);
        SYS_CONSOLE_PRINT("[%s]   measuring range +-%lu us (half the grid period)"
                          "   saturated: %s\r\n",
                          tag, half_us, p->saturated ? "YES - value unusable" : "no");
    }
}

bool PEER_CliTry(int argc, char **argv)
{
    bool is_peer = (argc >= 2 && !strcmp(argv[1], "peer"));
    bool is_sec = (argc >= 2 && !strcmp(argv[1], "sec"));
    PEER_PHASE p;

    if (!is_peer && !is_sec)
    {
        return false;
    }

    if (argc >= 3 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off")))
    {
        s_on = !strcmp(argv[2], "on");
        SYS_CONSOLE_PRINT("[PEER] capture: %s\r\n", s_on ? "on" : "off");
        return true;
    }
    if (argc >= 3 && !strcmp(argv[2], "reset"))
    {
        st_reset(&s_peer);
        st_reset(&s_sec);
        s_caps = 0u; s_no_grid = 0u; s_notready = 0u; s_wait_max = 0u;
        SYS_CONSOLE_PRINT("[PEER] statistics cleared (calibration kept)\r\n");
        return true;
    }
    /* `tbase peer count [reset]` - the event counter in hardware.  One half
       of the count check: exactly N pulses in, exactly N out. */
    if (argc >= 3 && !strcmp(argv[2], "count"))
    {
        if (argc >= 4 && !strcmp(argv[3], "reset"))
        {
            s_ev_base = peer_events_raw();
            SYS_CONSOLE_PRINT("[PEER] event counter zeroed (raw state %u)\r\n",
                              (unsigned)s_ev_base);
            return true;
        }
        SYS_CONSOLE_PRINT("[PEER] events since reference: %u   raw state %u   chain: %s\r\n",
                          (unsigned)peer_events_since_base(),
                          (unsigned)peer_events_raw(),
                          s_ev_ok ? "TC3 counting" : "TC3 NOT set up");
        SYS_CONSOLE_PRINT("[PEER] counter is 16 bit - a sample of more than 65535"
                          " events is not unambiguous.\r\n");
        return true;
    }

    /* `tbase peer mirror on|off` - diagnostic mode: mirror PD11's edge onto
       PD10 in the ISR, so the analyser can measure the edge->ISR-entry
       latency from outside.  It cannot be measured from inside: every
       timestamp the ISR takes already lies after the entry. */
    if (argc >= 4 && !strcmp(argv[2], "mirror"))
    {
        bool want = (strcmp(argv[3], "off") != 0);
        if (peer_mirror_set(want))
        {
            SYS_CONSOLE_PRINT("[PEER] mirror PD11 -> PD10: %s%s\r\n",
                              want ? "ON" : "off",
                              want ? "   (diagnostic mode - while it runs, THIS"
                                     " board's cross-measurement is not meaningful)"
                                   : "");
        }
        return true;
    }
    if (argc >= 3 && !strcmp(argv[2], "mirror"))
    {
        SYS_CONSOLE_PRINT("[PEER] mirror: %s   mirrored edges: %lu\r\n",
                          s_mirror ? "ON" : "off",
                          (unsigned long)s_mirror_edges);
        SYS_CONSOLE_PRINT("[PEER] `tbase peer mirror on` needs `tbase pin off` first."
                          "  Measured from outside: stimulus on the neighbour's channel,\r\n"
                          "[PEER] response on the own one - the difference is the latency"
                          " from edge to ISR entry.\r\n");
        return true;
    }

    /* `tbase peer gen <n> [us]` - the other half: send exactly n edges. */
    if (argc >= 4 && !strcmp(argv[2], "gen"))
    {
        unsigned long n = strtoul(argv[3], NULL, 0);
        unsigned long us = (argc >= 5) ? strtoul(argv[4], NULL, 0) : 100ul;

        if (n == 0ul || n > PEER_GEN_MAX)
        {
            SYS_CONSOLE_PRINT("[PEER] n must be 1..%u (the far side's counter is"
                              " 16 bit)\r\n", (unsigned)PEER_GEN_MAX);
            return true;
        }
        if (us < PEER_GEN_MIN_US)
        {
            SYS_CONSOLE_PRINT("[PEER] spacing at least %u us - shorter is not"
                              " carried by the write itself\r\n", (unsigned)PEER_GEN_MIN_US);
            return true;
        }
        SYS_CONSOLE_PRINT("[PEER] sending %lu pulses %lu us apart on PD10"
                          " (blocks %lu ms)\r\n", n, us, (n * us) / 1000ul);
        if (peer_gen((uint32_t)n, (uint32_t)us))
        {
            SYS_CONSOLE_PRINT("[PEER] sent: %lu rising edges\r\n",
                              (unsigned long)s_gen_sent);
        }
        return true;
    }

    if (argc >= 3 && !strcmp(argv[2], "cal"))
    {
        calibrate();
        SYS_CONSOLE_PRINT("[PEER] recalibrated: %s   delta32 = 0x%08lX\r\n",
                          s_cal_ok ? "ok"
                                   : "FAILED (no two samples close together)",
                          (unsigned long)s_delta32);
        SYS_CONSOLE_PRINT("[PEER]   %lu of %u samples kept, spread %lu ticks,"
                          " %lu discarded   read gap max %lu cycles%s\r\n",
                          (unsigned long)s_cal_n, (unsigned)PEER_CAL_N,
                          (unsigned long)s_cal_spread_tk, (unsigned long)s_cal_wild,
                          (unsigned long)s_cal_gap_cyc,
                          PTP_TRIG_DwtOk() ? " (subtracted)"
                                           : "  DWT OFF - gap NOT subtracted");
        return true;
    }

    if (is_sec)
    {
        uint64_t ref, period = 0u;
        (void)PTP_TRIG_GridRef(&ref, &period);
        SYS_CONSOLE_PRINT("[SEC] own grid against the own second"
                          " (1PPS, no wire involved)\r\n");
        SYS_CONSOLE_PRINT("[SEC] second edges so far: %lu\r\n",
                          (unsigned long)PPS_EdgeCount());
        st_eval(&s_sec, &p);
        print_phase("SEC", &p, period);
        if (!p.valid)
        {
            SYS_CONSOLE_PRINT("[SEC] It needs BOTH: an armed grid"
                              " (trigper) and edges on PC12 (tbase pps).\r\n");
        }
        SYS_CONSOLE_PRINT("[SEC] This number tells you WHICH board has moved -"
                          " the cross-measurement cannot.\r\n");
        return true;
    }

    /* tbase peer */
    {
        uint64_t ref, period = 0u;
        bool have_grid = PTP_TRIG_GridRef(&ref, &period);

        SYS_CONSOLE_PRINT("[PEER] input PD11 (EXT1 pin 6, EXTINT6)"
                          "   chain: %s   capture: %s\r\n",
                          s_hw_ok ? "up" : "NOT SET UP",
                          s_on ? "on" : "off");
        SYS_CONSOLE_PRINT("[PEER] events in TC3: %u (since reference)   %s\r\n",
                          (unsigned)peer_events_since_base(),
                          s_ev_ok ? "count check: `peer count reset`, neighbour `peer gen <n>`"
                                  : "TC3 NOT set up");
        SYS_CONSOLE_PRINT("[PEER] captures: %lu   discarded, no grid armed: %lu"
                          "   errata re-reads: %lu (max %lu loops)\r\n",
                          (unsigned long)s_caps, (unsigned long)s_no_grid,
                          (unsigned long)s_notready, (unsigned long)s_wait_max);
        SYS_CONSOLE_PRINT("[PEER] delta32 = 0x%08lX  (%s, %lu/%u samples, spread %lu"
                          " ticks, %lu wild, read gap %lu cycles subtracted)"
                          "   grid reference: %s\r\n",
                          (unsigned long)s_delta32,
                          s_cal_ok ? "calibrated" : "NOT calibrated",
                          (unsigned long)s_cal_n, (unsigned)PEER_CAL_N,
                          (unsigned long)s_cal_spread_tk, (unsigned long)s_cal_wild,
                          (unsigned long)s_cal_gap_cyc,
                          have_grid ? "present" : "MISSING (nothing armed)");
        st_eval(&s_peer, &p);
        print_phase("PEER", &p, period);

        if (s_caps == 0u)
        {
            SYS_CONSOLE_PRINT("[PEER] NO EDGE.  Cable the neighbour's EXT1 pin 5"
                              " to pin 6 here, ground connected, and the neighbour\r\n"
                              "[PEER] must be driving a grid (trigper).  Check without"
                              " firmware: python scripts/test_peer_wiring.py\r\n");
        }
        SYS_CONSOLE_PRINT("[PEER] Read both boards: (A-B)/2 is the true offset,"
                          " (A+B)/2 the path's propagation delay.\r\n");
    }
    return true;
}
