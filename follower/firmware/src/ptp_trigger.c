/*******************************************************************************
  Time-triggered actions - implementation

  See ptp_trigger.h.  Phase C of PTP_TIMEBASE_PLAN.md, software stage: the
  instant is computed on the shared timebase, the firing is a SYS_TIME one-shot
  callback.  Phase E replaces the firing with a hardware compare; nothing in the
  scheduling logic here has to change for that.
*******************************************************************************/

#include <stdlib.h>
#include <string.h>
#include "ptp_trigger.h"
#include "board_pins.h"
#include "pin_table.h"
#include "grid_div.h"
#include "someip.h"
#include "ptp_timebase.h"
#include "definitions.h"

/* --------------------------------------------------------------------------- */
/* state                                                                       */
/* --------------------------------------------------------------------------- */

typedef struct
{
    bool             used;
    bool             isr_ctx;
    uint16_t         id;
    PTP_TRIG_Handler fn;
    uintptr_t        ctx;
    uint32_t         last_seq;
    bool             seq_valid;
} action_t;

static action_t s_act[PTP_TRIG_ACTIONS];

static void trig_cb(uintptr_t context);      /* SYS_TIME callback, ISR context */

static volatile bool     s_armed;
static volatile uint16_t s_armed_id;
static volatile uint64_t s_target_ns;
static volatile uint64_t s_target_L;
static volatile uint64_t s_period_ns;
static volatile uint64_t s_phase_ns;

/* ------------------------------------------------- Pulling the phase together (slew)
 *
 * WHY THIS EXISTS, and the reason is a failed demo: after `ptp stop` the
 * boards were supposed to visibly drift apart.  They do not - the servo
 * has learned each board's rate and holds it, only the DIFFERENCE of the
 * learned rates drifts, measured at 165 ns/s = 0.165 ppm (E66).
 *
 * That leads to something fundamental: THE DRIFT CANNOT BE SEEN WITH THE
 * EYE.  For half a period of offset in 60 s you would need a half-period
 * of 0.165 ppm * 60 s ~ 10 us, i.e. 100 kHz of blinking.  There is no
 * blink rate for which the numbers work out.
 *
 * So the other way around: START BIG.  Without a usable timebase, each
 * board anchors its grid at its OWN start (s_phase_ns), so two boards
 * started one after another end up arbitrarily far apart.  When the
 * timebase comes back, the phase error is worked off in a BOUNDED way -
 * not jumped, but pulled, and that is exactly what shows in the blinking.
 *
 * The debt is stated in nanoseconds of master time and shrinks by at most
 * period/`s_slew_div` per grid point.  Sign: positive means "my grid runs
 * too late, I have to pull the next point forward". */
static volatile int64_t  s_slew_debt;     /* open phase error, 0 = done          */
/* THE RATE IS A FRACTION OF THE PERIOD, NOT AN ABSOLUTE TIME - and that is
 * the fix for a design mistake that made the demo unusable.
 *
 * The first build had 500 ns per grid point, fixed.  At a 100 us grid that
 * is 10,000 points/s, i.e. 5 ms/s - usable.  At a 250 ms grid it is 4
 * points/s, i.e. 2 us/s: the 54 ms of debt measured on the device would
 * have taken 27,000 seconds.  The LEDs stood still, and it looked like a
 * defect.
 *
 * As a FRACTION it scales by itself: the pay-down per second is
 * (period/div)/period = 1/div, i.e. independent of the period.  At 128 the
 * worst case (half a period) closes in 64 points - 16 s at 250 ms, 6.4 ms
 * at 100 us.  That is exactly the right order of magnitude in both cases,
 * and the demo therefore needs NO command any more. */
#define TRIG_SLEW_DIV_DEF   128u
static volatile uint32_t s_slew_div = TRIG_SLEW_DIV_DEF;
static uint32_t          s_slew_runs;     /* how often a pull-in started         */
static uint32_t          s_slew_done;     /* how often it finished               */
static uint64_t          s_slew_start_debt; /* debt at the last start           */
/* FIRST trigger point named by the master, 0 = choose it yourself.  Only
   applies to the first arm; after that trig_fire() carries the grid
   forward. */
static volatile uint64_t s_first_ns;
/* Was the start point named by the master taken - and if not, why?  Two fix
   attempts ran without this number, and both times it stayed open whether
   the fix does not take effect or the theory is wrong.  That exact
   distinction is the question, so it belongs in a counter, not a guess. */
static uint32_t s_start_taken;
static uint32_t s_start_past;      /* already lay in the past       */
static uint32_t s_start_offgrid;   /* did not sit on the grid       */
static uint32_t s_start_absent;    /* master sent none (0)          */
/* How often the grid was anchored at its OWN start instead of the absolute
   one.  This is a demo state, and in normal operation the number must
   stay 0 - hence counted, not just inferable from the value of `grid
   phase`. */
static uint32_t s_phase_local;

/* Set by the callback, consumed by PTP_TRIG_Tasks() for deferred handlers. */
static volatile bool     s_pending_defer;
static volatile uint64_t s_pending_sched_ns;
static volatile int32_t  s_pending_late;
static volatile uint16_t s_pending_id;

static SYS_TIME_HANDLE s_timer = SYS_TIME_HANDLE_INVALID;
static PTP_TRIG_MODE   s_mode = PTP_TRIG_MODE_STRICT;
static bool            s_stage2;   /* SYS_TIME callback hands over to TC1 */

static uint64_t s_ticks_per_us;
static uint32_t s_cnt_fired;
static uint32_t s_cnt_refused;
static uint32_t s_cnt_missed;

/* Edges produced while ARMING, i.e. outside the grid - plus the grid index
   of the most recent arm.  Both only for signing off the start picture;
   see the long rationale at the pre-write site. */
static uint32_t s_cnt_pre_edge;
static uint64_t s_pre_last_n;
/* Kept separate from the arm counter, so that counter's invariant ("MUST be
   0") keeps its meaning: this one counts the corrections AFTER a phase
   pull-in. */
static uint32_t s_cnt_pre_slew;

/* --- WHERE DOES THE SKIPPED SECOND COME FROM? (instrumentation 2026-08-19) ---
 * The catch-up loop below is measured to skip exactly 10,000 periods, i.e.
 * one second.  Whether the local clock, the conversion, or the deadline
 * was wrong can only be told apart by comparing with the PREVIOUS pass -
 * hence both sides are recorded.  Nothing is logged below SKIPLOG_MIN
 * periods, so a harmless one-period catch-up does not flood the buffer. */
#define SKIPLOG_N       8u
#define SKIPLOG_MIN     10u        /* from this many skipped periods on */
typedef struct
{
    uint64_t tick_prev;    /* local tick at the previous fire        */
    uint64_t tick;         /* local tick now                         */
    uint64_t ns_prev;      /* converted master time at the previous  */
    uint64_t ns;           /* converted master time now              */
    uint64_t next_before;  /* the deadline, BEFORE catching up       */
    uint32_t skipped;      /* how many periods were skipped          */
} skiplog_t;
static volatile skiplog_t s_skiplog[SKIPLOG_N];
static volatile uint32_t  s_skiplog_n;
static uint64_t s_fire_tick_prev;
static uint64_t s_fire_ns_prev;

static uint32_t s_cnt_rearm_lost;   /* periodic trigger gave up entirely */
static uint32_t s_cnt_stalled;      /* watchdog found an armed-but-dead chain */
static uint32_t s_cnt_ho_stop;      /* periodic trigger stopped: no time source */
static bool     s_ho_disarmed;      /* the last disarm was a holdover stop      */

/* Counter-sanity instrumentation.
 *
 * SYS_TIME_GetElapsedCount() cannot tell "x ticks after prev" from
 * "x + 65536 ticks after prev": the hardware counter is 16 bit (TOP is forced to
 * 0xFFFF by SYS_TIME_Initialize, overriding the 59999 the generated PLIB writes),
 * and the bookkeeping ISR only refreshes prev every HALF_PERIOD = 546 us.  The
 * dropouts on the pin are 546.9 us long, which is 32768 ticks - so the error is
 * real, but half the size the one-wrap ambiguity would predict, and guessing
 * which mechanism produces it is how one fixes the wrong thing.  The trigger ISR
 * reads the counter 10000 times a second at a 100 us period, so it is the
 * cheapest place to catch the misread in the act. */
static uint64_t s_ck_prev;
static uint32_t s_ck_bad;           /* reads whose delta is implausible      */
static int64_t  s_ck_min;           /* most negative delta error, ticks      */
static int64_t  s_ck_max;
static int64_t  s_ck_last;
static uint32_t s_ck_gap;           /* trigger did not run at all for a long time */
static int64_t  s_ck_gap_max;
static int32_t  s_late_last;
static int32_t  s_late_max;
static int32_t  s_late_min;
static uint64_t s_late_sum;
static uint32_t s_late_n;

/* --------------------------------------------------------------------------- */
/* hardware firing backend - phase E1                                          */
/*                                                                             */
/* Phase C fires from SYS_TIME_CallbackRegisterUS and measured -11.7 .. +13.8 us
 * of lateness.  Two causes, both removable: the delay is truncated to whole
 * microseconds, and SYS_TIME has to walk its timer list.
 *
 * E1 keeps SYS_TIME only to get within a millisecond, then hands the last hop to
 * a dedicated TC compare.  Deliberately NOT a free-running TC1 whose count is
 * mapped onto TC0's: the two counters share the clock but not the phase, and
 * determining that offset needs two reads that are not atomic - the same
 * simultaneity problem as everywhere else in this project.  Instead TC1 is
 * RETRIGGERED from zero with interrupts off, so the only error is the fixed,
 * deterministic path between reading SYS_TIME and issuing the retrigger.  That
 * fixed part is calibrated away by TB_HW_LATENCY_TICKS; what remains is jitter
 * of a few instructions plus interrupt latency.
 *
 * TC1 shares GCLK channel 9 with TC0, which is correct rather than a conflict:
 * the channel supplies the 60 MHz clock, and that is exactly the clock wanted.
 * What TC0 occupies is its two compare channels, not the GCLK channel.
 */

/* Offsets from the DFP header, not from memory: INTENCLR 0x08, INTENSET 0x09,
   INTFLAG 0x0A (tc.h TC_INTEN*_REG_OFST).  Getting INTENSET and INTFLAG one byte
   too high cost a debugging round: the "enable" write landed in INTFLAG and
   cleared flags instead, so TC1 counted and matched but never interrupted. */
/* In 16-bit mode CC[] is an array starting at 0x1C with a 2-byte stride
   (DFP: TC_COUNT16_CC_REG_OFST = 0x1C, CC_NUM = 2) - so CC1 sits at 0x1E.
   Taken from the header, not guessed: this peripheral's offsets have
   already been misremembered twice in this project (TC1, EIC). */

#define TC_MC0              TC_INTFLAG_MC0_Msk   /* DFP, Bit 4 */
#define TC_MC1              TC_INTFLAG_MC1_Msk   /* DFP, Bit 5 */
#define TC_CMD_RETRIGGER    TC_CTRLBSET_CMD_RETRIGGER
#define TC_CMD_STOP         TC_CTRLBSET_CMD_STOP
#define TC_ONESHOT          TC_CTRLBSET_ONESHOT_Msk

/* Width of the SYS_TIME hardware counter (16 bit) and the CPU:GCLK1 clock
   ratio.  The counter runs at 60 MHz, the core at 120 - measured 2.0000015,
   so the whole number here is correct and the residual error is 0.4 ticks
   per ISR period. */
/* ------------------------------------------------------------------ stage 1
 * REGISTER ACCESS SIGN-OFF - same rationale as in `pps_capture.c`: the TC1
 * block used to be hand-coded addresses here until 2026-08-20, and
 * INTENSET/INTFLAG one byte too high has already cost one debugging round
 * (the "enable" write landed in INTFLAG and cleared flags instead, TC1
 * counted and compared but never interrupted).  Reference values from
 * component/tc.h.
 */
#define TRIG_ADDR_OF(x)  ((uintptr_t)&(x))

_Static_assert(TRIG_ADDR_OF(TC1_REGS->COUNT16.TC_CTRLA)    == 0x40003C00u, "TC1_CTRLA");
_Static_assert(TRIG_ADDR_OF(TC1_REGS->COUNT16.TC_CTRLBCLR) == 0x40003C04u, "TC1_CTRLBCLR");
_Static_assert(TRIG_ADDR_OF(TC1_REGS->COUNT16.TC_CTRLBSET) == 0x40003C05u, "TC1_CTRLBSET");
_Static_assert(TRIG_ADDR_OF(TC1_REGS->COUNT16.TC_EVCTRL)   == 0x40003C06u, "TC1_EVCTRL");
_Static_assert(TRIG_ADDR_OF(TC1_REGS->COUNT16.TC_INTENCLR) == 0x40003C08u, "TC1_INTENCLR");
_Static_assert(TRIG_ADDR_OF(TC1_REGS->COUNT16.TC_INTENSET) == 0x40003C09u, "TC1_INTENSET");
_Static_assert(TRIG_ADDR_OF(TC1_REGS->COUNT16.TC_INTFLAG)  == 0x40003C0Au, "TC1_INTFLAG");
_Static_assert(TRIG_ADDR_OF(TC1_REGS->COUNT16.TC_SYNCBUSY) == 0x40003C10u, "TC1_SYNCBUSY");
_Static_assert(TRIG_ADDR_OF(TC1_REGS->COUNT16.TC_CC[0])    == 0x40003C1Cu, "TC1_CC0");
_Static_assert(TRIG_ADDR_OF(TC1_REGS->COUNT16.TC_CC[1])    == 0x40003C1Eu, "TC1_CC1");
_Static_assert(sizeof(TC1_REGS->COUNT16.TC_INTENSET) == 1u, "TC1_INTENSET is 8 bit");
_Static_assert(sizeof(TC1_REGS->COUNT16.TC_INTFLAG)  == 1u, "TC1_INTFLAG is 8 bit");
_Static_assert(sizeof(TC1_REGS->COUNT16.TC_CC[0])    == 2u, "TC1_CC0 is 16 bit");
_Static_assert(sizeof(TC1_REGS->COUNT16.TC_CTRLA)    == 4u, "TC1_CTRLA is 32 bit");

_Static_assert(TC_MC0 == 0x10u,               "MC0 = Bit 4");
_Static_assert(TC_MC1 == 0x20u,               "MC1 = Bit 5");
_Static_assert(TC_CMD_RETRIGGER == (1u << 5), "CTRLBSET.CMD = RETRIGGER");
_Static_assert(TC_CMD_STOP      == (2u << 5), "CTRLBSET.CMD = STOP");
_Static_assert(TC_ONESHOT       == (1u << 2), "CTRLBSET.ONESHOT");
_Static_assert(MCLK_APBAMASK_TC1_Msk == (1u << 15), "APBAMASK.TC1");
_Static_assert(TC1_GCLK_ID == 9u, "TC1 GCLK channel (shared with TC0)");
/* --------------------------------------------------------------------------- */

#define SYST_WRAP_TK        65536u
#define SYST_DWT_DIV        2u

/* Reference period for a one-shot in the pulse model, when no period is
   armed.  100 us is the grid this project targets. */
#define PULSE_DEFAULT_US    100u
/* SYNCBUSY.CC1 comes from the DFP header, NOT hand-written: `TC_SYNCBUSY_CC1`
   is a macro with an argument there (`TC_SYNCBUSY_CC1(value)`), and a
   home-grown version shadows it with a warning that is easy to miss - the
   same class of mistake as the guessed offsets above, only in reverse.
   What is needed is the mask. */
#define TC1_SYNCBUSY_CC1_M  TC_SYNCBUSY_CC1_Msk

#define TRIG_OUTSET         (&PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUTSET)
#define TRIG_OUTCLR         (&PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUTCLR)
#define TRIG_OUTTGL         (&PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUTTGL)

#define LED_BLINK_MS        250u

/* Handing over to TC1 needs the remaining delay to fit its 16-bit counter.
 * 60000 ticks = 1 ms, comfortably below the 65536-tick wrap. */
#define TC1_MAX_ARM_TICKS   60000u

/* Fixed cost of the arming path, subtracted from the compare value.  Measured
 * against the Saleae, not guessed - see test_results.md.  Starts at 0 so the
 * first measurement shows the raw offset. */
#define TB_HW_LATENCY_TICKS 0u

static bool     s_hw_mode;            /* true = fire from TC1                   */
static bool     s_pin_armed;          /* drive PD10 on every fire               */
static volatile bool s_hw_pending;    /* TC1 is armed for the final hop         */

/* The pin level is DERIVED from the absolute grid index, not accumulated by
 * toggling.  This is the second half of a fix whose first half was incomplete.
 *
 * Toggling makes the level carry the parity of this board's fire count, and a
 * dropout eats an odd number of fires: measured at a 100 us period, every long
 * dropout was 647 us of hold, i.e. 546.9 us on top of one period - and
 * 32768 ticks / 60 MHz is 546.1 us, so it is exactly one half-wrap of the 16-bit
 * SYS_TIME counter being misread (test_results.md E3.2).  Five swallowed fires
 * is odd, so the level flipped against the grid and the two boards drifted in
 * and out of phase: in phase at t=1 ms, inverse from 0.5 s, in phase again at
 * 4 s.  Pre-setting the level only at arm time, as the first fix did, cannot
 * survive that.
 *
 * So the ISR writes OUTSET or OUTCLR chosen by the grid parity instead of
 * OUTTGL.  s_pin_op is picked in the re-arm path, which means the ISR is still a
 * SINGLE STORE with no branch and no added latency on the edge - that matters,
 * because the edge is the measurement.  A dropout now shows up as a longer hold
 * and heals on the very next fire, without the spurious short pulse that a
 * correction *after* firing would produce.
 *
 * One-shots keep the old behaviour by pointing s_pin_op at OUTTGL: there is no
 * grid to derive a level from. */
static volatile uint32_t * volatile s_pin_op;
static volatile uint64_t s_grid_n;    /* index of the currently armed instant   */

#define TRIG_FIRE()         (*s_pin_op = BOARD_TRIG_MASK)

/* Level held BEFORE instant n is low for odd n and high for even n (see
   trig_arm_grid), so firing at n has to produce the opposite. */
static inline void pin_arm_for(uint64_t n)
{
    s_pin_op = ((n & 1u) != 0u) ? TRIG_OUTSET : TRIG_OUTCLR;
}

/* Pre-write the level BEFORE grid point `n`, in HARDWARE mode.  Returns
 * true if that actually produced an edge (i.e. the level was wrong).
 *
 * ONE function for two call sites - arming and re-deriving after a
 * pull-in - because a second copy of the parity computation is exactly the
 * path along which the two would drift apart.  The full rationale for why
 * hardware mode needs a pre-write at all sits at the call site in
 * trig_arm_grid(): the PORT uses `EVACT = TGL`, so the pin only TOGGLES,
 * and which of the two levels a board ends up with is decided purely by
 * the moment it happened to arm. */
static inline bool pin_preset_for(uint64_t n)
{
    /* At grid point n the pin goes HIGH for odd n (see pin_arm_for), so the
       level BEFORE it has to be the opposite. */
    const bool want_high = ((n & 1u) == 0u);
    const bool is_high =
        (PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUT & BOARD_TRIG_MASK) != 0u;

    if (want_high)
    {
        *TRIG_OUTSET = BOARD_TRIG_MASK;
    }
    else
    {
        *TRIG_OUTCLR = BOARD_TRIG_MASK;
    }
    return (want_high != is_high);
}

static void hw_pin_init(void)
{
    PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_DIRSET = BOARD_TRIG_MASK;
    PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUTCLR = BOARD_TRIG_MASK;
}

/* --------------------------------------------------------------------------- */
/* the pin, driven by hardware                                                 */
/*                                                                             */
/* Measured on 2026-08-12: TC1 hits its compare instant exactly, but the PIN was
 * driven by software in TC1_Handler, so every bit of interrupt-entry jitter
 * landed on the edge.  Both boards reported the SAME mean lateness (144 ticks)
 * with a spread of 2.5 to 2.7 us - the fixed part cancels between boards, the
 * spread does not, and that spread was the whole remaining GPIO scatter once the
 * 1PPS timebase had removed the model's contribution (test_results.md E7).
 *
 * So the pin now toggles without the CPU: TC1 MC0 raises an event, EVSYS carries
 * it asynchronously, and the PORT's event input toggles PD10.  What is left on the
 * edge is the TC1 compare and the event path - tens of nanoseconds instead of
 * microseconds.  The ISR still runs, but only to count and to re-arm; if it is
 * late now, the EDGE is not.
 *
 * IDs from the DFP header, not from memory: EVENT_ID_GEN_TC1_MC_0 = 77,
 * EVENT_ID_USER_PORT_EV_0 = 1, and the user register takes channel+1 because 0
 * means "no channel". */
/* THIS FIRMWARE'S EVSYS CHANNEL ASSIGNMENT - a list, because a channel used
 * twice looks exactly like a broken register:
 *
 *   Channel 0   TC1_MC_0      -> PORT_EV_0   trigger, rising edge         (here)
 *   Channel 1   EIC_EXTINT_12 -> TC2         1PPS capture (pps_capture.c, PPS_EVSYS_CH)
 *   Channel 2   TC1_MC_1      -> PORT_EV_1   trigger, falling edge        (here)
 *   Channel 3   EIC_EXTINT_6  -> TC4         neighbour edge (peer_capture.c, PEER_EVSYS_CH)
 *
 * PEER_CAPTURE_PLAN.md named channel 2 - that was wrong, the falling edge
 * has owned it since the pulse model.  That is exactly why this list is
 * here: the next channel is not "whatever's next in my head", it is the
 * next one in this table.
 *
 * The conflict cost half an evening and was, from the outside,
 * indistinguishable from a hardware fault: TC1 issued MC1 (EVCTRL =
 * 0x3000), EVSYS_USER[2] pointed at the channel, PORT_EVCTRL was correct
 * (0xCAAA), the MC1 interrupts arrived 12,034 times a second - and the pin
 * did not move.  Only reading back EVSYS_CHANNEL[1] showed 0x051E: EVGEN 30
 * (EIC_EXTINT_12) with a resynchronised path, i.e. the 1PPS chain from
 * E11.  The channel was carrying the one-second pulse, not the compare.
 *
 * LESSON: for a chain of several links, READ BACK every link, including
 * the ones you wrote yourself - the write only proves it happened, not
 * that it still holds. */
#define EVSYS_CH        0u
#define EVSYS_CH_FALL   2u

/* PULSE MODEL: two compare channels instead of toggling.
 *
 * The toggle path (EVACT = TGL) has a property that made E24 expensive:
 * ONE extra or missing edge inverts everything after it, permanently.  In
 * the pulse model, CC0 produces the rising edge (EVACT = SET) and CC1 the
 * falling one (EVACT = CLR), both from the SAME retrigger - a lost period
 * then costs one pulse, and the level afterwards is defined again.
 *
 * It is also the shape the endpoint protocol requires (`PulseTime` and
 * `IdleTime` instead of "toggling", some_ip.md 8.4) and the precondition
 * for the software PWM.
 *
 * All numbers from the DFP, not from memory: EVENT_ID_GEN_TC1_MC_1 = 78,
 * EVENT_ID_USER_PORT_EV_1 = 2, EVACT SET = 1 / CLR = 2, PID1 at bit 8,
 * EVACT1 at 13, PORTEI1 at 15 (component/port.h).
 *
 * The switch stays flippable at runtime (`tbase pulse on|off`), so both
 * paths are comparable on ONE flash - the same pattern as `tbase pps` and
 * `tbase hw`.  The toggle path only gets removed once the new one has been
 * measured. */
static bool     s_pulse_mode;          /* off: toggle as before                  */
static uint32_t s_pulse_pct = 50u;     /* duty cycle in percent                  */
static uint32_t s_pulse_clamped;       /* high time had to be shortened          */
/* Intermediate state of the pulse model: MC0 has fired, MC1 (the falling
   edge) is still pending, and re-arming only happens AFTER that.  It counts
   as armed for the watchdog - without that it would step in during exactly
   this window, count a stall that is not one, and arm against the MC1
   half. */
static volatile bool     s_pulse_await_fall;
static volatile uint64_t s_pulse_await_since;
static uint32_t          s_pulse_await_lost;   /* MC1 did not come - watchdog helped */
/* Instrument before fixing: without these two numbers "the pulse is not
   coming" cannot be told apart from "MC1 never fires". */
static uint32_t          s_mc0_n;
static uint32_t          s_mc1_n;
/* How often the one-shot stop in the pulse model actually triggered.
   Counted, not assumed: a special case nobody sees is not one - and this
   number is the counter-check for E45 (there, TC1 kept running after every
   `trig`). */
static uint32_t          s_shot_stop;
/* Repaired SYS_TIME overflows and the last shortfall - see
   PTP_TRIG_FixElapsed().  Counted, because a silent repair is the worst
   kind of repair: it takes away the defect's visibility without proving
   it. */
static uint32_t          s_free_stop;      /* TC1 ran with no compare pending    */
static uint32_t          s_oneshot_skip;   /* pulse did not fit before the overflow */
static uint32_t          s_syst_repair;
static uint32_t          s_syst_repair_tk;
static uint32_t          s_dwt_isr_prev;
static bool              s_dwt_isr_have;
static uint64_t s_pulse_ticks;         /* High-Zeit in SYS_TIME-Ticks            */
static uint64_t s_period_ticks;        /* Gitterperiode in Ticks                 */

static void hw_pin_event_enable(bool on);
static void hw_pin_action_set(bool pulse);
static void pulse_recalc(void);
static void trig_rearm(uint64_t now);

/* End of the limited pulse train, ABSOLUTE in grandmaster ns.  UINT64_MAX
   means unlimited and is the resting state - so the extension costs
   exactly one comparison per re-arm in normal operation, and no
   behaviour. */
static uint64_t s_end_ns = UINT64_MAX;
static uint32_t s_cnt_burst_done;

static void hw_evsys_pin_init(void)
{
    MCLK_REGS->MCLK_APBBMASK |= MCLK_APBBMASK_EVSYS_Msk;

    /* Asynchronous path: no GCLK needed and no resynchronisation delay - the
       whole point is to keep the edge as close to the compare as possible. */
    EVSYS_REGS->CHANNEL[EVSYS_CH].EVSYS_CHANNEL =
        EVSYS_CHANNEL_EVGEN(EVENT_ID_GEN_TC1_MC_0) | EVSYS_CHANNEL_PATH_ASYNCHRONOUS;
    EVSYS_REGS->EVSYS_USER[EVENT_ID_USER_PORT_EV_0] = EVSYS_CH + 1u;

    /* Second channel for the falling edge.  It is always set up, but only
       enabled in pulse mode (PORTEI1) - an event with no consumer costs
       nothing. */
    EVSYS_REGS->CHANNEL[EVSYS_CH_FALL].EVSYS_CHANNEL =
        EVSYS_CHANNEL_EVGEN(EVENT_ID_GEN_TC1_MC_1) | EVSYS_CHANNEL_PATH_ASYNCHRONOUS;
    EVSYS_REGS->EVSYS_USER[EVENT_ID_USER_PORT_EV_1] = EVSYS_CH_FALL + 1u;

    /* PID is the pin within the group, so 10 for PD10 on group 3.  The
       ACTIONS are set by hw_pin_action_set(), because they depend on pulse
       mode. */
    PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_EVCTRL =
        PORT_EVCTRL_PID0(BOARD_TRIG_PIN) | PORT_EVCTRL_PID1(BOARD_TRIG_PIN);
    hw_pin_action_set(false);
}

/* Sets the action of the two PORT slots: either toggle (old, one slot) or
   SET/CLR (pulse model, two slots).  The enable bits stay untouched - those
   are set by hw_pin_event_enable(). */
static void hw_pin_action_set(bool pulse)
{
    uint32_t v = PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_EVCTRL;

    v &= ~(PORT_EVCTRL_EVACT0_Msk | PORT_EVCTRL_EVACT1_Msk
           | PORT_EVCTRL_PID0_Msk | PORT_EVCTRL_PID1_Msk);
    v |= PORT_EVCTRL_PID0(BOARD_TRIG_PIN) | PORT_EVCTRL_PID1(BOARD_TRIG_PIN);
    if (!pulse)
    {
        v |= PORT_EVCTRL_EVACT0_TGL;
    }
    else
    {
        v |= PORT_EVCTRL_EVACT0(PORT_EVCTRL_EVACT0_SET_Val)
           | PORT_EVCTRL_EVACT1(PORT_EVCTRL_EVACT0_CLR_Val);
    }
    PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_EVCTRL = v;
}

/* PORTEI0 is the gate: with it clear the event arrives and does nothing, so
   "tbase pin off" still works and costs no edge. */
static void hw_pin_event_enable(bool on)
{
    uint32_t v = PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_EVCTRL;
    if (on)
    {
        v |= PORT_EVCTRL_PORTEI0_Msk;
        /* The second slot only in pulse mode - in toggle mode an extra CLR
           edge would be exactly the fault the model is meant to avoid. */
        if (s_pulse_mode) { v |= PORT_EVCTRL_PORTEI1_Msk; }
        else              { v &= ~PORT_EVCTRL_PORTEI1_Msk; }
    }
    else
    {
        v &= ~(PORT_EVCTRL_PORTEI0_Msk | PORT_EVCTRL_PORTEI1_Msk);
    }
    PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_EVCTRL = v;
}

/* --------------------------------------------------------------------------- */
/* identification LEDs                                                         */
/* --------------------------------------------------------------------------- */

/* Since stage 3c the LEDs are TWO ROWS OF THE PIN TABLE, no longer their
 * own pair of register macros here.  The reason is not tidiness: as soon
 * as the endpoint protocol offers the same pins as index 2 and 6, there
 * would be two places that know "active low" - and the first one that
 * forgets it makes a conformant tool display the opposite.  Active low now
 * lives in the table exactly once.
 *
 * The indices are `lan866x-ledblink`'s defaults; the mapping "LED1 =
 * index 2, LED2 = index 6" is therefore that external tool's own. */
#define LED_PIN_INDEX_1     2u
#define LED_PIN_INDEX_2     6u

static const PIN_ROW *s_led[BOARD_LED_COUNT];

static uint8_t  s_led_blink;          /* bit per LED */
static uint64_t s_led_next_L;
static bool     s_led_phase;

static void led_set(unsigned i, bool on)
{
    /* Inversion is the table's job - what is passed here is LOGICALLY on
       or off. */
    PIN_Set(s_led[i], on);
}

static void led_init(void)
{
    /* Direction and resting level of ALL table rows, including PD10.  The
       duplicated effort with hw_pin_init() is intentional: one site
       configures from the table, the other belongs to the scheduling path
       and has to be able to run without it. */
    PIN_Initialize();
    s_led[0] = PIN_Find(LED_PIN_INDEX_1);
    s_led[1] = PIN_Find(LED_PIN_INDEX_2);
}

/* ------------------------------------------------------------------ action 4
 * TOGGLE LED1 ON THE GRID - synchronisation made visible to the eye.
 *
 * The point is not remote control, it is a demo without measurement gear:
 * `trigper 4 500ms` from the bridge, and every follower toggles its LED at
 * the SAME absolute grid point.  Two boards blink in lockstep; with
 * `trigper 4 500ms d` and `ptp stop` they visibly drift apart over
 * minutes.  Until now, exactly that needed a logic analyser.
 *
 * ISR CONTEXT, even though it would not matter for an LED: the body is a
 * single PORT write (`PIN_Set` = OUTSET or OUTCLR), and that way the LED
 * edge dates the grid point instead of the main-loop pass.  Anyone with a
 * scope on the LED then measures the grid, not the main loop - that costs
 * nothing and avoids a wrong conclusion.
 *
 * WHY THE CLAIM IS NOT TAKEN HERE: `PIN_Claim()` writes a shared table that
 * the CLI also writes.  From an ISR that would be a race on `s_owner[]`,
 * and from an ISR a failure cannot be reported either.  So only the WISH
 * is set; PTP_TRIG_Tasks() picks up the claim in task context, and
 * trig_stop_clean() gives it back.
 *
 * As long as the claim is missing, NOTHING is written - but `s_gled_busy`
 * counts it, and the number shows in `tbase trig`.  An LED that stays
 * silent with nothing incrementing anywhere would be exactly the kind of
 * silent failure this project has paid dearly for. */
#define TRIG_ACTION_LED     4u

static volatile bool     s_gled_want;    /* a schedule for action 4 is running  */
static volatile bool     s_gled_phase;
static volatile uint32_t s_gled_hits;
static bool              s_gled_own;     /* claim on LED1 held                  */
static uint32_t          s_gled_busy;    /* LED1 was held by someone else       */
/* Can the timebase be shown to be the master's?  Fetched ONCE per main-loop
   pass and handed to the ISR as a plain bit: PTP_TB_QualityGet() computes
   an age (64-bit division), and that does not belong in an interrupt.  The
   value is therefore at most one pass old, i.e. microseconds. */
static volatile bool     s_gled_synced;
static uint32_t          s_gled_free;    /* fires with no grid reference         */
static volatile uint32_t s_gled_tog;     /* toggles SINCE the last stop           */
static volatile uint64_t s_gled_last_ns; /* grid point of the last fire           */

static void led_grid_action(uint64_t scheduled_ns, int32_t late_ticks, uintptr_t ctx)
{
    (void)late_ticks; (void)ctx;

    s_gled_hits++;
    s_gled_want = true;

    /* THE LEVEL COMES FROM GRID PARITY, AS LONG AS THERE IS A GRID
     * REFERENCE - and that is the lesson from E24, applied here a second
     * time.
     *
     * A `phase = !phase` binds the level to the COUNT of this board's own
     * fires: if a board loses one grid point, it blinks permanently OUT OF
     * PHASE with its neighbour from then on, and the fault gets hunted in
     * time synchronisation, where there is none.  Computed from
     * `scheduled_ns / period`, every board gets the same index and
     * therefore the same level; a lost grid point then costs one skipped
     * toggle and heals itself.
     *
     * AND WHEN THE TIMEBASE CANNOT BE SHOWN TO BE THE MASTER'S, IT IS
     * COUNTED RATHER THAN COMPUTED.  That is not a stopgap for a demo, it
     * is the more honest statement: in `tbase mode free` the trigger
     * deliberately keeps firing, but on an EXTRAPOLATED time - the grid
     * points are locally consistent, but that checks NOTHING about their
     * absolute position relative to other boards.  A level derived from
     * that number would claim a phase agreement nobody has verified.  A
     * local toggle only claims what is true: "I switch on every one of my
     * own fires".
     *
     * The visible consequence is the demo: without a master, the phase
     * depends on WHEN the board was started - start two boards staggered
     * and they blink out of phase.  When the master comes back, both
     * compute from the grid again and lock together at the next toggle.
     *
     * Without a running period (one-shot) only the toggle remains as well -
     * there is no index there, and a one-shot cannot be out of phase. */
    if (s_gled_synced && s_period_ns != 0u)
    {
        /* FROM THE INDEX, not from `scheduled_ns / period`.  The quotient
           only equals the index as long as the phase is 0; if the board
           pulls in its own phase, the instants shift and the quotient
           would be wrong - the level would then jump in the middle of
           pulling together. */
        s_gled_phase = (PTP_TRIG_GridIndex() & 1u) != 0u;
    }
    else
    {
        s_gled_phase = !s_gled_phase;
        s_gled_free++;
        s_gled_tog++;
    }
    s_gled_last_ns = scheduled_ns;
    if (s_gled_own)
    {
        led_set(0u, s_gled_phase);
    }
}

/* Follow up on the LED1 claim - task context, every pass.
 *
 * If there is no claim and the LED is held by someone else (typically:
 * `tbase led blink 1` is still running), NOTHING is written and
 * `s_gled_busy` counts it.  Self-healing: as soon as the other user
 * releases it, the next pass picks it up.
 *
 * Counted only ONCE per episode rather than on every pass - a counter that
 * races ahead at a 120 MHz main loop says nothing about the number of
 * conflicts, only about the runtime. */
static void gled_claim_service(void)
{
    static bool s_gled_busy_seen;

    /* Fetch the quality reading ONCE per pass - rationale at s_gled_synced.
       Before the early exit, because the ISR needs the bit even when the
       claim is already held. */
    s_gled_synced = PTP_TB_IsUsable();

    /* s_led[] is only filled by led_init().  Without this check, the
       action before initialisation would be a null-pointer access -
       PIN_Claim() does check for NULL, but only AFTER the dereference
       here. */
    if (!s_gled_want || s_gled_own || s_led[0] == NULL)
    {
        return;
    }
    if (PIN_Claim(s_led[0]->index, PIN_OWNER_GRIDLED))
    {
        s_gled_own = true;
        s_gled_busy_seen = false;
        /* Set the now-valid level right away, so the LED does not sit in
           the old state until the next grid point. */
        led_set(0u, s_gled_phase);
    }
    else if (!s_gled_busy_seen)
    {
        s_gled_busy_seen = true;
        s_gled_busy++;
    }
}

static void led_service(void)
{
    uint64_t now;
    unsigned i;

    if (s_led_blink == 0u)
    {
        return;
    }
    now = SYS_TIME_Counter64Get();
    if (now < s_led_next_L)
    {
        return;
    }
    s_led_next_L = now + (uint64_t)LED_BLINK_MS * 1000u * s_ticks_per_us;
    s_led_phase = !s_led_phase;
    for (i = 0u; i < BOARD_LED_COUNT; i++)
    {
        if ((s_led_blink & (1u << i)) != 0u)
        {
            led_set(i, s_led_phase);
        }
    }
}

static void hw_tc1_init(void)
{
    /* TC1's APB clock is off out of the box: MCLK_APBAMASK is 0x77ff, and bit 15
     * (TC1) is clear while bit 14 (TC0, used by SYS_TIME) is set. */
    MCLK_REGS->MCLK_APBAMASK |= MCLK_APBAMASK_TC1_Msk;

    TC1_REGS->COUNT16.TC_CTRLA = 0u;                                   /* disable before config  */
    while ((TC1_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_ENABLE_Msk) != 0u) { }           /* ENABLE sync            */
    /* 16-bit mode, prescaler 1, no waveform output - the pin is driven by the
     * ISR, not by the compare unit.  That is E1; E2 would use WO instead. */
    TC1_REGS->COUNT16.TC_CTRLA = 0u;
    /* Compare match raises an EVENT as well as the interrupt.  The event is what
       toggles the pin now; the interrupt only counts and re-arms. */
    /* Enable BOTH compare events: MC0 sets the pin, MC1 clears it in the
       pulse model.  An event with no enabled PORT slot costs nothing, so
       both stay on permanently and the mode decides only at the PORT. */
    TC1_REGS->COUNT16.TC_EVCTRL = TC_EVCTRL_MCEO0_Msk | TC_EVCTRL_MCEO1_Msk;
    TC1_REGS->COUNT16.TC_CC[0] = 0xFFFFu;
    TC1_REGS->COUNT16.TC_INTENCLR = 0xFFu;
    TC1_REGS->COUNT16.TC_INTFLAG = 0xFFu;
    TC1_REGS->COUNT16.TC_CTRLA = TC_CTRLA_ENABLE_Msk;                                 /* ENABLE                 */
    while ((TC1_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_ENABLE_Msk) != 0u) { }

    /* PRIORITIES, AND THE ORDER IS MEASURED, NOT GUESSED.
     *
     * Before: TC1 sat at 0 here ("above the stack's ISRs"), EXTINT12 got no
     * assignment at all and kept the reset value - which is also 0 -, and
     * **TC0, the SYS_TIME timebase itself, sat at 7**, the lowest, because
     * the generated `plib_nvic.c` puts it there.  So the clock ran below
     * everything that depends on it.
     *
     * Measured 2026-08-17 (E39): four events on two boards where the local
     * tick of a 1PPS edge was **1.0710 ms +- 0.6 us** too small - a fixed
     * amount, 98 % of a 16-bit wrap (1.0923 ms).  The edge spacing was
     * short by the same amount in the same second, while the 1PPS on the
     * wire is stable to 160 ns.  So the clock jumped, not the signal.
     *
     * If TC0's overflow handler is held off longer than one wrap period,
     * `SYS_TIME_GetElapsedCount()` loses one overflow and computes the
     * 16-bit difference wrong by 65536 ticks - and it gets held off by
     * exactly the two ISRs that used to outrank it here.
     *
     * New ranking, by urgency and brevity:
     *   0  EXTINT12 - the timestamp.  Shortest ISR, must not wait on
     *                 anything.
     *   1  TC0      - time itself.  Must not be starved by anyone.
     *   2  TC1      - the follow-up.  Has ~77 us of slack in a 100 us
     *                 period, and the pin hangs off hardware (EVSYS), not
     *                 off this ISR - so being preempted costs no edge.
     *   7  stack (SERCOM, GMAC, DMAC) - unchanged.
     *
     * THIS IS A THEORY WITH A TEST, not a fix: if `over 100 us`
     * in `tbase pps` sits at 0 after this change, the mechanism is proven;
     * if the events persist, it is disproven and the 21 us difference to
     * the wrap was the warning sign. */
    NVIC_SetPriority(TC0_IRQn, 1);
    NVIC_SetPriority(TC1_IRQn, 2);
    NVIC_EnableIRQ(TC1_IRQn);
}

/* Final hop: fire exactly remaining_ticks from now.  Interrupts off so the path
 * from reading the counter to the retrigger is deterministic. */
/* AND THE SECOND CLOCK: DWT->CYCCNT, the debug core's cycle counter.
 *
 * Introduced by the user (FIRMWARE_SELF_DEBUGGING.md §8).  It costs two
 * register reads, runs at the CPU clock (~8.3 ns resolution) and is
 * completely independent of TC0 and every peripheral - so it can measure
 * what SYS_TIME cannot say about itself.
 *
 * Two uses here:
 *   1. the duration of the PRIMASK window below, in cycles instead of via
 *      the analyser,
 *   2. as a COUNTER-CLOCK in the 1PPS path (pps_capture.c): if SYS_TIME
 *      loses an overflow, it reports a one-second interval short by 65536
 *      ticks, while the DWT keeps counting its ~120 million cycles
 *      unperturbed.  That way the loss is MEASURED, not inferred.
 *
 * The caveat from the document applies and does not get in the way here:
 * CYCCNT hangs off the same PLL as GCLK1, so an ABSOLUTE frequency cannot
 * be obtained from it - only the divider ratio.  But what is being asked
 * is not an absolute value, it is whether one of the two clocks LOSES
 * STEPS, and for that a fixed ratio is exactly the right reference.
 *
 * NOCYCCNT is checked and reported: a counter that is not implemented
 * reads 0, and a silent zero has already cost this project one evening. */
static bool s_dwt_ok;

void PTP_TRIG_DwtInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0u)
    {
        s_dwt_ok = false;            /* counter not implemented */
        return;
    }
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    /* Proof that it is REALLY running - not just that it should be there. */
    {
        uint32_t a = DWT->CYCCNT;
        uint32_t b = DWT->CYCCNT;
        s_dwt_ok = (b != a);
    }
}

bool PTP_TRIG_DwtOk(void)
{
    return s_dwt_ok;
}

/* RE-SET, BECAUSE THE DEBUGGER OWNS THE REGISTERS.
 *
 * Measured 2026-08-17: after `PTP_TRIG_DwtInit()` CYCCNT was running (the
 * init check passed), and ~116 ms later `DEMCR = 0x00000000` - TRCENA
 * cleared, CYCCNTENA gone, the counter frozen at 13,889,390.  No code in
 * this project writes DEMCR other than this function; 116 ms after start
 * is the moment pyOCD disconnects after the `reset` and cleans up the
 * debug registers.
 *
 * That is not a bug in the debugger - the registers belong to it.  But a
 * measurement that silently returns zeros after every flash is the worst
 * kind of instrument.  Hence: check on every pass, re-set if needed, and
 * COUNT IT.  The counter is the actual payoff - without it, a disabled
 * counter would again have turned into an inconspicuous zero. */
static uint32_t s_dwt_rearm;

/* DWT->CYCCNT extended to 64 bits - the reference clock the wrap
 * correction needs.
 *
 * NOT instrumentation, but function: `pps_fix_wrap()` uses it to detect
 * the lost SYS_TIME overflow (E41..E44).  Called in the 1PPS path, i.e. at
 * 1 Hz - the 32-bit counter only overflows after 35.8 s, so the margin is
 * a factor of 35.  That is exactly where SYS_TIME fails: its 16 bits
 * overflow every 1.09 ms, and the bookkeeping for that occasionally misses
 * its deadline.
 *
 * With interrupts disabled, because reading and advancing the upper word
 * belong together - the same trap that already had to be patched in
 * SYS_TIME_PLIBCallback (2026-08-12). */
static uint32_t s_d64_prev;
static uint32_t s_d64_high;

uint64_t PTP_TRIG_Dwt64(void)
{
    uint64_t v;
    bool istate;

    if (!s_dwt_ok)
    {
        return 0u;
    }
    istate = SYS_INT_Disable();
    {
        uint32_t now = DWT->CYCCNT;
        if (now < s_d64_prev)
        {
            s_d64_high++;
        }
        s_d64_prev = now;
        v = ((uint64_t)s_d64_high << 32) | (uint64_t)now;
    }
    SYS_INT_Restore(istate);
    return v;
}

static void dwt_ensure(void)
{
    if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0u
        || (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0u)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
        s_dwt_rearm++;
        /* And renew the proof that it is running, instead of carrying the
           old verdict forward. */
        {
            uint32_t a = DWT->CYCCNT;
            uint32_t b = DWT->CYCCNT;
            s_dwt_ok = (b != a);
        }
    }
}

uint32_t PTP_TRIG_DwtRearms(void)
{
    return s_dwt_rearm;
}





static bool hw_arm_final(uint64_t target_L)
{
    bool ok = false;
    uint32_t st = __get_PRIMASK();
    __disable_irq();
    {
        uint64_t now = SYS_TIME_Counter64Get();
        if (target_L > now)
        {
            uint64_t rem = target_L - now;
            if (rem <= TC1_MAX_ARM_TICKS)
            {
                uint16_t cc = (uint16_t)((rem > TB_HW_LATENCY_TICKS) ? (rem - TB_HW_LATENCY_TICKS) : 1u);
                TC1_REGS->COUNT16.TC_CC[0] = cc;
                /* PULSE MODEL: the falling edge from the SAME retrigger.
                 *
                 * CC0 sets the pin (EVACT = SET), CC1 clears it (CLR), both
                 * from the same counter start.  That makes the high time
                 * the DIFFERENCE of two compare values, not dependent on a
                 * second command - a lost period costs one pulse, not the
                 * polarity.
                 *
                 * The limit is the 16-bit counter: cc + high time has to
                 * stay below the overflow, otherwise the falling edge would
                 * arrive a whole wrap period late - exactly the fault that
                 * cost E24.  If it does not fit, the high time is
                 * shortened and that gets counted, rather than silently
                 * producing something else. */
bool cc1_ok = false;
                                if (s_pulse_mode)
                {
                    /* CC1 belongs to the falling edge of the pulse that is
                     * CURRENTLY RUNNING, not the next one's.
                     *
                     * The first design set CC1 = cc + high time, i.e.
                     * behind CC0 - and because the re-arm restarts the
                     * counter at every grid point, CC1 was never reached
                     * (measured: pin stays high, 596,848 fires, zero
                     * edges).  The second design caught the falling edge
                     * up via an MC1 interrupt: that cost a SECOND 64-bit
                     * time read per period, and because the SYS_TIME race
                     * scales with the NUMBER of accesses (~0.1 %), that
                     * path lost 24 to 32 periods per two seconds at a p99
                     * of 11 us - against 0 and 1.3 us in toggle mode.
                     *
                     * This version needs no second interrupt: the
                     * retrigger happens right after grid point n, so the
                     * falling edge of n is now only (high time minus
                     * elapsed) ahead of us - a small value that fits in the
                     * same counter run as CC0 for point n+1.  One run, two
                     * edges, one interrupt. */
                    /* TWO CASES, and the second is measured to be
                     * necessary.
                     *
                     * `gone` is the path from the grid point to here:
                     * interrupt entry plus ISR lead-in.  MEASURED
                     * 2026-08-16 (S3.2) this is ~23 us at a 100 us grid -
                     * much more than the edge position would suggest,
                     * because the edge itself is produced by hardware and
                     * does not carry this delay.
                     *
                     * LONG PULSE (high time > elapsed): the falling edge of
                     * the pulse CURRENTLY RUNNING is still ahead of us, so
                     * it belongs in this run - `high time minus elapsed`.
                     *
                     * SHORT PULSE (high time <= elapsed): that edge is
                     * already past by the time this code runs.  Instead,
                     * the falling edge of the pulse that CC0 of THIS run is
                     * about to produce is scheduled: CC1 = cc + high time.
                     * That works because in the pulse model the counter is
                     * NOT stopped after MC0, and because the next
                     * retrigger only comes `gone` after CC0 - so the short
                     * edge lies before it.
                     *
                     * Without this second case, the emergency brake below
                     * triggered on EVERY period: at 10 % commanded, a high
                     * time of 22.85 us came out instead of 10 us, i.e.
                     * exactly the elapsed delay - a duty cycle that was not
                     * the commanded one and still looked stable. */
                    uint64_t cur = (s_period_ticks != 0u && target_L > s_period_ticks)
                                   ? (target_L - s_period_ticks) : 0u;
                    uint64_t gone = (now > cur) ? (now - cur) : 0u;
                    uint32_t cc1 = 0u;              /* 0 = keine Flanke planbar */

                    /* ONLY FOR A RUNNING PULSE TRAIN, not for a one-shot.
                     *
                     * This branch places CC1 BEFORE CC0, because in
                     * periodic operation the rising edge already happened
                     * `gone` ticks ago and the falling one belongs in the
                     * next counter run.  A one-shot has no previous pulse -
                     * there CC1 behind CC0 is the only correct choice, and
                     * only then can the counter stop itself at the
                     * overflow (ONESHOT).
                     *
                     * Measured 2026-08-18: `ONESHOT skipped: 3` for three
                     * one-shots - i.e. every time -, and that is exactly
                     * where the third stray edge that ONESHOT was supposed
                     * to eliminate came back. */
                    if (s_period_ns != 0u && cur != 0u && s_pulse_ticks > gone
                        && (s_pulse_ticks - gone) < (uint64_t)cc)
                    {
                        cc1 = (uint32_t)(s_pulse_ticks - gone);
                    }
                    else if (s_pulse_ticks != 0u
                             && ((uint64_t)cc + s_pulse_ticks) < 0xFFFFu)
                    {
                        cc1 = (uint32_t)cc + (uint32_t)s_pulse_ticks;
                    }

                    if (cc1 != 0u)
                    {
                        TC1_REGS->COUNT16.TC_CC[1] = (uint16_t)cc1;
                        while ((TC1_REGS->COUNT16.TC_SYNCBUSY & TC1_SYNCBUSY_CC1_M) != 0u) { }
                        /* Passt der Puls in EINEN Zaehlerlauf?  Nur dann darf der
                           Zaehler sich am Ueberlauf selbst anhalten. */
                        cc1_ok = (cc1 > cc);
                    }
                    else
                    {
                        /* Neither fits - all that is left is to clear the
                           level directly and get CC1 out of the way.
                           Counted, because a silent special case is exactly
                           the kind of bug nobody finds later. */
                        *TRIG_OUTCLR = BOARD_TRIG_MASK;
                        TC1_REGS->COUNT16.TC_CC[1] = 0xFFFFu;
                        while ((TC1_REGS->COUNT16.TC_SYNCBUSY & TC1_SYNCBUSY_CC1_M) != 0u) { }
                        s_pulse_clamped++;
                    }
                }
                /* CC0 is a write-synchronised register.  Retriggering before the
                 * new value has crossed into the 60 MHz domain makes the counter
                 * compare against the PREVIOUS CC0 and fire far too early - the
                 * Saleae saw -1.49 ms on one board while the other was clean,
                 * because the race depends on where in the sync period the arming
                 * lands.  The wait is a few GCLK cycles, constant, and disappears
                 * into TB_HW_LATENCY_TICKS. */
                while ((TC1_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_CC0_Msk) != 0u) { }
                TC1_REGS->COUNT16.TC_INTFLAG = TC_MC0;
                /* ONE-SHOT: LET THE COUNTER STOP ITSELF, NOT THE ISR.
                 *
                 * In one-shot mode TC1 stops at the overflow, i.e. AFTER
                 * CC0 and CC1 and BEFORE it passes them a second time.
                 * That leaves no stray edge, regardless of when the MC1
                 * ISR runs.
                 *
                 * MEASURED 2026-08-17: with the stop living only in the
                 * MC1 ISR, `trig` and `trigto` reproducibly produced 3
                 * transitions instead of 2 - rise, fall, and a second rise,
                 * because the counter kept running for one overflow
                 * (1092 us) before the delayed ISR stopped it.  A stop that
                 * depends on interrupt latency is no stop.
                 *
                 * For the periodic trigger, ONESHOT is EXPLICITLY CLEARED:
                 * there, every period retriggers, and a stuck ONESHOT bit
                 * would end the trigger for good after the first pulse. */
                /* ONESHOT only if the WHOLE pulse fits before the overflow.
                 *
                 * In one-shot mode the counter stops at the overflow - if
                 * CC1 lies past it, the falling edge never comes and the
                 * pin stays HIGH.  Measured 2026-08-18: `trigto` produced
                 * ONE transition instead of two,
                 * and the next step saw a pin that was already high.  A
                 * stop that swallows the effect is worse than the stray-edge
                 * bug it fixes - so in this case, take the path through
                 * MC1 and count it. */
                if (s_period_ns == 0u && cc1_ok)
                {
                    /* ONESHOT AND RETRIGGER IN ONE WRITE.
                     *
                     * CTRLB is write-synchronised, and two writes in a row
                     * cost the second one: measured 2026-08-18, EXACTLY THE
                     * FIRST one-shot after a mode change failed (zero
                     * edges), while the second and third fired flawlessly -
                     * because by then ONESHOT was already set and the write
                     * changed nothing more.  The counters said everything
                     * right along the way: `clamped 0`, `ONESHOT skipped 0`,
                     * `high 3000 ticks`, `one-shot stops` counted up.  So
                     * everything was armed - only the counter was never
                     * started.
                     *
                     * A register written twice is written once too often:
                     * both bits live in CTRLBSET, so they go together. */
                    TC1_REGS->COUNT16.TC_CTRLBSET = TC_ONESHOT | TC_CMD_RETRIGGER;
                }
                else
                {
                    if (s_period_ns == 0u) { s_oneshot_skip++; }
                    /* Different registers (CLR and SET), hence two writes
                       here - with the wait in between, so the clear has
                       landed before the counter starts. */
                    TC1_REGS->COUNT16.TC_CTRLBCLR = TC_ONESHOT;
                    while ((TC1_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_CTRLB_Msk) != 0u) { }
                    TC1_REGS->COUNT16.TC_CTRLBSET = TC_CMD_RETRIGGER;
                }
                /* CTRLB is write-synchronised too.  Enabling the match interrupt
                 * before the retrigger has crossed into the 60 MHz domain leaves
                 * the counter still running high - past CC0 - so the match lands
                 * a whole 65536-tick wrap later, 1.09 ms off.  The analyser saw
                 * exactly that on more than a tenth of the fires. */
                while ((TC1_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_CTRLB_Msk) != 0u) { }
                /* MC0 ONLY.  The falling edge is produced by the PORT event
                   itself; an interrupt for it would be a second time-read
                   per period and thereby double the SYS_TIME race's hit
                   rate. */
                TC1_REGS->COUNT16.TC_INTENSET = TC_MC0;
                s_hw_pending = true;
                ok = true;
            }
        }
    }
    /* COULD NOT BE ARMED -> STOP THE COUNTER.  Otherwise it keeps running
     * and passes its STALE CC0 on every wrap - in toggle mode that was E24
     * (permanent polarity inversion), in the pulse model it produces
     * pulses nobody commanded.
     *
     * MEASURED 2026-08-16 (E28) and it is a regression from the pulse
     * model: there the handler must NOT stop after MC0, because otherwise
     * the counter never reaches CC1 - which made the stop disappear from
     * the one path that used to guarantee it.  During a 999 ms dropout
     * this produced one pulse per wrap (1042 us measured), i.e. a signal
     * that looks like a slow square wave instead of a standstill - the
     * most expensive kind of bug, because it looks like it is working.
     *
     * The stop belongs exactly here and not in the handler: here it is
     * known that there is NO next compare. */
    if (!ok)
    {
        TC1_REGS->COUNT16.TC_CTRLBSET = TC_CMD_STOP;
        if (s_pulse_mode) { *TRIG_OUTCLR = BOARD_TRIG_MASK; }
    }
    if (st == 0u) { __enable_irq(); }
    return ok;
}

static void hw_disarm(void)
{
    /* STOP the counter, for the same reason TC1_Handler does - and this is the
     * place where it was missing.
     *
     * Disabling the interrupt is not enough: the compare match raises an EVENT
     * (TC1_REGS->COUNT16.TC_EVCTRL.MCEO0, set once in hw_tc1_init and never cleared), and the event
     * reaches the pin without the CPU.  Clearing INTENSET therefore does not
     * silence the pin - it silences the only code that would have stopped the
     * counter.  A running TC1 wraps every 65536 ticks and passes its stale CC0
     * again, so PD10 kept toggling every 1092.27 us while nothing was armed at all.
     *
     * Measured on 2026-08-15 in the w17 captures: hundreds of intervals of exactly
     * 1092.3 us on BOTH boards during the idle stretch before arming, and the
     * COUNT of them differed between the boards by one about half the time.  Since
     * the pin toggles, an odd difference inverts the level for good: both boards
     * then switch at exactly the same instants with opposite levels, which on a
     * rising-edge measurement reads as a whole grid period of skew.  That is the
     * "3 in 30 arms a full period off" defect - it was never a timing error.
     *
     * hw_arm_final() retriggers, and a retrigger restarts a stopped counter from
     * zero, so stopping here costs nothing on the next arm. */
    TC1_REGS->COUNT16.TC_CTRLBSET = TC_CMD_STOP;
    TC1_REGS->COUNT16.TC_INTENCLR = (uint8_t)(TC_MC0 | TC_MC1);
    TC1_REGS->COUNT16.TC_INTFLAG = (uint8_t)(TC_MC0 | TC_MC1);
    s_hw_pending = false;
}

/* --------------------------------------------------------------------------- */
/* helpers                                                                     */
/* --------------------------------------------------------------------------- */

static action_t *act_find(uint16_t id)
{
    uint32_t i;
    for (i = 0u; i < PTP_TRIG_ACTIONS; i++)
    {
        if (s_act[i].used && s_act[i].id == id)
        {
            return &s_act[i];
        }
    }
    return NULL;
}

static void hw_disarm(void);

static void trig_disarm(void)
{
    hw_disarm();
    s_stage2 = false;
    if (s_timer != SYS_TIME_HANDLE_INVALID)
    {
        SYS_TIME_TimerDestroy(s_timer);
        s_timer = SYS_TIME_HANDLE_INVALID;
    }
    s_armed = false;
}

static void trig_note_late(int32_t late)
{
    s_late_last = late;
    if (s_late_n == 0u)
    {
        s_late_max = late;
        s_late_min = late;
    }
    else
    {
        if (late > s_late_max) { s_late_max = late; }
        if (late < s_late_min) { s_late_min = late; }
    }
    /* Mean over the magnitude is meaningless with a sign, so sum the raw value
       and let the reader divide - min/max are the interesting columns anyway. */
    s_late_sum += (uint64_t)((late < 0) ? -late : late);
    s_late_n++;
}

/* Arms for a target already known in local ticks.  Two backends, chosen at
   runtime so both can be measured from one flash (see 'tbase hw'). */
static bool trig_arm_ticks(uint64_t target_L)
{
    uint64_t now = SYS_TIME_Counter64Get();
    uint64_t delay_ticks;
    uint32_t delay_us;

    if (target_L <= now)
    {
        return false;
    }
    delay_ticks = target_L - now;

    if (s_hw_mode)
    {
        /* Close enough for the precise hop straight away? */
        if (delay_ticks <= TC1_MAX_ARM_TICKS)
        {
            s_stage2 = false;
            return hw_arm_final(target_L);
        }
        /* Otherwise let SYS_TIME get us to ~0.5 ms out, then hand over.  Its own
           jitter does not matter here: it only has to land inside the window. */
        s_stage2 = true;
        delay_us = (uint32_t)((delay_ticks - (TC1_MAX_ARM_TICKS / 2u)) / s_ticks_per_us);
        if (delay_us == 0u)
        {
            s_stage2 = false;
            return hw_arm_final(target_L);
        }
        s_timer = SYS_TIME_CallbackRegisterUS(trig_cb, (uintptr_t)0, delay_us, SYS_TIME_SINGLE);
        return (s_timer != SYS_TIME_HANDLE_INVALID);
    }

    s_stage2 = false;
    delay_us = (uint32_t)(delay_ticks / s_ticks_per_us);
    if (delay_us == 0u)
    {
        return false;
    }

    /* Truncating to whole microseconds means the callback can arrive a fraction
       EARLY - hence the signed lateness.  Rounding up instead would bias every
       trigger late, which is worse: early is correctable by the handler, late is
       not.  This truncation is one of the two reasons phase C measured 25.5 us,
       and the hw backend above removes it. */
    s_timer = SYS_TIME_CallbackRegisterUS(trig_cb, (uintptr_t)0, delay_us, SYS_TIME_SINGLE);
    return (s_timer != SYS_TIME_HANDLE_INVALID);
}

/* --------------------------------------------------------------------------- */
/* the callback - ISR context                                                  */
/* --------------------------------------------------------------------------- */

/* Shared by both backends.  'now' is read by the caller as its very first act,
   so the lateness figure is not inflated by this function's own prologue. */
static void trig_fire(uint64_t now)
{
    int64_t  late_t;
    action_t *a;

    late_t = (int64_t)now - (int64_t)s_target_L;
    if (late_t > INT32_MAX) { late_t = INT32_MAX; }
    if (late_t < INT32_MIN) { late_t = INT32_MIN; }

    s_timer = SYS_TIME_HANDLE_INVALID;
    s_armed = false;
    trig_note_late((int32_t)late_t);
    s_cnt_fired++;

    a = act_find(s_armed_id);
    if (a != NULL)
    {
        if (a->isr_ctx)
        {
            a->fn(s_target_ns, (int32_t)late_t, a->ctx);
        }
        else
        {
            /* Deferred: only a flag here.  A handler that prints or sends would
               otherwise do it from the TC0 ISR (plan C.3). */
            s_pending_sched_ns = s_target_ns;
            s_pending_late = (int32_t)late_t;
            s_pending_id = s_armed_id;
            s_pending_defer = true;
        }
    }

    /* Periodic: in TOGGLE mode this re-arms here, in PULSE MODE only after
     * the falling edge (MC1).
     *
     * The reason is measured, not reasoned out: the re-arm retriggers the
     * counter, and a retriggered counter starts at 0.  If CC1 - the
     * falling edge - lies behind CC0, it is never reached.  The pin went
     * high once and stayed high, with 596,848 counted fires and correct
     * PORT_EVCTRL (0xCAAA, SET/CLR both enabled).  So one counter run has
     * to cover BOTH edges: 0 -> CC0 rising -> CC1 falling -> retrigger. */
    if (s_period_ns == 0u)
    {
        return;
    }
    trig_rearm(now);
}

/* The periodic re-arm, factored out of trig_fire() so the MC1 half of
   TC1_Handler() can call it too.  Content unchanged. */
/* Orderly end: stop the counter, disarm, restore the resting level.
 *
 * Factored out of PTP_TRIG_Cancel(), so a completed burst leaves the same
 * end state as a cancel - without the resting level moving after
 * trig_disarm().  That was in fact wrong once before: this function also
 * cleans up BEFORE a new arm, and there the resting level completely
 * killed the one-shot (PULSE_TRAIN_PLAN.md E5, FIXLOOP iteration 5).
 * Cancelling and rescheduling look the same in the code and are not. */
static void trig_stop_clean(void)
{
    s_period_ns = 0u;
    s_end_ns = UINT64_MAX;
    trig_disarm();
    *TRIG_OUTCLR = BOARD_TRIG_MASK;

    /* RELEASE THE GRID LED, and right HERE - this is the one bottleneck
     * every stop goes through (cancel as well as a completed pulse train).
     * A claim nobody gives back makes `tbase led` unusable for the rest
     * of the session, and the cause would then be a command sent from the
     * bridge an hour ago.
     *
     * LED off first, then release: after release this module must not
     * touch the pin any more. */
    if (s_gled_own)
    {
        led_set(0u, false);
        PIN_Release(s_led[0]->index, PIN_OWNER_GRIDLED);
        s_gled_own = false;
    }
    s_gled_want = false;
    /* AND RESET THE PHASE, not just turn the LED off.
     *
     * Measured 2026-08-24: without this line, `s_gled_phase` survived a
     * `trigoff`, and the level after a restart therefore hung off hidden
     * old state.  When demonstrating with local toggling (master off), the
     * phase of two boards then becomes unpredictable: it is the
     * combination of the CARRIED-OVER level and the parity of the start
     * offset, and the first part is invisible.  Four samples showed
     * in-phase blinking where a five-period offset predicted out-of-phase -
     * not an error in the prediction, but a value that was never reset.
     *
     * With the reset: even offset in periods -> in phase, odd -> out of
     * phase.  That is computable in advance, and the bridge NAMES the
     * start instant, so the prediction can be checked before the
     * measurement. */
    s_gled_phase = false;
    s_gled_tog   = 0u;
    /* Reset the phase and the open debt too.  A grid that has been
       stopped has no phase any more - and a leftover debt would carry
       forward into the next start even though it belongs to a grid that
       no longer exists.  Same lesson as with the LED phase on 2026-08-24:
       whatever survives a stop makes the next run unpredictable. */
    s_phase_ns   = 0u;
    s_slew_debt  = 0;

    /* And turn off the hardware divider's grid LED: with no grid events it
       no longer toggles and would stay wherever the last grid point left
       it.  The divider itself stays configured. */
    GDIV_GridStopped();
}

static void trig_rearm(uint64_t now)
{
    uint64_t next = s_target_ns + s_period_ns;
    uint64_t n    = s_grid_n + 1u;
    uint64_t now_ns;

    /* STOP in holdover instead of firing on.  PTP_TB_IsUsable() used to be
     * consulted only when SCHEDULING, so an armed periodic trigger kept going
     * with no time source - and the boards then diverge at the raw difference
     * of their crystals, measured -11.2 ppm, i.e. 11.2 us per second.  Ten
     * seconds of that beats an hour of the residual drift the whole servo plan
     * is about, and it is invisible from outside: the square wave stays clean,
     * the counters keep counting, only the reference is gone.
     *
     * Disarming leaves the pin where it is - no edge - and the watchdog in
     * PTP_TRIG_Tasks() re-arms onto the absolute grid as soon as the timebase
     * is usable again, so both boards come back in phase by construction. */
    if (s_mode == PTP_TRIG_MODE_STRICT && !PTP_TB_IsUsable())
    {
        s_cnt_ho_stop++;
        s_ho_disarmed = true;
        trig_disarm();
        /* TURN OFF THE GRID LED HERE TOO, and this is not tidiness added
         * as an afterthought: there are TWO stop paths, and this one goes
         * through `trig_disarm()`, not through `trig_stop_clean()`.
         * Without this line the LED froze wherever the last grid point
         * left it - LIT permanently in half the cases, indistinguishable
         * from a defect.  On pin PD10, freezing is intentional (no edge, so
         * the watchdog can cleanly re-arm onto the absolute grid again); an
         * LED, on the other hand, is a DISPLAY, and a display that stays
         * in its last state claims something that no longer holds.
         *
         * That way LED off uniformly means "no grid" - whether via
         * `trigoff` or because the master is missing.  Which of the two
         * cases applies is told by `tbase trig` (`stopped by holdover`) or
         * `tbase div` (`TC6 COUNT` stands still). */
        GDIV_GridStopped();
        return;
    }

    /* Was the grid index re-derived in THIS pass?  Then the PD10 level has
       to be pulled along - see the rationale at the call site below. */
    bool rederived = false;

    /* ---------------------------------------------- Pull the phase together, bounded
     *
     * Only with a usable timebase: without one there is no absolute
     * reference to pull towards, and in FREE the trigger deliberately
     * keeps firing on an extrapolated time.
     *
     * The debt is created ONCE, when the base becomes usable and the grid
     * is still hanging off its own start (s_phase_ns != 0).  It is reduced
     * in the SHORTER direction: if the own phase is past half the period,
     * it is shorter to DELAY the next point rather than pull it forward. */
    if (PTP_TB_IsUsable())
    {
        if (s_slew_debt == 0 && s_phase_ns != 0u)
        {
            uint64_t half = s_period_ns / 2u;

            s_slew_debt = (s_phase_ns <= half)
                        ? (int64_t)s_phase_ns                       /* pull forward */
                        : -(int64_t)(s_period_ns - s_phase_ns);     /* delay        */
            s_slew_runs++;
            s_slew_start_debt = (uint64_t)((s_slew_debt < 0) ? -s_slew_debt
                                                            : s_slew_debt);
        }
        if (s_slew_debt != 0)
        {
            uint64_t mag  = (uint64_t)((s_slew_debt < 0) ? -s_slew_debt : s_slew_debt);
            uint64_t rate = s_period_ns / s_slew_div;   /* fraction of the period */
            uint64_t step;

            if (rate == 0u) { rate = 1u; }              /* never 0 - or it hangs */
            step = (mag < rate) ? mag : rate;

            if (s_slew_debt > 0)
            {
                next -= step;               /* pull the point forward */
                s_slew_debt -= (int64_t)step;
            }
            else
            {
                next += step;               /* delay the point */
                s_slew_debt += (int64_t)step;
            }

            /* RE-DERIVE THE GRID INDEX AT THE END, and this is the point
             * where the demo would otherwise fail silently - E24 for the
             * third time.
             *
             * `n` simply keeps counting the whole time, but is anchored at
             * its OWN start.  So after pulling together, two boards have
             * the same instants but possibly OPPOSITE parity - they blink
             * at the same time and INVERTED, and that looks like "never
             * came together", even though the time is correct.
             *
             * With the re-derivation, both boards get the same index for
             * the same instant and therefore the same level.  The price is
             * a single skipped or doubled toggle at the moment the grid is
             * taken over - a one-off, visible as a single blink, and much
             * cheaper than a permanent inversion. */
            if (s_slew_debt == 0)
            {
                s_phase_ns = 0u;
                n = next / s_period_ns;
                s_slew_done++;
                rederived = true;

                /* THE SAME FOR THE DIVIDER, and without this line the demo
                   is only half fixed: PD10 then runs in phase, but the LED
                   does not.  It hangs off TC6.COUNT and off PC21's level,
                   and both are LOCAL - noticed on the bench on 2026-08-24
                   as visibly out-of-phase LEDs with a coincident PD10 (E24
                   for the fourth time, now one divider stage further
                   along). */
                GDIV_AlignRequest();
            }
        }
    }

    /* If the next instant is already gone, SKIP whole periods rather than
       firing a burst to catch up.  n is carried alongside next so the grid
       index stays exact across skips - deriving it here with a division
       would cost another 64-bit divide in the ISR, and the re-arm path is
       already the thing that sets the 20 us period floor (E3.1). */
    if (PTP_TB_Convert(now, &now_ns))
    {
        const uint64_t next_before = next;
        uint32_t skipped = 0u;
        while (next <= now_ns)
        {
            next += s_period_ns;
            n++;
            s_cnt_missed++;
            skipped++;
        }
        if (skipped >= SKIPLOG_MIN)
        {
            volatile skiplog_t *e = &s_skiplog[s_skiplog_n % SKIPLOG_N];
            e->tick_prev   = s_fire_tick_prev;
            e->tick        = now;
            e->ns_prev     = s_fire_ns_prev;
            e->ns          = now_ns;
            e->next_before = next_before;
            e->skipped     = skipped;
            s_skiplog_n++;
        }
        s_fire_tick_prev = now;
        s_fire_ns_prev   = now_ns;
    }

    /* THE END OF THE BURST, and the spot is precisely chosen: AFTER the
     * skip loop (which pushes `next` past `now`) and BEFORE the arm loop.
     * Checked earlier, a burst ending during a dropout would produce a
     * pulse PAST the end.
     *
     * The early return here is correct and not suspicious as it usually
     * would be: the holdover branch above does the same thing, and
     * trig_stop_clean() stops TC1, thereby reaching the same state as the
     * invariant block at the end of the function.
     *
     * s_cnt_rearm_lost and s_free_stop are DELIBERATELY left untouched: one
     * is an alarm ("a trigger that stops must recover or say so"), the
     * other counts the emergency brake against a free-running TC1.  A
     * completed burst is neither, and confusing the two makes the counters
     * useless for their actual purpose (E5). */
    if (next >= s_end_ns)
    {
        s_cnt_burst_done++;
        trig_stop_clean();
        return;
    }

    /* Retry across the following periods instead of giving up.
     *
     * The first version set s_armed only on success and stopped otherwise -
     * so ONE failed re-arm killed the periodic trigger for good, silently.
     * The firmware counters only hinted at it (a single 494 us outlier);
     * what actually exposed it was the logic analyser seeing 20 transitions
     * on one board where the other had 1504.  A trigger that stops must
     * either recover or say so, never just stop. */
    uint32_t tries;
    for (tries = 0u; tries < 8u; tries++)
    {
        if (PTP_TB_LocalFor(next, (uint64_t *)&s_target_L))
        {
            s_target_ns = next;
            if (trig_arm_ticks(s_target_L))
            {
                /* Level for the instant just armed, from the grid index -
                   this is what heals the parity after a dropout. */
                s_grid_n = n;
                pin_arm_for(n);

                /* PULL THE PD10 LEVEL ALONG AFTER A RE-DERIVATION, and
                 * without these six lines the demo is only half fixed -
                 * seen in a capture on the bench on 2026-08-24: LEDs in
                 * phase, the two boards' PD10 shifted by 180 degrees,
                 * edges coincident.
                 *
                 * The reason is the same as with the divider, just one
                 * level deeper: the PORT uses `EVACT = TGL`, so the pin
                 * only TOGGLES, and its level is therefore the COUNT of
                 * toggles since arming - not a function of the index.  Up
                 * to now it is anchored exactly once, at arm time
                 * (trig_arm_grid, pre-write).  If the grid pulls its phase
                 * together afterwards and re-derives the index, the index
                 * shifts out from under the pin; if the shifts of two
                 * boards differ in PARITY, they run inverted from then on -
                 * with correct timing.
                 *
                 * The LED does NOT show this, because its level is
                 * `(idx/n)&1` and a shift by 1 almost never changes that.
                 * That is why it looked like "LEDs correct, signal wrong",
                 * and it was a bug. */
                if (rederived && s_hw_mode && s_pin_armed && !s_pulse_mode)
                {
                    if (pin_preset_for(n))
                    {
                        s_cnt_pre_slew++;
                    }
                }

                /* THE ONE APPLICATION POINT FOR DIVIDER ALIGNMENT.  Here,
                   because the index is known exactly and means the same
                   thing on every board; with no pending request it is just
                   a flag test. */
                GDIV_AlignTick(n);
                s_armed = true;
                break;
            }
        }
        /* Too close now, or the model refused - aim a period further out. */
        next += s_period_ns;
        n++;
        s_cnt_missed++;
    }
    if (!s_armed)
    {
        s_cnt_rearm_lost++;
    }

    /* THE INVARIANT, AND IT WAS VIOLATED: TC1 MAY ONLY RUN WHEN A COMPARE
     * IS PENDING.
     *
     * MEASURED 2026-08-18 from the raw capture of two standing runs: in one
     * block of twelve, the pin produced ONE PULSE PER OVERFLOW for
     * ~0.95 s - 1042.3 us LOW plus 49.99 us HIGH, sum 1092.28 us, which is
     * 915 of 919 gaps.  So not a dropout, but a free-running counter
     * passing its STALE CC0/CC1 again on every overflow - the same
     * mechanism as E24 and E37, just a third path leading there.
     *
     * The path: if the next grid point lies further out than TC1 can
     * reach, the SYS_TIME stage takes over the deadline (`s_hw_pending`
     * stays false) - and nobody tells TC1 it has nothing left to do.  In
     * the pulse model, though, it still has two live compare values, and
     * those are enough for a neat, completely uncommanded square wave.
     *
     * Hence here, at the ONE place where both cases converge: no hardware
     * compare pending -> stop the counter and clear the level.  Counted,
     * because a safeguard nobody sees is not one. */
    if (!s_hw_pending)
    {
        TC1_REGS->COUNT16.TC_CTRLBSET = TC_CMD_STOP;
        if (s_pulse_mode) { *TRIG_OUTCLR = BOARD_TRIG_MASK; }
        s_free_stop++;
    }
}

/* SYS_TIME callback: either the software backend firing, or - in hw mode - the
   coarse stage handing the last millisecond over to TC1. */
static void trig_cb(uintptr_t context)
{
    uint64_t now = SYS_TIME_Counter64Get();
    (void)context;

    s_timer = SYS_TIME_HANDLE_INVALID;

    if (s_stage2)
    {
        s_stage2 = false;
        if (!hw_arm_final(s_target_L))
        {
            /* Two very different reasons to land here, and treating them alike
               was a bug worth 1.5 ms.

               TOO EARLY: SYS_TIME's coarse stage can fire well before its window,
               leaving more than TC1 can span.  The first version fired
               immediately, which is early by the whole remaining delay - the
               logic analyser saw -1.49 ms, matching the -89403 ticks the counter
               reported.  The right answer is to wait longer, not to fire.

               TOO LATE: the instant is gone.  Then firing now is the best
               available, and the lateness figure shows how bad it was. */
            if (s_target_L > now)
            {
                if (trig_arm_ticks(s_target_L))
                {
                    s_armed = true;
                    return;
                }
            }
            if (s_pin_armed) { TRIG_FIRE(); }
            s_armed = false;
            trig_fire(now);
        }
        return;
    }

    if (s_pin_armed) { TRIG_FIRE(); }
    s_armed = false;
    trig_fire(now);
}

/* TC1 compare match - the precise backend.  The pin is toggled as the first
   instruction after the counter read, so the edge carries as little of this
   handler as possible. */
/* Is this counter read consistent with the previous one?  Expected delta is one
   period; anything off by more than a quarter period is the counter lying, not
   the trigger being late, because the fire itself is hardware-timed. */
static inline void ck_check(uint64_t now)
{
    if (s_ck_prev != 0u && s_period_ns != 0u)
    {
        int64_t want = (int64_t)((s_period_ns / 1000u) * s_ticks_per_us);
        int64_t err  = (int64_t)(now - s_ck_prev) - want;
        int64_t wrap = (int64_t)(65536u * s_ticks_per_us / 60u);   /* one 16-bit wrap */

        /* Two very different faults, kept apart because lumping them together
           made the counter report "max 114112188 ticks", which is a two-second
           OUTAGE and not a misread at all.  A misread is bounded by a couple of
           wrap periods; anything larger means the trigger simply did not run. */
        if (err > 4 * wrap || err < -4 * wrap)
        {
            s_ck_gap++;
            if (err > s_ck_gap_max) { s_ck_gap_max = err; }
        }
        else if (err > wrap / 4 || err < -(wrap / 4))
        {
            s_ck_bad++;
            s_ck_last = err;
            if (err > s_ck_max) { s_ck_max = err; }
            if (err < s_ck_min) { s_ck_min = err; }
        }
    }
    s_ck_prev = now;
}

/* MAKE UP a lost SYS_TIME overflow, with the cycle counter as witness.
 *
 * THIS IS THE ROOT FIX FOR E41-E45, and the cause is a width problem:
 * `SYS_TIME_GetElapsedCount()` forms the difference of two 16-bit counter
 * readings and can therefore express AT MOST 65536 ticks (1092.27 us).  If
 * the compare interrupt arrives later than that - measured up to 1605 us
 * (E41) - it adds the difference modulo 65536 to `swCounter64`, and the
 * missing overflow is gone PERMANENTLY.  That is exactly what produces the
 * -1,092,964 ns residuals in the timebase, the re-anchoring, and the
 * ~40 us phase wander.
 *
 * The witness: DWT->CYCCNT runs at the CPU clock (120 MHz) and is
 * independent of TC0.  Its ratio to GCLK1 is measured at 2.0000015 - over
 * a 546 us ISR period that is 0.4 ticks of error, against a target
 * granularity of 65536.  Plenty of margin.
 *
 * Deliberately ONLY in the interrupt path and not in
 * `SYS_TIME_Counter64Get()`: there, the same ambiguity is transient and
 * heals itself at the next interrupt, while a second repair would double
 * count.
 *
 * If the witness fails (pyOCD clears DEMCR on disconnect), NOTHING is
 * repaired - the system then behaves as before, instead of relying on a
 * stalled counter.  And `missing` is capped: more than eight overflows
 * would no longer be a repair, it would be guesswork. */
uint32_t PTP_TRIG_FixElapsed(uint32_t elapsed)
{
    uint32_t cyc, dc, want, missing;

    if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0u
        || (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0u)
    {
        s_dwt_isr_have = false;          /* witness gone - discard the reference */
        return elapsed;
    }
    cyc = DWT->CYCCNT;
    if (!s_dwt_isr_have)
    {
        s_dwt_isr_prev = cyc;
        s_dwt_isr_have = true;
        return elapsed;
    }
    dc = cyc - s_dwt_isr_prev;           /* 32-bit overflow is correct here */
    s_dwt_isr_prev = cyc;

    want = dc / SYST_DWT_DIV;            /* CPU cycles -> GCLK1 ticks */
    if (want <= elapsed)
    {
        return elapsed;                  /* nothing missing (or the witness lags) */
    }
    /* Round to whole overflows: the difference is either ~0 or ~N*65536,
       nothing in between. */
    missing = ((want - elapsed) + (SYST_WRAP_TK / 2u)) / SYST_WRAP_TK;
    if (missing == 0u || missing > 8u)
    {
        return elapsed;
    }
    s_syst_repair += missing;
    s_syst_repair_tk = missing * SYST_WRAP_TK;
    return elapsed + missing * SYST_WRAP_TK;
}


uint32_t PTP_TRIG_SysTimeRepairs(void)
{
    return s_syst_repair;
}

/* The grid reference, for anyone who wants to measure an instant against
 * the grid (peer_capture.c).  Two values that only mean something
 * TOGETHER - hence read under PRIMASK.
 *
 * Exactly this kind of tear cost a second of silence on 2026-08-19: the
 * timebase's anchor used to be written in two assignments, and whoever hit
 * the gap computed with the new tick and the old time.  Here the reader is
 * the main loop and the writer is partly the ISR; `s_target_L` is 64 bit
 * and therefore two accesses on a 32-bit core anyway.  The lock costs
 * nanoseconds.
 *
 * WHICH grid point comes back does not matter: they all lie a whole
 * multiple of the period apart, so they are congruent modulo the period.
 * The caller only computes the phase anyway. */
bool PTP_TRIG_GridRef(uint64_t *ref_tick, uint64_t *period_ticks)
{
    bool ok;
    uint32_t prim = __get_PRIMASK();

    __disable_irq();
    ok = (s_armed && s_period_ticks != 0u);
    if (ok)
    {
        *ref_tick = s_target_L;
        *period_ticks = s_period_ticks;
    }
    if (prim == 0u)
    {
        __enable_irq();
    }
    return ok;
}


void TC1_Handler(void)
{
    uint64_t now = SYS_TIME_Counter64Get();

    /* The pin is NOT touched here any more - the compare match drove it through
     * EVSYS before this handler was even entered.  That is the point: the edge no
     * longer carries interrupt-entry jitter.
     *
     * IN THE PULSE MODEL, STOPPING HERE IS NOT ALLOWED, and that cost one
     * measurement round: the stop prevents the 1.09 ms ghost signal from
     * E24, but it also prevents the counter from ever reaching CC1 - and
     * CC1 is the FALLING edge.  Measured on 2026-08-16: `PORT_EVCTRL` was
     * correct (0xCAAA: SET at slot 0, CLR at slot 1, both enabled), the
     * trigger fired 596,848 times, and the analyser saw ZERO edges - the
     * pin went high once and stayed high.
     *
     * So in the pulse model, the stop only happens after the falling edge,
     * in the MC1 half of this handler.  The reason for the stop stays the
     * same: a counter that keeps running passes its stale CC0 on every
     * wrap. */
    if (!s_pulse_mode)
    {
        TC1_REGS->COUNT16.TC_CTRLBSET = TC_CMD_STOP;
    }

    /* MC1 is the falling edge: only stop and clean up, NO trig_fire().  A
     * second fire per period would be a duplicate count and a second
     * action - the pulse is ONE event with two edges. */
    /* The MC1 flag is only cleaned up here - the falling edge already got
       the PORT event done before any code ran. */
    if ((TC1_REGS->COUNT16.TC_INTFLAG & TC_MC1) != 0u)
    {
        s_mc1_n++;
        TC1_REGS->COUNT16.TC_INTFLAG = TC_MC1;

        /* ONLY the falling edge, no MC0 alongside it: that is the end of a
         * ONE-SHOT in the pulse model (only there is MC1 enabled, see
         * below) - and the one moment where both things are known: the
         * pulse is done, and no next compare is coming.  This is exactly
         * where the stop belongs.
         *
         * MEASURED 2026-08-17 (E45), and it is E24/E37 for the third time:
         * without this stop, TC1 kept running after a `trig` and passed
         * its two stale compare values on every overflow - 1959
         * transitions instead of one, gaps of 433.67 and 658.60 us, sum
         * 1092.27 us.  The comment above claimed a stop happened here; it
         * described an intent, not code.
         *
         * No `trig_fire()` and no `ck_check()`: that was already correct
         * before, and an early exit is the only form in which it STAYS
         * correct - the rest of this function assumes MC0. */
        if ((TC1_REGS->COUNT16.TC_INTFLAG & TC_MC0) == 0u)
        {
            TC1_REGS->COUNT16.TC_INTENCLR = TC_MC1;
            /* Only stop if truly nothing is pending any more.  If
               `trig_fire()` re-armed in the meantime, the counter belongs
               to the next compare - then the stop would be the bug E28
               describes. */
            if (!s_armed && !s_hw_pending)
            {
                TC1_REGS->COUNT16.TC_CTRLBSET = TC_CMD_STOP;
                *TRIG_OUTCLR = BOARD_TRIG_MASK;
                s_shot_stop++;
            }
            return;
        }
    }

    s_mc0_n++;
    ck_check(now);

    TC1_REGS->COUNT16.TC_INTFLAG = TC_MC0;
    TC1_REGS->COUNT16.TC_INTENCLR = TC_MC0;
    s_hw_pending = false;
    s_armed = false;
    trig_fire(now);

    /* ONE-SHOT IN THE PULSE MODEL: request the falling edge, so the
     * counter can be stopped afterwards (above).  Only AFTER `trig_fire()`,
     * because only then is it known whether a re-arm happened - in
     * periodic operation `s_armed` is true again by now, MC1 stays
     * disabled, and the objection from the comment at `hw_arm_final()`
     * ("a second time read per period") does not apply: this interrupt
     * comes once per ONE-SHOT, not per period, and the branch above reads
     * no time.
     *
     * The MC1 flag is NOT cleared here.  If the falling edge has already
     * passed (a very short high time), it is still set, the interrupt
     * fires immediately, and the stop happens anyway - clearing it would
     * swallow exactly this case and let the counter run forever. */
    if (s_pulse_mode && !s_armed && !s_hw_pending)
    {
        TC1_REGS->COUNT16.TC_INTENSET = TC_MC1;
    }
}

/* --------------------------------------------------------------------------- */
/* public                                                                      */
/* --------------------------------------------------------------------------- */


void PTP_TRIG_Initialize(void)
{
    /* The counter-clock first: it costs nothing and is read by
       hw_arm_final() from the very first call on. */
    PTP_TRIG_DwtInit();
    uint32_t hz = SYS_TIME_FrequencyGet();

    memset(s_act, 0, sizeof(s_act));
    /* TC1 FIRST, because hw_disarm() writes TC1 registers and TC1's APB clock is
       gated out of reset - hw_tc1_init() is what ungates it.  The two disarm calls
       below used to run against a clock-gated peripheral; that went unnoticed
       because their writes did nothing, which is precisely the reason a missing
       STOP could hide here. */
    hw_tc1_init();
    trig_disarm();
    hw_disarm();
    hw_pin_init();
    led_init();
    hw_evsys_pin_init();
    s_pin_op = TRIG_OUTTGL;   /* until a grid is armed, behave as before */
    s_grid_n = 0u;
    s_hw_mode = true;      /* E1 by default; 'tbase hw off' falls back to phase C */
    /* Through ArmPin(), not by assigning s_pin_armed: since the pin is driven by
       the PORT event, the gate is PORTEI0 in hardware and setting the flag alone
       leaves it closed.  That is exactly what happened on the first try - the
       trigger fired happily and the analyser saw nothing at all. */
    (void)PTP_TRIG_ArmPin(true);
    s_stage2 = false;
    s_period_ns = 0u;
    s_phase_ns = 0u;
    s_pending_defer = false;
    s_mode = PTP_TRIG_MODE_STRICT;
    s_cnt_fired = 0u;
    s_cnt_refused = 0u;
    s_cnt_missed = 0u;
    s_cnt_rearm_lost = 0u;
    s_cnt_stalled = 0u;
    s_cnt_ho_stop = 0u;
    s_ho_disarmed = false;
    s_ck_prev = 0u; s_ck_bad = 0u; s_ck_min = 0; s_ck_max = 0; s_ck_last = 0;
    s_ck_gap = 0u; s_ck_gap_max = 0;
    s_late_last = 0;
    s_late_max = 0;
    s_late_min = 0;
    s_late_sum = 0u;
    s_late_n = 0u;

    if (hz == 0u)
    {
        hz = 60000000u;
    }
    s_ticks_per_us = (uint64_t)hz / 1000000u;
    if (s_ticks_per_us == 0u)
    {
        s_ticks_per_us = 1u;
    }
}

/* Which action ids this follower actually has, as a bitmask, for the STATUS
   reply.  A master that knows a board is alive still cannot use it without
   knowing what it can do, and "action 3 does nothing here" is otherwise
   indistinguishable from a lost command: both leave the pin quiet.  Ids beyond
   31 are not represented - none exist today, and a wider field would have to
   travel in the reply for no present gain. */
uint32_t PTP_TRIG_ActionMask(void)
{
    uint32_t i, mask = 0u;

    for (i = 0u; i < PTP_TRIG_ACTIONS; i++)
    {
        if (s_act[i].used && s_act[i].id < 32u)
        {
            mask |= (1uL << s_act[i].id);
        }
    }
    return mask;
}

bool PTP_TRIG_Register(uint16_t action_id, PTP_TRIG_Handler h, uintptr_t ctx, bool isr_ctx)
{
    uint32_t i;
    action_t *a = act_find(action_id);

    if (h == NULL)
    {
        return false;
    }
    if (a == NULL)
    {
        for (i = 0u; i < PTP_TRIG_ACTIONS; i++)
        {
            if (!s_act[i].used)
            {
                a = &s_act[i];
                break;
            }
        }
        if (a == NULL)
        {
            return false;
        }
        a->used = true;
        a->id = action_id;
        a->seq_valid = false;
    }
    a->fn = h;
    a->ctx = ctx;
    a->isr_ctx = isr_ctx;
    return true;
}

/* Common validation for both schedule entry points. */
static PTP_TRIG_RESULT trig_check(uint16_t action_id, uint32_t cmd_seq, action_t **out)
{
    action_t *a = act_find(action_id);

    if (a == NULL)
    {
        return PTP_TRIG_ERR_NO_ACTION;
    }
    if (!PTP_TB_IsUsable() && s_mode == PTP_TRIG_MODE_STRICT)
    {
        return PTP_TRIG_ERR_NO_TIME;
    }
    /* Fire exactly once per (action, sequence).  A repeated command - the master
       sends one-way and repeats deliberately - must not schedule twice (plan
       C.7).  The SOME/IP Session ID is what carries this over the wire. */
    if (a->seq_valid && a->last_seq == cmd_seq)
    {
        return PTP_TRIG_ERR_DUPLICATE;
    }
    *out = a;
    return PTP_TRIG_OK;
}

PTP_TRIG_RESULT PTP_TRIG_ScheduleAt(uint16_t action_id, uint32_t cmd_seq, uint64_t tx_ns)
{
    action_t *a = NULL;
    PTP_TRIG_RESULT r = trig_check(action_id, cmd_seq, &a);
    uint64_t now_ns;
    uint64_t target_L;

    if (r != PTP_TRIG_OK)
    {
        s_cnt_refused++;
        return r;
    }
    if (s_armed)
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_BUSY;
    }
    if (!PTP_TB_Now(&now_ns))
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_NO_TIME;
    }
    if (tx_ns < now_ns + (uint64_t)PTP_TRIG_MIN_LEAD_MS * 1000000ULL)
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_PAST;
    }
    if (tx_ns > now_ns + (uint64_t)PTP_TRIG_MAX_LEAD_MS * 1000000ULL)
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_TOO_FAR;
    }
    if (!PTP_TB_LocalFor(tx_ns, &target_L))
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_NO_TIME;
    }

    s_target_ns = tx_ns;
    s_target_L = target_L;
    s_period_ns = 0u;
    s_armed_id = action_id;
    /* No grid to derive a level from, so a one-shot still toggles. */
    s_pin_op = TRIG_OUTTGL;

    if (!trig_arm_ticks(target_L))
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_ARM;
    }
    s_armed = true;
    a->last_seq = cmd_seq;
    a->seq_valid = true;
    return PTP_TRIG_OK;
}

/* Arms the next instant on the absolute grid defined by s_period_ns/s_phase_ns.
   Shared by the CLI/command path and by the stall watchdog so the two cannot
   drift apart - in particular so a recovered trigger lands on the same grid, and
   with the same pin level, as one that was armed normally. */
static bool trig_arm_grid(void)
{
    uint64_t now_ns;
    uint64_t n;
    uint64_t next;
    uint64_t target_L;
    uint32_t tries;

    if (s_period_ns == 0u || !PTP_TB_Now(&now_ns))
    {
        return false;
    }

    /* TAKE THE POINT NAMED BY THE MASTER, if there is one.
     *
     * This is the fix for the measured index bug: everything that follows
     * computes correctly in absolute terms, but the minimum-lead
     * correction further down ties the result to `now_ns` - and `now_ns`
     * is not the same on two boards, because they process the same
     * command in different main-loop passes.  If `next` sits just above
     * the threshold for one board and just below it for the other, the
     * second one adds one extra step and fires a whole grid period later
     * (FIXPLAN_10KHZ.md T1: 3 of 30 arm cycles).
     *
     * A number from the master is produced exactly ONCE and therefore
     * cannot differ between boards.  It only applies to the first arm - the
     * re-arm during operation carries the grid forward and was never
     * affected. */
    bool from_master = (s_first_ns != 0u);

    /* WITHOUT A USABLE TIMEBASE, ANCHOR THE GRID AT ITS OWN START.
     *
     * Otherwise every board computes `next` from the same absolute time
     * and lands on the SAME point - the start instant is deliberately made
     * meaningless in normal operation (FIXPLAN_10KHZ.md T1).  But that is
     * exactly what blocks the demo, where two boards started one after
     * another are SUPPOSED to be far apart.
     *
     * If the timebase is usable, s_phase_ns stays at 0 and everything
     * behaves as before - so the demo cannot alter normal operation. */
    if (!from_master && !PTP_TB_IsUsable())
    {
        /* ONLY IF THE MASTER EXPLICITLY NAMED NO POINT, and this is the
         * fix for a measured regression (2026-08-24).
         *
         * The first build used to discard the named start point itself as
         * soon as the timebase was unusable - and thereby turned a
         * MOMENTARY state into a permanent local anchor.  That bit in
         * test_arm_parity.py: run 5 of 20 with k=1, offset +89,960 ns (0.9
         * of a 100 us period) and inverted start level, where two
         * reference runs before it had 0 of 20.  An arbitrary
         * sub-period offset is exactly this line's signature.
         *
         * Now the OPERATOR decides: `trigper ... w` sends start_ns = 0, and
         * only then does this branch get used.  So the demo's intent is
         * announced instead of guessed from a state that is allowed to
         * flicker. */
        s_phase_ns = now_ns % s_period_ns;
        s_phase_local++;
    }

    if (from_master)
    {
        next = s_first_ns;
        n = (next - s_phase_ns) / s_period_ns;
        s_first_ns = 0u;
    }
    else
    /* Absolute phase: the next instant on the global grid, not "now + period".
       This is the whole reason two nodes can align at all (plan G.1). */
    if (now_ns <= s_phase_ns)
    {
        next = s_phase_ns;
        n = 0u;
    }
    else
    {
        n = (now_ns - s_phase_ns) / s_period_ns + 1u;
        next = s_phase_ns + n * s_period_ns;
    }
    /* Respect the same minimum lead as a one-shot.  Computed, not iterated: this
       used to step one period at a time, which is 500 iterations at a 100 us
       period and 2500 at 20 us - harmless arithmetic, but it grows without bound
       as the period shrinks, and it runs with the timebase already read. */
    if (!from_master)
    {
        uint64_t lead = now_ns + (uint64_t)PTP_TRIG_MIN_LEAD_MS * 1000000ULL;
        if (next < lead)
        {
            uint64_t steps = ((lead - next) + s_period_ns - 1u) / s_period_ns;
            n    += steps;
            next += steps * s_period_ns;
        }
    }

    /* PLACE THE FIRST GRID POINT AT ODD PARITY.
     *
     * The pre-write of the level below is necessary (otherwise two boards
     * run permanently inverted, E24) - but it happens at ARM TIME, and the
     * first grid point only arrives after the master's lead time,
     * measured at 200 ms (TRIGM_DEFAULT_LEAD_MS).  At EVEN n, parity
     * demands the pre-level be HIGH; the pin would then go high at arm
     * time and stay that way for those 200 ms.  In a capture that is a
     * wide block ahead of the signal - not a timing error, but an edge
     * that sits on no grid point, and a start picture nobody wants to
     * demonstrate that way.
     *
     * At ODD n, the pre-level is LOW, and LOW is the pin's resting state.
     * The write below is then a level write to a pin that already has that
     * level - it produces nothing.  So n gets pushed forward by ONE grid
     * point when needed, instead of forcing the level.
     *
     * THIS DOES NOT BREAK ALIGNMENT: n comes from the absolute grid
     * (s_phase_ns, s_period_ns, s_first_ns - all three from the same
     * broadcast), so every board computes the same index and therefore
     * shifts the same way.  The price is a start one grid period later,
     * here 100 us against a 200 ms lead.
     *
     * In the PULSE MODEL this does not apply: there CC0 sets and CC1
     * clears, parity is defined per period, and there is nothing to
     * pre-write. */
    if (s_hw_mode && s_pin_armed && !s_pulse_mode && ((n & 1u) == 0u))
    {
        n    += 1u;
        next += s_period_ns;
    }

    for (tries = 0u; tries < 8u; tries++)
    {
        /* Pre-set the level in HARDWARE mode, and only there.
         *
         * In software mode the fire writes s_pin_op, which pin_arm_for() derives
         * from the grid index, so the level is anchored by construction and a
         * pre-set would be redundant.  In hardware mode it is not: the PORT event
         * uses EVACT = TGL, so the pin merely TOGGLES and pin_arm_for() never
         * reaches it.  Which of the two levels a board ends up on is then decided
         * by whenever it happened to arm - and two boards arming at slightly
         * different moments run INVERTED against each other for good.
         *
         * That is not theory: on 2026-08-14 a capture showed both followers
         * switching at exactly the same instants with opposite levels, which on a
         * rising-edge measurement reads as a full period of skew.  Synchronous and
         * inverted is the worst of both - it looks broken and measures broken,
         * while the timing was right all along.
         *
         * The earlier comment here argued a pre-set "injects an EXTRA edge".  It
         * cannot: OUTSET and OUTCLR are level writes, so setting a pin that
         * already holds that level produces nothing.  An edge appears exactly when
         * the level was wrong - which is the correction, once, off-grid, at arm
         * time.  Against a permanent inversion that is a bargain. */
        /* In the PULSE MODEL there is no parity to preserve: CC0 sets, CC1
           clears, the level is defined per period.  A software pre-write
           here would be exactly the kind of intervention the model is
           meant to eliminate. */
        if (s_hw_mode && s_pin_armed && !s_pulse_mode)
        {
            /* The parity computation lives in pin_preset_for().

               COUNTED, BECAUSE IT IS AN EDGE OUTSIDE THE GRID.  If the
               level is already correct, OUTSET/OUTCLR writes nothing - if
               it is not, an edge appears here that sits on no grid point
               and stays as a block in the capture until the first grid
               point (at a 200 ms lead, that is 200 ms wide).  Without this
               counter that is only visible with an analyser, and only then
               if you happen to look at the start of the capture.  With the
               parity choice above it MUST sit at 0, as long as the pin
               rests LOW. */
            s_pre_last_n = n;
            if (pin_preset_for(n))
            {
                s_cnt_pre_edge++;
            }
        }

        if (PTP_TB_LocalFor(next, &target_L))
        {
            s_target_ns = next;
            s_target_L = target_L;
            if (trig_arm_ticks(target_L))
            {
                s_grid_n = n;
                pin_arm_for(n);
                /* A new grid reference: the divider has to align to it
                   afterwards.  Requested, not executed - the application
                   point is the re-arm, and that comes one period later. */
                GDIV_AlignRequest();
                s_armed = true;
                return true;
            }
        }
        /* IN STEPS OF TWO, where parity applies: a single step flips it,
           and because the pre-write above sits INSIDE this loop, the pin
           would again go high 200 ms too early on the second attempt -
           the same block, just rarer and therefore harder to find.  In the
           pulse model there is no parity, so the single step stays
           correct there. */
        if (s_hw_mode && s_pin_armed && !s_pulse_mode)
        {
            next += 2u * s_period_ns;
            n += 2u;
            s_cnt_missed += 2u;
        }
        else
        {
            next += s_period_ns;
            n++;
            s_cnt_missed++;
        }
    }
    return false;
}

PTP_TRIG_RESULT PTP_TRIG_SchedulePeriodic(uint16_t action_id, uint32_t cmd_seq,
                                          uint64_t period_ns, uint64_t phase_ns,
                                          uint64_t start_ns, uint32_t count)
{
    action_t *a = NULL;
    PTP_TRIG_RESULT r = trig_check(action_id, cmd_seq, &a);
    uint64_t now_ns;

    if (r != PTP_TRIG_OK)
    {
        s_cnt_refused++;
        return r;
    }
    if (period_ns == 0u)
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_PAST;
    }
    if (!PTP_TB_Now(&now_ns))
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_NO_TIME;
    }

    /* Refused BEFORE disarming whatever is currently running: a command that
       cannot work must not take down a trigger that does. */
    if (period_ns < (uint64_t)PTP_TRIG_MIN_PERIOD_US * 1000ULL)
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_TOO_FAST;
    }

    trig_disarm();

    s_period_ns = period_ns;
    s_phase_ns = phase_ns;
    pulse_recalc();      /* the high time depends on the period */
    s_armed_id = action_id;
    /* Only adopt it if it still lies in the future and sits on the grid -
       a start point that has already passed would be worse than choosing
       one yourself. */
    s_first_ns = 0u;
    if (start_ns == 0u)
    {
        s_start_absent++;
    }
    else if (start_ns <= now_ns)
    {
        s_start_past++;
    }
    else if (((start_ns - phase_ns) % period_ns) != 0u)
    {
        s_start_offgrid++;
    }
    else
    {
        s_first_ns = start_ns;
        s_start_taken++;
    }

    /* THE END, computed AFTER the start-point check - because it depends
     * on the ACCEPTED start point, not the one that was commanded.
     *
     * Overflow is refused, not saturated: a saturated value would be
     * UINT64_MAX, i.e. "unlimited" - silently the opposite of the
     * command. */
    if (count == 0u)
    {
        s_end_ns = UINT64_MAX;
    }
    else if (s_first_ns == 0u)
    {
        s_period_ns = 0u;
        s_cnt_refused++;
        return PTP_TRIG_ERR_NO_START;
    }
    else
    {
        uint64_t span = (uint64_t)count * period_ns;

        if (period_ns != 0u && (span / period_ns) != (uint64_t)count)
        {
            s_period_ns = 0u;
            s_cnt_refused++;
            return PTP_TRIG_ERR_TOO_FAR;
        }
        if (span > (UINT64_MAX - s_first_ns))
        {
            s_period_ns = 0u;
            s_cnt_refused++;
            return PTP_TRIG_ERR_TOO_FAR;
        }
        s_end_ns = s_first_ns + span;
    }

    if (!trig_arm_grid())
    {
        s_period_ns = 0u;
        s_end_ns = UINT64_MAX;
        s_cnt_refused++;
        return PTP_TRIG_ERR_ARM;
    }
    a->last_seq = cmd_seq;
    a->seq_valid = true;
    return PTP_TRIG_OK;
}

void PTP_TRIG_Cancel(void)
{
    /* The body has lived in trig_stop_clean() since the burst rework,
       because a completed pulse train needs the same end state as a
       cancel.  The rationale for the resting level stays HERE, because it
       was worked out at this site and is needed wherever someone asks
       "why is a level being written here". */
    trig_stop_clean();
    /* RESTORE THE RESTING LEVEL - and ONLY HERE, on an explicit cancel.
     *
     * In the pulse model the pin has a resting level, and a trigger
     * stopped mid-pulse leaves it HIGH: measured 2026-08-18, the capture
     * after `trigoff` correctly showed zero edges and a start level of 1,
     * and the following one-shot therefore produced only ONE transition -
     * its rising edge had already been anticipated.
     *
     * The first attempt set this in `trig_disarm()`, and that was wrong:
     * that function also cleans up BEFORE a new arm (`trig_arm_grid()`
     * calls it), so the one-shot dropped out entirely - three steps with
     * zero edges, while the boards reported `accepted: 5  refused: 0`.
     * Cancelling and rescheduling look the same in the code and are not.
     *
     * 2026-08-19: NOW APPLIES TO BOTH MODELS, not just the pulse model any
     * more.  In toggle mode there used to be no defined resting level - the
     * pin stayed wherever the last toggle left it, i.e. HIGH in half the
     * cases.  Two consequences, both measured: the next arm had to fetch
     * the level and in doing so produced an edge outside the grid, which
     * stayed as a 200 ms wide block in the capture until the first grid
     * point (an increase of 2 of 4, or 1 of 4, arm cycles starting from
     * "rest"); and a pin stuck high produces NO edge, so it would have
     * passed any rest check indefinitely.
     *
     * Since the parity rule in trig_arm_grid(), LOW is the defined
     * pre-level in toggle mode too - which makes it a real resting level
     * and not an arbitrary choice.  The price is one falling edge on
     * cancel if the pin was high: it lies outside any measurement and buys
     * a clean start picture.
     *
     * Executed in trig_stop_clean(), no longer here. */
}

void PTP_TRIG_ModeSet(PTP_TRIG_MODE mode)
{
    s_mode = mode;
}

uint64_t PTP_TRIG_GridIndex(void)
{
    return s_grid_n;
}

uint8_t PTP_TRIG_GridEventGen(void)
{
    /* Here, because this is where TC1 is configured - see the rationale in
       the header. */
    return (uint8_t)EVENT_ID_GEN_TC1_MC_0;
}

void PTP_TRIG_PinMark(void)
{
    /* A scope marker for DEFERRED actions, which run in the main loop and are
     * therefore invisible to the ISR-driven pin.  That distinction is the whole
     * point: an action that queues a frame does so from the main loop, so the
     * instant that matters for a collision is this one, not the grid instant.
     *
     * Deliberately a TOGGLE and deliberately the same pin, so no rewiring is
     * needed - but for the same reason it must not run while the ISR is driving
     * the level from the grid index, or the two fight over the pin.  Use
     * `tbase pin off` when measuring with this marker. */
    PORT_REGS->GROUP[BOARD_TRIG_GROUP].PORT_OUTTGL = BOARD_TRIG_MASK;
}

bool PTP_TRIG_ArmPin(bool enable)
{
    /* The claim on the probe pin is registered here, not when the trigger
     * is armed: the question is "who DRIVES the pin", and it is driven as
     * soon as the PORT event is enabled - even when no instant is
     * currently scheduled.
     *
     * A FAILED claim is deliberately not passed through: the pin is then
     * held by a GPIO or PWM handle, and its owner must not be displaced by
     * operating the trigger.  Instead the event just stays off - the
     * trigger keeps firing, only without the pin, and `tbase pins` says
     * who holds it. */
    if (enable && !PIN_Claim(BOARD_TRIG_PIN_INDEX, PIN_OWNER_TRIGGER))
    {
        SYS_CONSOLE_PRINT("[TRIG] pin %u is held by '%s', not driving it\r\n",
                          BOARD_TRIG_PIN_INDEX, PIN_OwnerName(PIN_OwnerGet(BOARD_TRIG_PIN_INDEX)));
        s_pin_armed = false;
        hw_pin_event_enable(false);
        return false;
    }
    if (!enable) { PIN_Release(BOARD_TRIG_PIN_INDEX, PIN_OWNER_TRIGGER); }

    s_pin_armed = enable;
    /* With the hardware path the gate is PORTEI0, not a branch in the ISR. */
    hw_pin_event_enable(enable);
    return true;
}

/* High time from period and duty cycle, in SYS_TIME ticks.
 *
 * A separate function because it is needed at TWO sites: when setting the
 * duty cycle and at every new period.  Serving only one of the two gives a
 * duty cycle that, after a frequency change, means something different
 * from what it says - and that goes unnoticed, because the signal keeps
 * running. */
static void pulse_recalc(void)
{
    if (s_ticks_per_us == 0u)
    {
        s_pulse_ticks = 0u;
        s_period_ticks = 0u;
        return;
    }
    if (s_period_ns == 0u)
    {
        /* A ONE-SHOT HAS NO PERIOD - AND STILL NEEDS A HIGH TIME.
         *
         * This used to set both to zero, which made the pulse's high time
         * zero: the arm landed in the clamp branch, cleared the level in
         * software, and the pulse was INVISIBLE.  Measured 2026-08-18 on
         * freshly reset boards: `trig 1 300` produced zero transitions
         * with `master start: taken 2` and `clamped: 1` - the board
         * accepted the command and carried it out into nothing.
         *
         * It was only noticed because the step ran in isolation: in the
         * test plan, the quality measurement arms 100 us beforehand, and
         * then `s_period_ticks` was still left over from that round.  A
         * command whose effect depends on what ran before it is broken -
         * even if it works in the common case.
         *
         * So: the one-shot uses the project's default period (100 us) with
         * the same duty cycle.  That is a CHOICE, not a measurement, hence
         * it is stated here and visible in `tbase pulse`. */
        s_period_ticks = (uint64_t)PULSE_DEFAULT_US * s_ticks_per_us;
        s_pulse_ticks = s_period_ticks * (uint64_t)s_pulse_pct / 100ULL;
        return;
    }
    s_period_ticks = (s_period_ns / 1000ULL) * s_ticks_per_us;
    s_pulse_ticks = s_period_ticks * (uint64_t)s_pulse_pct / 100ULL;
}

void PTP_TRIG_PulseSet(bool on, uint32_t duty_pct)
{
    if (duty_pct >= 1u && duty_pct <= 99u)
    {
        s_pulse_pct = duty_pct;
    }
    s_pulse_mode = on;
    pulse_recalc();
    /* Switch the PORT action and re-set the gate - in this order, because
       hw_pin_event_enable() only enables the second slot in pulse mode. */
    hw_pin_action_set(s_pulse_mode);
    (void)PTP_TRIG_ArmPin(s_pin_armed);
    /* In the pulse model every period starts with a rising edge, so the
       resting level belongs at LOW.  Without this, the first pulse starts
       as a gap or not depending on the previous state - the same class of
       bug as the parity one, just a one-off. */
    if (s_pulse_mode)
    {
        *TRIG_OUTCLR = BOARD_TRIG_MASK;
    }
}

bool PTP_TRIG_PulseGet(uint32_t *duty_pct)
{
    if (duty_pct != NULL) { *duty_pct = s_pulse_pct; }
    return s_pulse_mode;
}

void PTP_TRIG_HwSet(bool enable)
{
    s_hw_mode = enable;
}

bool PTP_TRIG_HwGet(void)
{
    return s_hw_mode;
}

/* An armed periodic trigger whose instant is long gone will never fire again.
 *
 * Measured on the bench, both boards: fire counter frozen, `armed: yes`,
 * stage2 = yes, a LIVE SYS_TIME handle and rearm lost = 0 - i.e. the coarse
 * one-shot was accepted and then never called back.  It is registered from
 * inside the SYS_TIME callback (ISR context), which is the fragile part, so the
 * recovery deliberately happens HERE, in the main loop, and re-arms onto the
 * absolute grid rather than "now + period" so the board rejoins in phase.
 *
 * The threshold is generous: 2 ms is already 33 times the worst lateness ever
 * measured (60 us), so this cannot trip on a merely late fire. */
static void trig_watchdog(void)
{
    int64_t togo;

    if (s_period_ns == 0u)
    {
        return;                 /* no periodic trigger wanted - nothing to guard */
    }

    /* A periodic trigger that GAVE UP is just as dead as one that hangs, and the
     * first version of this watchdog only handled the second case: it returned
     * immediately unless s_armed was true.  Found by switching the timebase source
     * at run time, which clears the model - the re-arm inside trig_fire() failed
     * once, counted `rearm lost: 1`, set s_armed = false, and nothing ever brought
     * it back.  Both boards then sat silent with a perfectly healthy timebase.
     *
     * The re-arm loop's own comment demands that a stopped trigger either recovers
     * or says so.  It said so and did not recover; this is the recovery. */
    /* The pulse-mode intermediate state is NOT a stall - but it must not
     * last forever either.  If MC1 stays off (say because CC1 was
     * programmed unreachable - exactly the 2026-08-16 bug), the trigger
     * would otherwise stay silent forever, and the comment on the re-arm
     * loop has demanded since E1 that a stuck trigger either recovers or
     * says so.  So: two periods of patience, then count it and arm it
     * yourself. */
    if (!s_armed)
    {
        if (PTP_TB_IsUsable() || s_mode == PTP_TRIG_MODE_FREE)
        {
            /* A holdover stop is a correct, expected pause - not a stall.  Keeping
               the two apart matters: "stalls recovered" is meant to read zero in
               normal operation, and folding every grandmaster restart into it
               would make the number meaningless. */
            if (!s_ho_disarmed) { s_cnt_stalled++; }
            s_ho_disarmed = false;
            (void)trig_arm_grid();
        }
        return;
    }
    togo = (int64_t)s_target_L - (int64_t)SYS_TIME_Counter64Get();
    if (togo > -(int64_t)(2u * TC1_MAX_ARM_TICKS))
    {
        return;
    }

    trig_disarm();
    s_cnt_stalled++;
    (void)trig_arm_grid();     /* leaves it disarmed if the timebase is gone */
}

void PTP_TRIG_Tasks(void)
{
    dwt_ensure();      /* der Debugger schaltet TRCENA beim Abmelden ab - siehe dort */
    led_service();
    gled_claim_service();
    trig_watchdog();

    if (s_pending_defer)
    {
        action_t *a;
        uint64_t sched = s_pending_sched_ns;
        int32_t  late = s_pending_late;
        uint16_t id = s_pending_id;

        s_pending_defer = false;
        a = act_find(id);
        if (a != NULL && !a->isr_ctx)
        {
            a->fn(sched, late, a->ctx);
        }
    }
}

void PTP_TRIG_StatusGet(PTP_TRIG_STATUS *out)
{
    if (out == NULL)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->armed = s_armed;
    out->mode = s_mode;
    out->action_id = s_armed_id;
    out->target_ns = s_target_ns;
    out->period_ns = s_period_ns;
    out->fired = s_cnt_fired;
    out->refused = s_cnt_refused;
    out->missed = s_cnt_missed;
    out->end_ns = s_end_ns;
    out->burst_done = s_cnt_burst_done;
    out->late_last_ticks = s_late_last;
    out->late_max_ticks = s_late_max;
    out->late_min_ticks = s_late_min;
    out->late_sum_ticks = s_late_sum;
    out->late_n = s_late_n;
    out->ticks_per_us = (uint32_t)s_ticks_per_us;
}

const char *PTP_TRIG_ResultName(PTP_TRIG_RESULT r)
{
    switch (r)
    {
        case PTP_TRIG_OK:             return "ok";
        case PTP_TRIG_ERR_NO_ACTION:  return "no such action id";
        case PTP_TRIG_ERR_NO_TIME:    return "timebase not usable";
        case PTP_TRIG_ERR_PAST:       return "target in the past or too close";
        case PTP_TRIG_ERR_TOO_FAR:    return "target too far ahead";
        case PTP_TRIG_ERR_DUPLICATE:  return "duplicate action+sequence";
        case PTP_TRIG_ERR_BUSY:       return "a trigger is already armed";
        case PTP_TRIG_ERR_ARM:        return "could not arm the timer";
        case PTP_TRIG_ERR_TOO_FAST:   return "period too short for the re-arm path";
        case PTP_TRIG_ERR_NO_START:   return "count without an absolute start";
        default:                      return "?";
    }
}

/* --------------------------------------------------------------------------- */
/* built-in demo actions and CLI                                               */
/* --------------------------------------------------------------------------- */

/* Action 1, ISR context: the cheapest thing that still proves the trigger
   fired at the right instant.  No printing - that is what action 2 is for. */
static volatile uint32_t s_demo_isr_hits;
static volatile uint64_t s_demo_isr_L;

static void demo_isr(uint64_t scheduled_ns, int32_t late_ticks, uintptr_t ctx)
{
    (void)scheduled_ns; (void)late_ticks; (void)ctx;
    s_demo_isr_L = SYS_TIME_Counter64Get();
    s_demo_isr_hits++;
}

/* Action 2, task context: allowed to print, which action 1 is not. */
static void demo_task(uint64_t scheduled_ns, int32_t late_ticks, uintptr_t ctx)
{
    (void)ctx;
    SYS_CONSOLE_PRINT("[TRIG] action 2 fired: scheduled %llu ns, late %ld ticks (%ld ns)\r\n",
                      (unsigned long long)scheduled_ns, (long)late_ticks,
                      (long)((int64_t)late_ticks * 1000 / (int64_t)s_ticks_per_us));
}

/* "20" -> 20 ms, "20ms" -> 20 ms, "500us" -> 500 us.  Returns nanoseconds, or 0
 * for anything malformed so the caller can refuse it.
 *
 * A BARE NUMBER STAYS MILLISECONDS on purpose.  Every note, runbook and finger
 * in this project has `tbase per 20` in it, and reinterpreting that as
 * microseconds would silently change one command by a factor of 1000 while it
 * still looked like it worked - the same shape of mistake as `tbase 20 1` with a
 * forgotten `per`.  Making the unit explicit is what removes that class.
 *
 * No "ns" suffix: two boards agree to about +/-2 us, so a nanosecond period
 * would promise a resolution that does not exist. */
static uint64_t parse_time_ns(const char *s)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);

    if (end == s || v == 0u)
    {
        return 0u;
    }
    if (*end == '\0' || !strcmp(end, "ms"))
    {
        return (uint64_t)v * 1000000ULL;
    }
    if (!strcmp(end, "us"))
    {
        return (uint64_t)v * 1000ULL;
    }
    return 0u;                    /* a typo is refused, never guessed */
}

/* Prints a duration in whichever unit reads naturally.  Needed because the old
   "%llu ms" showed a 500 us period as "0 ms". */
static void print_dur(uint64_t ns)
{
    if (ns >= 1000000ULL && (ns % 1000000ULL) == 0u)
    {
        SYS_CONSOLE_PRINT("%lu ms", (unsigned long)(ns / 1000000ULL));
    }
    else
    {
        SYS_CONSOLE_PRINT("%lu us", (unsigned long)(ns / 1000ULL));
    }
}

bool PTP_TRIG_CliTry(int argc, char **argv)
{
    PTP_TRIG_STATUS st;
    PTP_TRIG_RESULT r;
    uint64_t now_ns;

    if (argc >= 3 && !strcmp(argv[1], "fire"))
    {
        uint32_t ahead_ms = (uint32_t)strtoul(argv[2], NULL, 0);
        uint16_t id = (argc >= 4) ? (uint16_t)strtoul(argv[3], NULL, 0) : 1u;
        static uint32_t seq = 0u;
        if (!PTP_TB_Now(&now_ns))
        {
            SYS_CONSOLE_PRINT("[TRIG] no timebase\r\n");
            return true;
        }
        r = PTP_TRIG_ScheduleAt(id, ++seq, now_ns + (uint64_t)ahead_ms * 1000000ULL);
        SYS_CONSOLE_PRINT("[TRIG] schedule id=%u in %lu ms: %s\r\n",
                          (unsigned)id, (unsigned long)ahead_ms, PTP_TRIG_ResultName(r));
        return true;
    }

    /* `tbase burst <period> <n> [id]` - exactly n grid points, then stop.
     *
     * WHY A START POINT IS COMPUTED HERE, even though `tbase per` hard-codes
     * 0u: the end is start + n * period, and with start = 0 ("follower
     * chooses for itself") it would be board-dependent - a burst with no
     * shared start is not a fleet action, it is n separate actions that
     * happen to look similar (PULSE_TRAIN_PLAN.md E2).  That is why the
     * time layer explicitly refuses this combination, and this command
     * would otherwise have to fail.
     *
     * Computed the same way as in the master (trigm_fire_periodic): now +
     * lead time, then ROUND UP to the next grid multiple.  The lead time is
     * the same as the master's default, so a locally started burst and a
     * commanded one sit on the same timescale - locally it could be
     * shorter, but a second number would be a second source of truth.
     *
     * The output names start AND end, because both need to be checkable:
     * end - start / period == n is the check on the command before an
     * analyser is even switched on. */
    if (argc >= 4 && !strcmp(argv[1], "burst"))
    {
        uint64_t period_ns = parse_time_ns(argv[2]);
        uint32_t n = (uint32_t)strtoul(argv[3], NULL, 0);
        uint16_t id = (argc >= 5) ? (uint16_t)strtoul(argv[4], NULL, 0) : 1u;
        static uint32_t bseq = 2000u;
        uint64_t now_ns = 0u;
        uint64_t start = 0u;

        if (period_ns == 0u)
        {
            SYS_CONSOLE_PRINT("[TRIG] cannot read a period from '%s'\r\n", argv[2]);
            SYS_CONSOLE_PRINT("       tbase burst <20|20ms|500us> <n> [id]\r\n");
            return true;
        }
        if (n == 0u)
        {
            /* 0 means "unlimited" on the wire, and that is `tbase per`.
               Here it would be a silent mode switch - so refuse instead
               of guessing. */
            SYS_CONSOLE_PRINT("[TRIG] n = 0 means unlimited - use 'tbase per'"
                              " for that\r\n");
            return true;
        }
        if (!PTP_TB_Now(&now_ns))
        {
            SYS_CONSOLE_PRINT("[TRIG] no usable timebase - cannot compute a"
                              " start\r\n");
            return true;
        }
        {
            uint64_t lead = now_ns + (uint64_t)PTP_TRIG_BURST_LEAD_MS * 1000000ULL;
            uint64_t k = (lead + period_ns - 1u) / period_ns;   /* phase 0 */
            start = k * period_ns;
        }
        r = PTP_TRIG_SchedulePeriodic(id, ++bseq, period_ns, 0u, start, n);
        SYS_CONSOLE_PRINT("[TRIG] burst id=%u every ", (unsigned)id);
        print_dur(period_ns);
        SYS_CONSOLE_PRINT(", phase 0, %lu pulses,\r\n"
                          "       start %llu.%09llu s   end %llu.%09llu s: %s\r\n",
                          (unsigned long)n,
                          (unsigned long long)(start / 1000000000ULL),
                          (unsigned long long)(start % 1000000000ULL),
                          (unsigned long long)((start + (uint64_t)n * period_ns)
                                               / 1000000000ULL),
                          (unsigned long long)((start + (uint64_t)n * period_ns)
                                               % 1000000000ULL),
                          PTP_TRIG_ResultName(r));
        return true;
    }

    if (argc >= 3 && !strcmp(argv[1], "per"))
    {
        uint64_t period_ns = parse_time_ns(argv[2]);
        uint16_t id = (argc >= 4) ? (uint16_t)strtoul(argv[3], NULL, 0) : 1u;
        static uint32_t pseq = 1000u;

        if (period_ns == 0u)
        {
            SYS_CONSOLE_PRINT("[TRIG] cannot read a period from '%s'\r\n", argv[2]);
            SYS_CONSOLE_PRINT("       tbase per 20 | 20ms | 500us   (bare number = ms)\r\n");
            return true;
        }
        /* phase 0: the grid is anchored on grandmaster zero, so every node lands
           on the same instants no matter when it got the command (plan G.1). */
        /* count = 0: `tbase per` stays unlimited.  The limited pulse train
           is `tbase burst` - its own subcommand instead of a third
           positional argument, because `tbase per 100us 256` would
           otherwise mean "id = 256", and that mix-up happened exactly
           once (PULSE_TRAIN_PLAN.md 9.2). */
        r = PTP_TRIG_SchedulePeriodic(id, ++pseq, period_ns, 0u, 0u, 0u);
        SYS_CONSOLE_PRINT("[TRIG] periodic id=%u every ", (unsigned)id);
        print_dur(period_ns);
        SYS_CONSOLE_PRINT(", phase 0: %s\r\n", PTP_TRIG_ResultName(r));
        return true;
    }

    if (argc >= 2 && !strcmp(argv[1], "cancel"))
    {
        PTP_TRIG_Cancel();
        SYS_CONSOLE_PRINT("[TRIG] cancelled\r\n");
        return true;
    }

    if (argc >= 2 && !strcmp(argv[1], "hw"))
    {
        if (argc >= 3)
        {
            PTP_TRIG_HwSet(!strcmp(argv[2], "on") || !strcmp(argv[2], "1"));
        }
        SYS_CONSOLE_PRINT("[TRIG] backend: %s\r\n",
                          PTP_TRIG_HwGet() ? "E1 (TC1 compare)" : "C (SYS_TIME callback)");
        return true;
    }

    /* tbase pulse [on|off] [percent] - set the level instead of toggling.
     *
     * Flippable at runtime, so both paths are comparable on ONE flash: the
     * same reason as with `tbase pps` and `tbase hw`, and the only honest
     * way to prove "no worse than before". */
    if (argc >= 2 && !strcmp(argv[1], "pulse"))
    {
        if (argc >= 3)
        {
            bool on = (!strcmp(argv[2], "on") || !strcmp(argv[2], "1"));
            uint32_t pct = s_pulse_pct;
            if (argc >= 4)
            {
                int v = atoi(argv[3]);
                if (v < 1 || v > 99)
                {
                    SYS_CONSOLE_PRINT("[TRIG] duty must be 1..99 %% - refused\r\n");
                    return true;
                }
                pct = (uint32_t)v;
            }
            PTP_TRIG_PulseSet(on, pct);
        }
        SYS_CONSOLE_PRINT("[TRIG] pulse: %s   duty asked: %lu %%   clamped: %lu\r\n",
                          s_pulse_mode ? "on (SET/CLR)" : "off (toggle)",
                          (unsigned long)s_pulse_pct,
                          (unsigned long)s_pulse_clamped);
        /* THE ACHIEVED VALUES, not just the commanded ones.  Both are shown
         * because they CAN diverge: high time and period are whole counter
         * ticks, so every combination of period and percentage rounds.  A
         * tool that commands 30 % and gets 29.9 % should be able to read
         * that here instead of discovering it on the analyser.
         *
         * Resolution: at 60 MHz one tick is 16.67 ns; the duty cycle in
         * hundredths of a percent, because a whole percentage point is
         * already 1 us at a 100 us period - i.e. the very magnitude this is
         * about. */
        if (s_period_ticks != 0u && s_ticks_per_us != 0u)
        {
            uint32_t p100 = (uint32_t)((s_pulse_ticks * 10000ULL) / s_period_ticks);
            SYS_CONSOLE_PRINT("[TRIG] achieved: period %lu ticks (%lu ns)"
                              "   high %lu ticks (%lu ns)   duty %lu.%02lu %%\r\n",
                              (unsigned long)s_period_ticks,
                              (unsigned long)((s_period_ticks * 1000u) / s_ticks_per_us),
                              (unsigned long)s_pulse_ticks,
                              (unsigned long)((s_pulse_ticks * 1000u) / s_ticks_per_us),
                              (unsigned long)(p100 / 100u), (unsigned long)(p100 % 100u));
        }
        else
        {
            SYS_CONSOLE_PRINT("[TRIG] achieved: no period armed yet\r\n");
        }
        SYS_CONSOLE_PRINT("[TRIG] mc0: %lu   mc1: %lu   watchdog: %lu   one-shot stops: %lu"
                          "   SYS_TIME repairs: %lu (last %lu ticks)"
                          "   free-run stopped: %lu   ONESHOT skipped: %lu\r\n",
                          (unsigned long)s_mc0_n, (unsigned long)s_mc1_n,
                          (unsigned long)s_pulse_await_lost,
                          (unsigned long)s_shot_stop,
                          (unsigned long)s_syst_repair,
                          (unsigned long)s_syst_repair_tk,
                          (unsigned long)s_free_stop,
                          (unsigned long)s_oneshot_skip);
        /* burst done is DELIBERATELY its own counter: a pulse train that
           ended normally is neither a lost arm (s_cnt_rearm_lost) nor the
           emergency brake against a free-running counter (s_free_stop).
           Mixing them up makes both useless for their actual purpose. */
        if (s_end_ns == UINT64_MAX)
        {
            SYS_CONSOLE_PRINT("[TRIG] burst done: %lu   end: unlimited\r\n",
                              (unsigned long)s_cnt_burst_done);
        }
        else
        {
            SYS_CONSOLE_PRINT("[TRIG] burst done: %lu   end: %llu.%09llu s\r\n",
                              (unsigned long)s_cnt_burst_done,
                              (unsigned long long)(s_end_ns / 1000000000ULL),
                              (unsigned long long)(s_end_ns % 1000000000ULL));
        }
        return true;
    }

    if (argc >= 3 && !strcmp(argv[1], "pin"))
    {
        (void)PTP_TRIG_ArmPin(!strcmp(argv[2], "on") || !strcmp(argv[2], "1"));
        SYS_CONSOLE_PRINT("[TRIG] PD10 toggle: %s\r\n",
                          (!strcmp(argv[2], "on") || !strcmp(argv[2], "1")) ? "on" : "off");
        return true;
    }

    if (argc >= 2 && !strcmp(argv[1], "led"))
    {
        /* Which board is this terminal talking to?  `led blink` answers it from
           across the bench.  Defaults to LED1 (PC21); pass 2 for LED2 (PA16). */
        unsigned which = (argc >= 4) ? (unsigned)strtoul(argv[3], NULL, 0) : 1u;
        const char *what = (argc >= 3) ? argv[2] : "";

        if (which < 1u || which > BOARD_LED_COUNT)
        {
            SYS_CONSOLE_PRINT("[LED] no LED %u, only 1 (PC21) and 2 (PA16)\r\n", which);
            return true;
        }
        which -= 1u;

        /* The identification LED also registers its claim - otherwise it
         * keeps blinking under a GPIO handle, and the tool at the other
         * end sees a pin that does not follow its command.  `off` releases
         * it again.
         *
         * The claim sits in the INDIVIDUAL branches and not before them: a
         * typo must not leave behind a claim that nobody releases
         * afterwards. */
        if (!strcmp(what, "on") || !strcmp(what, "1"))
        {
            if (!PIN_Claim(s_led[which]->index, PIN_OWNER_BLINK)) { goto led_busy; }
            s_led_blink &= (uint8_t)~(1u << which);
            led_set(which, true);
        }
        else if (!strcmp(what, "off") || !strcmp(what, "0"))
        {
            s_led_blink &= (uint8_t)~(1u << which);
            led_set(which, false);
            PIN_Release(s_led[which]->index, PIN_OWNER_BLINK);
        }
        else if (!strcmp(what, "blink"))
        {
            if (!PIN_Claim(s_led[which]->index, PIN_OWNER_BLINK)) { goto led_busy; }
            s_led_blink |= (uint8_t)(1u << which);
            s_led_next_L = 0u;          /* service it on the next pass */
        }
        else
        {
            /* Refuse a typo instead of doing something adjacent - the same
               lesson the `mirror` command learned in cff7cdf. */
            SYS_CONSOLE_PRINT("[LED] unknown '%s'\r\nusage: tbase led on|off|blink [1|2]\r\n",
                              what);
            return true;
        }
        SYS_CONSOLE_PRINT("[LED] LED%u (%s, pin %u): %s\r\n", which + 1u,
                          (which == 0u) ? "PC21" : "PA16", s_led[which]->index,
                          ((s_led_blink & (1u << which)) != 0u) ? "blink" : what);
        return true;

    led_busy:
        SYS_CONSOLE_PRINT("[LED] pin %u is held by '%s'\r\n", s_led[which]->index,
                          PIN_OwnerName(PIN_OwnerGet(s_led[which]->index)));
        return true;
    }

    if (argc >= 2 && !strcmp(argv[1], "pins"))
    {
        PIN_Print();
        return true;
    }

    if (argc >= 2 && !strcmp(argv[1], "ep"))
    {
        /* Make the endpoint layer visible.  It lives in someip.c, and this
           printout is the ONLY connection to it - the protocol path does
           not know the CLI, and the CLI only knows the protocol's own
           report. */
        if (argc >= 3 && (!strcmp(argv[2], "offer")))
        {
            /* Switching it off is the more interesting counter-check: an
               endpoint that does not offer itself does not exist for
               `lan866x-discovery` - and
               that distinguishes "the offer matters" from "the host finds
               it anyway". */
            bool on = (argc >= 4) ? (!strcmp(argv[3], "on") || !strcmp(argv[3], "1"))
                                  : true;
            EPSRV_OfferSet(on);
            EPSRV_Print();
            return true;
        }
        if (argc >= 3 && (!strcmp(argv[2], "log")))
        {
            bool on = (argc >= 4) && (!strcmp(argv[3], "on") || !strcmp(argv[3], "1"));
            EPSRV_LogSet(on);
            SYS_CONSOLE_PRINT("[EP] frame log: %s\r\n", on ? "on" : "off");
            return true;
        }
        EPSRV_Print();
        return true;
    }

    if (argc >= 2 && !strcmp(argv[1], "slew"))
    {
        /* The rate at which an own phase gets pulled onto the absolute grid.
         *
         * THE DEMO NO LONGER NEEDS THIS.  The rate is a fraction of the
         * period and thereby scales by itself; this command only remains
         * to FALSIFY the mechanism - halve the divider and if the pull-in
         * takes half as long, it is proven.
         *
         * And what gets set is the DIVIDER, not a time: a time would again
         * be meaningless at a different period, and that is exactly what
         * the first build failed on (500 ns per point = 2 us/s at a
         * 250 ms grid). */
        if (argc >= 3)
        {
            unsigned long v = strtoul(argv[2], NULL, 0);

            /* Lower bound 8: a step of more than an eighth of the period
               pushes the next point into the past, at which point the
               catch-up loop takes over and the grid skips points. */
            if (v < 8ul || v > 1000000ul)
            {
                SYS_CONSOLE_PRINT("[TRIG] slew <divider 8..1000000>"
                                  "   step = period/divider per grid point\r\n");
                return true;
            }
            s_slew_div = (uint32_t)v;
        }
        SYS_CONSOLE_PRINT("[TRIG] slew: period/%lu per grid point",
                          (unsigned long)s_slew_div);
        if (s_period_ns != 0u)
        {
            /* The practical number: how long the worst case (half a
               period) takes - the value one compares against a stopwatch.
               It does NOT depend on the period, which is precisely the
               point of the fraction. */
            SYS_CONSOLE_PRINT("   half period in %lu s",
                              (unsigned long)((uint64_t)(s_slew_div / 2u)
                                              * s_period_ns / 1000000000ull));
        }
        SYS_CONSOLE_PRINT("\r\n");
        return true;
    }

    if (argc >= 3 && !strcmp(argv[1], "mode"))
    {
        bool free_mode = (!strcmp(argv[2], "free"));
        PTP_TRIG_ModeSet(free_mode ? PTP_TRIG_MODE_FREE : PTP_TRIG_MODE_STRICT);
        SYS_CONSOLE_PRINT("[TRIG] mode: %s%s\r\n", free_mode ? "FREE" : "STRICT",
                          free_mode ? "  <- fires without a usable timebase, NOT synchronized" : "");
        return true;
    }

    if (argc < 2 || strcmp(argv[1], "trig") != 0)
    {
        return false;              /* not ours - let tbase handle it */
    }

    PTP_TRIG_StatusGet(&st);
    SYS_CONSOLE_PRINT("[TRIG] backend: %s   PD10: %s\r\n",
                      PTP_TRIG_HwGet() ? "E1 (TC1 compare)" : "C (SYS_TIME callback)",
                      s_pin_armed ? "on" : "off");
    SYS_CONSOLE_PRINT("[TRIG] armed: %s   mode: %s   action: %u   period: ",
                      st.armed ? "yes" : "no",
                      (st.mode == PTP_TRIG_MODE_FREE) ? "FREE" : "STRICT",
                      (unsigned)st.action_id);
    print_dur(st.period_ns);
    SYS_CONSOLE_PRINT("\r\n");
    SYS_CONSOLE_PRINT("[TRIG] fired: %lu   refused: %lu   skipped periods: %lu   rearm lost: %lu\r\n",
                      (unsigned long)st.fired, (unsigned long)st.refused, (unsigned long)st.missed,
                      (unsigned long)s_cnt_rearm_lost);
    SYS_CONSOLE_PRINT("[TRIG] stalls recovered by watchdog: %lu"
                      "   stopped by holdover: %lu\r\n",
                      (unsigned long)s_cnt_stalled, (unsigned long)s_cnt_ho_stop);
    /* The start point named by the master: taken or discarded, and why.
       Without this line, two fix attempts left it open whether the fix
       does not take effect or the theory is wrong - and that is exactly
       the question. */
    SYS_CONSOLE_PRINT("[TRIG] master start: taken %lu   past %lu   off-grid %lu"
                      "   absent %lu   local phase %lu\r\n",
                      (unsigned long)s_start_taken, (unsigned long)s_start_past,
                      (unsigned long)s_start_offgrid,
                      (unsigned long)s_start_absent,
                      (unsigned long)s_phase_local);
    /* WHERE THE SECOND CAME FROM.  Without these lines the ring buffer
       would not exist - the lesson from `skipped periods` never being
       printed. */
    if (s_skiplog_n != 0u)
    {
        uint32_t i;
        uint32_t cnt = (s_skiplog_n < SKIPLOG_N) ? s_skiplog_n : SKIPLOG_N;
        SYS_CONSOLE_PRINT("[TRIG] large skips: %lu   (buffer shows the last %lu)\r\n",
                          (unsigned long)s_skiplog_n, (unsigned long)cnt);
        SYS_CONSOLE_PRINT("[TRIG]   n  periods   d(tick)      d(ns)        now_ns - next_before\r\n");
        for (i = 0u; i < cnt; i++)
        {
            volatile skiplog_t *e = &s_skiplog[i];
            /* d(tick) in ticks, d(ns) in ns: a healthy period is 6000
               ticks and 100,000 ns.  A second would be 60,000,000 ticks
               or 1,000,000,000 ns. */
            SYS_CONSOLE_PRINT("[TRIG]  %2lu  %8lu  %10lld  %11lld  %14lld\r\n",
                              (unsigned long)i, (unsigned long)e->skipped,
                              (long long)((int64_t)e->tick - (int64_t)e->tick_prev),
                              (long long)((int64_t)e->ns - (int64_t)e->ns_prev),
                              (long long)((int64_t)e->ns - (int64_t)e->next_before));
        }
    }
    SYS_CONSOLE_PRINT("[TRIG] edges while arming (outside the grid): %lu"
                      "   first grid index %llu (%s)\r\n",
                      (unsigned long)s_cnt_pre_edge,
                      (unsigned long long)s_pre_last_n,
                      ((s_pre_last_n & 1u) != 0u) ? "odd - pre-level LOW"
                                                  : "EVEN - pre-level HIGH");
    /* Counted separately: AFTER a pull-in, a correction edge is the normal
       case (the index shifted out from under the toggling pin), but at arm
       time it is a finding.  If this reads 0 even though `slew done`
       counted, the level was already correct - both are fine, just not the
       same thing. */
    SYS_CONSOLE_PRINT("[TRIG] level corrected after re-derivation: %lu\r\n",
                      (unsigned long)s_cnt_pre_slew);
    SYS_CONSOLE_PRINT("[TRIG] counter misreads: %lu   last %ld   min %ld   max %ld ticks"
                      "  (32768 = half the 16-bit wrap)\r\n",
                      (unsigned long)s_ck_bad, (long)s_ck_last,
                      (long)s_ck_min, (long)s_ck_max);
    {
        /* And WHO is holding up the loop.  Sections of SYS_Tasks() in
           cycles; 120 cycles = 1 us.  The section with the largest maximum
           is the cause of the 1.15 ms gap from E41. */
        /* The names follow the TEMPORAL order of the markers, not the call
           structure: marker 5 sits at the end of APP_Tasks(), but markers
           6..11 sit INSIDE it before that - so section 5 is only the rest
           of APP after TRIGCMD (packet log, state machine), not all of
           APP. */
        /* THE SIZE AND COUNT OF THE ENTRIES MUST MATCH - here it said [12]
           for 14 entries, and the loop ran to 14.  For i = 12, 13,
           `names[i]` read past the array, `%s` dereferenced a wild
           pointer, HardFault.  Both followers died on the command; it
           became visible through the click board's traffic LED, which
           hangs off SPI CS and stopped flickering - the main loop had
           stalled, not just the console.  The compiler had warned ("excess
           elements in array initializer"); my build invocation filtered
           for "error:" and swallowed the warning. */
    }
    SYS_CONSOLE_PRINT("[TRIG] outages (trigger did not run): %lu   longest %ld ms\r\n",
                      (unsigned long)s_ck_gap,
                      (long)(s_ck_gap_max / (int64_t)(s_ticks_per_us * 1000u)));

    /* Where the arm chain is sitting right now.  Printing only "armed: yes" was
       a lie worth an hour: both boards reported armed while their fire counters
       were frozen, and nothing in the output said which stage had stalled. */
    {
        uint64_t nowL = SYS_TIME_Counter64Get();
        int64_t  togo = (int64_t)s_target_L - (int64_t)nowL;
        SYS_CONSOLE_PRINT("[TRIG] chain: stage2=%s  hw_pending=%s  systime_timer=%s"
                          "  target-now=%ld ticks (%ld us)\r\n",
                          s_stage2 ? "yes" : "no",
                          s_hw_pending ? "yes" : "no",
                          (s_timer == SYS_TIME_HANDLE_INVALID) ? "none" : "live",
                          (long)togo, (long)(togo / (int64_t)s_ticks_per_us));
        if (s_armed && togo < -(int64_t)(2u * TC1_MAX_ARM_TICKS))
        {
            SYS_CONSOLE_PRINT("[TRIG] STALLED: armed but the instant is %ld us gone\r\n",
                              (long)(-togo / (int64_t)s_ticks_per_us));
        }
    }
    if (st.late_n > 0u)
    {
        int32_t tpu = (int32_t)st.ticks_per_us;
        SYS_CONSOLE_PRINT("[TRIG] lateness over %lu fires, ticks (%ld ticks = 1 us):\r\n",
                          (unsigned long)st.late_n, (long)tpu);
        SYS_CONSOLE_PRINT("[TRIG]   last %ld (%ld ns)   min %ld (%ld ns)   max %ld (%ld ns)\r\n",
                          (long)st.late_last_ticks, (long)((int64_t)st.late_last_ticks * 1000 / tpu),
                          (long)st.late_min_ticks,  (long)((int64_t)st.late_min_ticks * 1000 / tpu),
                          (long)st.late_max_ticks,  (long)((int64_t)st.late_max_ticks * 1000 / tpu));
        SYS_CONSOLE_PRINT("[TRIG]   mean |late| %llu ticks (%llu ns)\r\n",
                          (unsigned long long)(st.late_sum_ticks / st.late_n),
                          (unsigned long long)(st.late_sum_ticks * 1000ULL / st.late_n / (uint64_t)tpu));
    }
    SYS_CONSOLE_PRINT("[TRIG] demo action 1 (isr) hits: %lu\r\n", (unsigned long)s_demo_isr_hits);
    /* The grid LED, and the three numbers answer three different
       questions: does the action fire at all (`hits`), is it allowed to
       write (`LED1`), and if not - how often has it been refused because
       of that (`busy`).  Without the third number, an LED that stays
       silent could not be told apart from an action that never fired. */
    SYS_CONSOLE_PRINT("[TRIG] action %u (grid LED1) hits: %lu   LED1: %s   busy: %lu\r\n",
                      (unsigned)TRIG_ACTION_LED, (unsigned long)s_gled_hits,
                      s_gled_own ? "held"
                                 : (s_gled_want ? "WANTED, held by someone else"
                                                : "not in use"),
                      (unsigned long)s_gled_busy);
    /* Where the LEVEL comes from, and how often it has already been
       produced with no grid reference.  The second number is the
       interesting one: as long as it stays still, the phase can be shown
       relative to the other boards; once it starts rising, it depends only
       on this board's own start instant. */
    SYS_CONSOLE_PRINT("[TRIG]   phase from: %s   fires without grid reference: %lu\r\n",
                      s_gled_synced ? "grid parity (synced)"
                                    : "LOCAL TOGGLE (no master reference)",
                      (unsigned long)s_gled_free);
    /* THE QUANTITY THAT DECIDES THE LEVEL - and the reason it is printed
     * here.
     *
     * On 2026-08-24, prediction and measurement disagreed: at a start
     * offset of one period, out-of-phase was computed, in-phase was
     * measured.  What I had read was the PIN, what I had reasoned about was
     * `s_gled_phase`, and in between sat two assumptions (claim held, level
     * not inverted).  So print the number itself, plus the last grid point
     * and the toggles SINCE THE LAST STOP - only that makes the computation
     * checkable on the device instead of in one's head. */
    /* PHASE AND DEBT, because otherwise "it's doing nothing" cannot be told
       apart from "it's currently pulling in slowly" - and because the
       phase MUST be 0 in normal operation: if it shows anything else, a
       demo has altered the operating state. */
    {
        int64_t debt = s_slew_debt;
        uint64_t mag = (uint64_t)((debt < 0) ? -debt : debt);

        SYS_CONSOLE_PRINT("[TRIG] grid phase: %llu ns   slew debt: %s%llu us"
                          "   rate period/%lu   runs %lu   done %lu\r\n",
                          (unsigned long long)s_phase_ns,
                          (debt < 0) ? "-" : "+",
                          (unsigned long long)(mag / 1000ull),
                          (unsigned long)s_slew_div,
                          (unsigned long)s_slew_runs, (unsigned long)s_slew_done);
        if (s_slew_start_debt != 0u)
        {
            SYS_CONSOLE_PRINT("[TRIG]   last start at %llu us of debt\r\n",
                              (unsigned long long)(s_slew_start_debt / 1000ull));
        }
    }
    SYS_CONSOLE_PRINT("[TRIG]   phase bit: %u   toggles since stop: %lu"
                      "   last grid point: %llu.%09llu s\r\n",
                      s_gled_phase ? 1u : 0u,
                      (unsigned long)s_gled_tog,
                      (unsigned long long)(s_gled_last_ns / 1000000000ull),
                      (unsigned long long)(s_gled_last_ns % 1000000000ull));
    return true;
}

void PTP_TRIG_CliInit(void)
{
    /* Two built-ins so both contexts are exercised without extra wiring:
       id 1 in ISR context, id 2 deferred to the main loop. */
    (void)PTP_TRIG_Register(1u, demo_isr, 0u, true);
    (void)PTP_TRIG_Register(2u, demo_task, 0u, false);
    /* Action 4: LED1 on the grid.  ISR context - the body is a single PORT
       write, so the LED edge dates the grid point instead of the main-loop
       pass.  Action 3 belongs to the raw-frame sender (`noip_test.c`),
       hence 4. */
    (void)PTP_TRIG_Register(TRIG_ACTION_LED, led_grid_action, 0u, true);
}
