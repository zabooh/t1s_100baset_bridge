/*******************************************************************************
  1PPS capture - implementation.  See pps_capture.h for what and why.
*******************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "definitions.h"
#include "config/default/system/console/sys_console.h"
#include "config/default/driver/lan865x/drv_lan865x.h"
#include "system/command/sys_command.h"
#include "pps_capture.h"
#include "board_pins.h"
#include "hw_shared.h"
#include "ptp_timebase.h"
#include "ptp_trigger.h"
#include "lan865x_diag.h"

#define PPS_IF          0u                  /* eth0, the 10BASE-T1S MAC-PHY */
#define MAC_TSL         0x00010074u         /* wall clock seconds [31:0]    */
#define MAC_TN          0x00010075u         /* wall clock nanoseconds       */



/* ------------------------------------------------------------------ Stage 1
 * REGISTER ACCESS SIGN-OFF.  Until 2026-08-20 these registers' addresses
 * stood here as hand offsets from a base address - four times in this
 * project one of them was wrong, and a wrong offset reads a neighbouring,
 * plausible-looking register.  A hex comparison does not catch that: it
 * shows THAT code changed, not whether the same address is meant.  Hence
 * every previously hand-written address, access width and bitmask is here
 * pinned against the DFP header at compile time.  If one deviates - wrong
 * symbol, different chip, new pack version - the build breaks instead of
 * the hardware.
 * Source of the expected values: component/eic.h, component/tc.h,
 * same54p20a.h.
 */
#define PPS_ADDR_OF(x)  ((uintptr_t)&(x))

_Static_assert(PPS_ADDR_OF(EIC_REGS->EIC_CTRLA)    == 0x40002800u, "EIC_CTRLA");
_Static_assert(PPS_ADDR_OF(EIC_REGS->EIC_SYNCBUSY) == 0x40002804u, "EIC_SYNCBUSY");
_Static_assert(PPS_ADDR_OF(EIC_REGS->EIC_EVCTRL)   == 0x40002808u, "EIC_EVCTRL");
_Static_assert(PPS_ADDR_OF(EIC_REGS->EIC_INTENCLR) == 0x4000280Cu, "EIC_INTENCLR");
_Static_assert(PPS_ADDR_OF(EIC_REGS->EIC_INTENSET) == 0x40002810u, "EIC_INTENSET");
_Static_assert(PPS_ADDR_OF(EIC_REGS->EIC_INTFLAG)  == 0x40002814u, "EIC_INTFLAG");
_Static_assert(PPS_ADDR_OF(EIC_REGS->EIC_ASYNCH)   == 0x40002818u, "EIC_ASYNCH");
_Static_assert(PPS_ADDR_OF(EIC_REGS->EIC_CONFIG[1])== 0x40002820u, "EIC_CONFIG1");
_Static_assert(sizeof(EIC_REGS->EIC_CTRLA)   == 1u, "EIC_CTRLA is 8 bit");
_Static_assert(sizeof(EIC_REGS->EIC_INTFLAG) == 4u, "EIC_INTFLAG is 32 bit");

_Static_assert(PPS_ADDR_OF(TC2_REGS->COUNT16.TC_CTRLA)    == 0x4101A000u, "TC2_CTRLA");
_Static_assert(PPS_ADDR_OF(TC2_REGS->COUNT16.TC_CTRLBSET) == 0x4101A005u, "TC2_CTRLBSET");
_Static_assert(PPS_ADDR_OF(TC2_REGS->COUNT16.TC_EVCTRL)   == 0x4101A006u, "TC2_EVCTRL");
_Static_assert(PPS_ADDR_OF(TC2_REGS->COUNT16.TC_INTFLAG)  == 0x4101A00Au, "TC2_INTFLAG");
_Static_assert(PPS_ADDR_OF(TC2_REGS->COUNT16.TC_STATUS)   == 0x4101A00Bu, "TC2_STATUS");
_Static_assert(PPS_ADDR_OF(TC2_REGS->COUNT16.TC_SYNCBUSY) == 0x4101A010u, "TC2_SYNCBUSY");
_Static_assert(PPS_ADDR_OF(TC2_REGS->COUNT16.TC_COUNT)    == 0x4101A014u, "TC2_COUNT16");
_Static_assert(PPS_ADDR_OF(TC2_REGS->COUNT16.TC_CC[0])    == 0x4101A01Cu, "TC2_CC0");
_Static_assert(PPS_ADDR_OF(TC2_REGS->COUNT16.TC_CCBUF[0]) == 0x4101A030u, "TC2_CCBUF0");
_Static_assert(sizeof(TC2_REGS->COUNT16.TC_CTRLBSET) == 1u, "TC2_CTRLBSET is 8 bit");
_Static_assert(sizeof(TC2_REGS->COUNT16.TC_EVCTRL)   == 2u, "TC2_EVCTRL is 16 bit");
_Static_assert(sizeof(TC2_REGS->COUNT16.TC_CC[0])    == 2u, "TC2_CC0 is 16 bit");

_Static_assert(EIC_CTRLA_ENABLE_Msk     == 0x02u,     "EIC ENABLE");
_Static_assert(EIC_SYNCBUSY_ENABLE_Msk  == 0x02u,     "EIC SYNCBUSY.ENABLE");
_Static_assert(TC_CTRLA_ENABLE_Msk      == 0x02u,     "TC ENABLE");
_Static_assert(TC_SYNCBUSY_ENABLE_Msk   == 0x02u,     "TC SYNCBUSY.ENABLE");
_Static_assert(TC_INTFLAG_MC0_Msk       == 0x10u,     "TC INTFLAG.MC0");
_Static_assert(TC_CTRLBSET_CMD_READSYNC == (4u << 5), "TC CMD READSYNC");
_Static_assert(MCLK_APBAMASK_EIC_Msk    == (1u << 10), "APBAMASK.EIC");
_Static_assert(EIC_GCLK_ID     == 4u,  "EIC GCLK channel");
_Static_assert(TC2_GCLK_ID     == 26u, "TC2 GCLK channel");
_Static_assert(EVSYS_GCLK_ID_0 == 11u, "EVSYS channel 0 GCLK index");
/* --------------------------------------------------------------------------- */

/* An edge whose second cannot be named without ambiguity is dropped rather than
   guessed.  The pulse marks a second boundary; if the main loop takes longer than
   this to get to it, the wall clock may already have rolled into the next second
   and the pair would be wrong by exactly one second - the kind of error that
   looks like a protocol bug. */
#define PPS_MAX_AGE_MS  200u

/* TC2 as the capture counter for stage B.  Base address and the APB/GCLK ids from
   the DFP header; the GCLK peripheral channel for TC2/TC3 is 26. */
/* MC0 is the only honest "a new value is in CC0" signal.  SYNCBUSY.CC0 answers a
   different question - whether a read-sync handshake is outstanding - and is
   clear both before and after a capture, which is why waiting on it alone still
   returned the previous second's value.  Offsets from the DFP header
   (component/tc.h): INTFLAG 0x0A, 8 bit, MC0 = bit 4. */
#define TC2_INTFLAG_MC0 TC_INTFLAG_MC0_Msk
/* AND MC0 IS STILL NOT THE RIGHT QUESTION.  Measured 2026-08-17: `MC0 timeouts 0`
   with `0 spins` on every single edge - the flag was always already set - and the
   reconstructed delta was nevertheless tens of thousands of ticks, so 2171 of 2208
   captures were discarded by the gate below (98.3 %).
 *
 * The datasheet says why, and it is a SECOND buffer level, not a timing problem
 * (SAM E54 48.7.2.8, TC STATUS bit 4): "For a capture channel x, the bit x is set
 * when a valid capture value is stored in the CCBUFx register. The bit x is
 * cleared automatically when the CCx register is read."  So the capture lands in
 * CCBUF0 and CC0 is the CPU's access point; CCBUFV0 is the handshake that says a
 * value has actually made it through, and reading CC0 clears it by itself.  MC0
 * says an interrupt-worthy event happened, which is a different statement.
 *
 * Offsets from the DFP header (component/tc.h): STATUS 0x0B, 8 bit,
 * CCBUFV0 = bit 4.  Grepped, not remembered - the comment on EIC_REGS->EIC_EVCTRL below is
 * what guessing an offset costs. */
#define TC2_STATUS_CCBUFV0 0x10u
/* Bounded, because a spin in an ISR must not be able to hang the board if the
   event path is broken.  The capture is latched at the edge and the handler runs
   ~575 ns later, so the flag is normally already set on the first look; 256 is
   several microseconds of headroom and still bounded. */
#define PPS_MC0_SPINS   256u
/* CCBUF0: the buffer a capture lands in first.  Offset from
   component/tc.h: TC_COUNT16_CCBUF_REG_OFST = 0x30.  For diagnostics only -
   the normal path keeps reading CC0. */

/* --- Diagnostics for the capture chain (RUNBOOK_PPS_CAPTURE.md 3) ----------
 * One record per edge, filled in the ISR only and never printed.  Only
 * comparing several edges settles anything, and that needs the raw values
 * SIDE BY SIDE - a single difference has turned out not to be interpretable
 * on its own. */
#define PPS_DIAG_N      8u
typedef struct
{
    uint16_t cc0;          /* CC0, read as in the normal path          */
    uint16_t ccbuf_pre;    /* CCBUF0 BEFORE reading CC0                */
    uint16_t ccbuf_post;   /* CCBUF0 afterwards                        */
    uint16_t ccbuf_wait;   /* CCBUF0 after plain waiting, WITHOUT READSYNC */
    uint16_t cc0_wait;     /* CC0 after the same wait                  */
    uint16_t wait_cyc;     /* how long was waited, in loops            */

    uint16_t count;        /* COUNT after READSYNC                     */
    uint8_t  status;       /* TC2.STATUS before reading (CCBUFV0 = 0x10) */
    uint8_t  intflag;      /* TC2.INTFLAG before clearing (MC0 = 0x10)  */
    uint8_t  chintflag;    /* EVSYS channel: EVD = bit 1, OVR = bit 0  */
    uint8_t  chstatus;     /* EVSYS channel: busy/ready                */
    uint16_t spins;        /* how long CCBUFV0 was waited on           */
} pps_diag_t;
static volatile pps_diag_t s_diag[PPS_DIAG_N];
static volatile uint32_t   s_diag_left;   /* edges still to record        */
static volatile uint32_t   s_diag_n;      /* recorded                     */

#define TC2_GCLK_CH     TC2_GCLK_ID   /* DFP: 26 */
#define PPS_EVSYS_CH    1u
#define EVSYS_CH0_GCLK_CH EVSYS_GCLK_ID_0   /* EVSYS_CHANNEL_n = PCHCTRL[11 + n] */
/* Ceiling for a believable capture-to-ISR delta.  The measured latency is about
   35 ticks (575 ns); 1000 ticks = 16.7 us leaves two decades of headroom and still
   rejects the ~31000 the broken readout produces. */
/* The bound for `now16 - cap`.  Used to be 1000 ticks (16.7 us), which fit a
 * reconstruction that reads right after the edge.  Since it waits for the
 * VALID value, that wait is now baked into the difference: measured at
 * 2141 ticks (35.7 us).  This does NOT hurt accuracy - `syst` and `now16`
 * are read immediately back to back and the difference is subtracted
 * exactly -, but the old bound discarded every valid value.  6000 ticks =
 * 100 us leaves headroom and stays an order of magnitude below the
 * 1.09 ms overflow.
 */
#define PPS_DELTA_SANE  6000u

#define TC_CMD_READSYNC TC_CTRLBSET_CMD_READSYNC
/* EVCTRL is 0x08.  I first wrote 0x20 from memory - which is CONFIG[1], the SENSE
   configuration for EXTINT 8..15 - so the "enable the event output" write silently
   corrupted EXTINT11's sense field instead, and no capture ever happened.  The
   symptom was a capture-to-ISR delta scattered across the whole 16-bit range:
   TC2 was counting, but CC0 never updated, so the difference was just the free
   counter minus a stale value.  Third time this project has been bitten by a
   guessed offset; the header is one grep away. */

static volatile uint64_t s_edge_tick;
static volatile bool     s_edge_pending;
static volatile uint32_t s_edges;
static uint32_t s_drops;
static uint32_t s_submitted;
/* Plausibility of the second number - the numbers for E28.  They live here,
   not in the time base, because only this module knows when the edge
   was. */
static bool     s_have_prev;
static uint32_t s_prev_sec;
static uint64_t s_prev_tick;
static uint32_t s_sec_jumps;      /* number did not fit the local spacing    */
static int32_t  s_sec_jump_last;  /* how many seconds it was off by          */
static uint32_t s_read_fail;
static bool     s_feed;
static bool     s_read_busy;
static uint64_t s_read_tick;            /* tick belonging to the read in flight */
static uint64_t s_ticks_per_ms;
/* TWO MARKER SLOTS, and the second is the more important one.
 *
 * The marker on PD10 (slot 1) requires `tbase pin off` AND a disarmed
 * trigger, because the grid drives the same pin.  So latency measurement
 * E38 measured a state that does not hold in the interesting case:
 * **without the TC1 interrupt, which runs every 100 us at 10 kHz** - i.e.
 * without the one serious competitor for the CPU.  The measured 680 ns
 * median / 1.68 us maximum therefore only hold for the unloaded case.
 *
 * On top of that comes the configuration: TC1 is explicitly set to NVIC
 * priority 0 in `ptp_trigger.c`, EXTINT12 gets none at all and keeps the
 * reset value - which is also 0.  **Same priority means no preemption**,
 * so a running TC1 ISR holds off the timestamp interrupt.
 *
 * Slot 2 puts the marker on PA16 (LED2) and is thereby independent of the
 * grid - the latency is measurable there WITH the trigger armed, and only
 * that answers the question.  Downside: PA16 is the LED, `tbase led` must
 * be off. */
#define PPS_MARK_OFF  0u
#define PPS_MARK_PD10 1u
#define PPS_MARK_LED  2u
static volatile uint8_t  s_isr_mark;      /* 0 off, 1 PD10, 2 PA16 (diagnostic) */
static volatile uint16_t s_edge_delta;   /* ticks from the capture to the ISR read */
static volatile uint16_t s_delta_min;
static volatile uint16_t s_delta_max;
static volatile uint32_t s_cap_used;     /* ticks taken from the capture   */
/* How often waiting for the VALID value was needed, and how long at most.
   Visible, so a change in timing behaviour stands out instead of showing
   up as a silent loss. */
static volatile uint32_t s_cap_notready;
static volatile uint16_t s_cap_wait_max;
/* The residual gap between the TC2 and the TC0 read, in CPU cycles (DWT,
   120 MHz -> 8.3 ns per cycle).  It is the only part of the reconstruction
   that still goes into the timestamp unfiltered; visible, so nobody takes
   it for zero. */
static volatile uint32_t s_gap_last;
static volatile uint32_t s_gap_min = 0xFFFFFFFFu;
static volatile uint32_t s_gap_max;


static volatile uint32_t s_cap_rejected; /* implausible -> fell back to entry */
static volatile uint32_t s_mc0_timeouts; /* MC0 never set - no capture arrived */
static volatile uint16_t s_mc0_spins;    /* how long the last wait took        */
static uint32_t s_last_sec;
static uint64_t s_last_gm_ns;

/* THE SPACING OF TWO EDGES - the only quantity that says whether the 1PPS
 * ITSELF has jumped.
 *
 * Measured 2026-08-17: the opening residual of an outlier series was
 * -1,070,518 ns, the sign meaning "local tick 1.07 ms TOO EARLY".  The only
 * input is the edge, and the second-number check below cannot see this: it
 * rounds the local spacing to whole seconds (`(dl_ms + 500) / 1000`) and is
 * therefore blind to anything under half a second.  One millisecond passes
 * through it silently.
 *
 * Hence this, in the ISR, from pure tick values: one subtraction and two
 * comparisons.  If the extremum stays at a few microseconds, the
 * millisecond does NOT come from the edge and the search continues further
 * inward; if it shows ~1 ms, the source is the PHY and the time base is
 * innocent. */
static volatile uint64_t s_prev_edge_tick;
static volatile bool     s_have_edge_prev;
static volatile int32_t  s_iv_last_tk;   /* deviation from 1 s, in ticks */
static volatile int32_t  s_iv_min_tk;
static volatile int32_t  s_iv_max_tk;
static volatile uint32_t s_iv_over_100us;  /* how often |deviation| > 100 us */
static volatile bool     s_iv_seen;

/* THE COUNTER-CLOCK: the same second spacing, but in CPU cycles from
 * DWT->CYCCNT.
 *
 * Brought in by the user (FIRMWARE_SELF_DEBUGGING.md 8), and it settles the
 * open question from E40.  There it is measured that one pair's tick was
 * short by exactly one 16-bit overflow (65536 ticks) - but the CONCLUSION
 * "SYS_TIME lost an overflow" has so far been an inference from the
 * magnitude.
 *
 * With two clocks that becomes a measurement: the DWT hangs off the same
 * PLL as GCLK1, so the ratio of the two second intervals is FIXED.  If
 * SYS_TIME loses an overflow, it reports an interval short by 65536 ticks,
 * while the DWT keeps counting its ~120 million cycles undeterred.  Exactly
 * that discrepancy is the proof - and its absence the refutation.
 *
 * One second at 120 MHz is 1.2e8 cycles and fits in a uint32 (max 4.29e9),
 * the counter does not wrap until 35.8 s - harmless for a 1 Hz interval. */
static volatile uint32_t s_prev_cyc;
static volatile uint32_t s_cyc_last;    /* cycles in the last second interval */
static volatile uint32_t s_cyc_min;
static volatile uint32_t s_cyc_max;

/* CORRECT THE LOST OVERFLOW AT THE POINT OF READING.
 *
 * Established (E41..E43, all measured): `SYS_TIME_Counter64Get()`
 * occasionally delivers a value short by **exactly 65536 ticks**, because
 * the TC0 callback arrives up to 1605 us late, leaving
 * `hwTimerPreviousValue` stale by more than one 16-bit overflow.  Ruled
 * out as the cause: the PHY's 1PPS (160 ns), ISR latency (1.6 us), the
 * PRIMASK window (4.1 us), the SPI register access (32 us), all eleven
 * task sections, the TCP/IP stack (locks no interrupts), the SPI driver,
 * the priorities, and our own callback in the ISR (7 us, 2 calls).  What
 * is left is the arithmetic in `SYS_TIME_HwTimerCompareUpdate()`.
 *
 * WHY HERE AND NOT THERE: patching the whole system's clock is the bigger
 * intervention, and a mistake in it hits everything.  Here the correction
 * hits exactly the path that needs accuracy - the time base's reference
 * pair.
 *
 * HOW: the DWT is the second clock, fixed ratio 2 (120 vs 60 MHz,
 * confirmed to 0.6 ppm against the measurements).  `s_k` is the running
 * offset of both time axes, updated ONLY when the two agree - so a wrong
 * reading cannot corrupt the reference.  If `syst` deviates downward by
 * about one overflow, it is corrected.
 *
 * AND THE CORRECTION IS THE PROOF: if `s_wrapfix` fires, the mechanism is
 * confirmed; if it never fires, the model is wrong - and that then shows
 * in the status line instead of going unnoticed. */
#define PPS_K_TOL_TK    4096u       /* 68 us: how close the clocks must be for a K update */
#define PPS_WRAP_TK     65536       /* TC0's 16-bit overflow                             */
#define PPS_WRAP_WIN    16384       /* window around -65536 in which correction applies  */
static volatile uint32_t s_wrapfix;      /* how often an overflow was made up  */
static volatile int64_t  s_wrapfix_err;  /* the deviation the last time         */
/* PRODUCE THE DEFECT ON COMMAND, instead of waiting for it.
 *
 * The fault occurs 1 to 4 times in ten minutes - so every check of a fix
 * cost ten minutes of waiting, and three rounds were needed because the
 * fix itself contained two bugs.  That is the wrong order: the defect is
 * sharply describable (one reading, short by exactly 65536 ticks), so it
 * can be produced.  That turns ten minutes of waiting into one second of
 * measuring - and a single observation into a regression test that runs
 * any time.
 *
 * Same lesson as with `tbase inject` for the outlier filter; drawn there
 * already and not applied here. */
static volatile uint32_t s_inject;      /* how many of the next edges to falsify */
/* SWITCH FOR THE FIX - so the test CAN fail.
 *
 * An acceptance run that can only confirm is not one: if the blocks stay
 * clean, there is no way to know whether the fix is working or the
 * injection is doing nothing at all.  With the switch both run in one
 * pass - fix on: no conspicuous blocks; fix off: the same injection must
 * produce some.  Only the second half shows the test has teeth. */
static volatile bool     s_fix_on = true;
static volatile int64_t  s_err_prev;    /* syst - dwt/2 of the previous edge */
static volatile bool     s_err_have;

/* WITHOUT `s_k`, and that is the real simplification.
 *
 * The second build tracked an offset `s_k` in the main loop (~30 kHz,
 * `err/64`) and checked the jump against it.  Time constant: 64 samples =
 * **2 ms**.  The event lasts 1.6 ms - so the filter absorbed exactly the
 * fault it was meant to make visible: at ~48 passes within the window
 * that is 1 - (63/64)^48 = **53 % absorption**, the jump shrinks to
 * ~-31,000 and falls out of the detection window.  Measured: `wrap fix 0`
 * with three actual events.
 *
 * And that same absorption is the explanation for the `min -31,072` the
 * clock comparison has been reporting for hours - the number was the
 * filter's fingerprint, not a puzzle in the instrument.
 *
 * What is correct is what the comment already said and the code did not
 * do: the reference is the PREVIOUS EDGE, unfiltered.  Between two edges
 * (1 s), `syst - dwt/2` drifts by 1.5 ppm x 60e6 = **90 ticks**, a lost
 * overflow is **65,536**.  Factor 700 - no model, no filter, no constant
 * needed. */
static uint64_t pps_fix_wrap(uint64_t syst)
{
    uint64_t d;
    int64_t  raw, jump;

    d = PTP_TRIG_Dwt64();
    if (d == 0u)
    {
        return syst;                 /* ohne Referenzuhr keine Korrektur */
    }
    raw = (int64_t)syst - (int64_t)(d / 2u);
    if (!s_err_have)
    {
        s_err_prev = raw;
        s_err_have = true;
        return syst;
    }
    jump = raw - s_err_prev;
    if (s_fix_on
        && jump < -(int64_t)(PPS_WRAP_TK - PPS_WRAP_WIN)
        && jump > -(int64_t)(PPS_WRAP_TK + PPS_WRAP_WIN))
    {
        s_wrapfix++;
        s_wrapfix_err = jump;
        /* THE REFERENCE STAYS RAW.  The first build set `raw + PPS_WRAP_TK`
           here - and because the next edge again delivers an UNCORRECTED
           `raw`, the same jump resulted again: the fix kept itself alive,
           on one board in 435 of 548 edges.  It became visible because
           `dropped (too late)` kept counting up digit by digit - a tick
           made too large lets `now - tick` overflow as an unsigned
           subtraction, and the edge counts as 200 ms too old.
           The loss is temporary (1.6 ms): the next edge reads normally
           again, raw against raw gives +65536, so no second correction. */
        s_err_prev = raw;
        return syst + (uint64_t)PPS_WRAP_TK;
    }
    s_err_prev = raw;
    return syst;
}

/* --- turning 1PPS on in the PHY -------------------------------------------- */
/* These two registers do NOT survive a reset, so every flash silently killed the
   source: "tbase pps on" switched the timebase over, the PHY stayed quiet, and
   the only evidence was "edges: 0" three screens down in "tbase pps".  On
   2026-08-13 that cost a measurement run - the boards sat in UNINIT while the
   A/B runner waited for LOCKED.  Enabling them from here removes the hand step.

   A4SEL = 01 puts 1PPS on DIOA4 (pin 23 -> R37 -> mikroBUS 13 -> PC12, see
   LAN8651_1PPS_HARDWARE.md); PPSEN starts the pulse train. */
#define PPS_PADCTRL      0x000A0088u
#define PPS_PADCTRL_MSK  0x00000300u
#define PPS_PADCTRL_VAL  0x00000100u
#define PPS_PPSCTL       0x000A0239u
#define PPS_PPSCTL_EN    0x00000001u
/* Two pulses' worth of patience before complaining: the first edge can be up to
   a second away simply because the pulse train is 1 Hz. */
#define PPS_ENABLE_GRACE_MS  2500u

static uint8_t  s_en_step;        /* 0 idle, 1 PADCTRL queued, 2 PPSCTL queued */
static uint64_t s_en_deadline;    /* 0 = not watching for a first edge          */
static uint32_t s_en_edges_at;    /* edge count when the enable was requested   */
static bool     s_en_complained;

/* --------------------------------------------------------------------------- */
/* the edge                                                                    */
/* --------------------------------------------------------------------------- */

void EIC_EXTINT_12_Handler(void)
{
    /* STAGE B: the tick is reconstructed from a hardware CAPTURE, not read on
     * entry.  TC2 free-runs on the same 60 MHz clock as SYS_TIME's TC0, and the
     * same EIC edge that raised this interrupt also latched TC2's counter into
     * CC0 - at the edge, with no CPU involved.
     *
     * So: read the clock now, read TC2 now, and subtract how far TC2 has moved
     * since the capture.  The interrupt latency drops out of the result entirely,
     * because it is contained in BOTH terms and cancels.  Stage A read the clock
     * on entry and therefore carried that latency: 2 to 5 us of it.
     *
     * What is left is the gap between the two reads below - a handful of
     * instructions, the same every time.  A constant bias is not a problem here:
     * it lands in the anchor as a fixed offset per board, which is D_const and a
     * calibration question (plan stage 2), not jitter. */
    uint64_t syst;
    uint16_t cap;
    uint16_t now16;

    /* Diagnostic marker, FIRST statement so it dates interrupt entry itself.
     * Against the 1PPS on the analyser this measures the true latency from edge
     * to handler, independently of any register handling here - which is what
     * the reconstructed 517 us delta needs checking against.  Uses PD10, so the
     * trigger must not be armed and "tbase pin off" must be set, or the PORT
     * event and this fight over the same pin. */
    if (s_isr_mark == PPS_MARK_PD10)
    {
        PTP_TRIG_PinMark();
    }
    else if (s_isr_mark == PPS_MARK_LED)
    {
        /* A single register write, so the marker dates the entry and not
           itself.  No function call, no pin table. */
        PORT_REGS->GROUP[BOARD_LED2_GROUP].PORT_OUTTGL = BOARD_LED2_MASK;
    }
    else
    {
        /* nichts */
    }

    /* WAIT FOR THE CAPTURE TO ARRIVE, not merely for the bus to be idle.
     *
     * This is the fix for the 31000-tick reconstruction.  The arithmetic gave it
     * away: 999.9440 ms x 60 MHz mod 65536 = 31200, i.e. exactly what you get by
     * subtracting a capture taken ONE SECOND EARLIER.  CC0 was being read 575 ns
     * after the edge, before the capture had propagated into the read domain, so
     * the read returned the previous pulse's value and the difference was the
     * whole second folded into 16 bits.
     *
     * SYNCBUSY.CC0, which is what this waited on before, cannot see that: it
     * reports whether a read-sync handshake is outstanding and is clear both
     * before and after a capture.  MC0 is set when a value is actually latched. */
    {
        uint32_t spins = 0u;
        /* WAIT ON CCBUFV0, NOT ON MC0.  MC0 was always already set and the value
           read out of CC0 was still stale - see the comment at TC2_REGS->COUNT16.TC_STATUS above.
           CCBUFV0 is the flag the datasheet ties to "a valid capture value is
           stored", and reading CC0 below clears it by itself, so the handshake is
           complete without a write. */
        /* DIAGNOSTIC: capture the state BEFORE reading - afterwards it is
           gone, because reading CC0 clears CCBUFV0 itself. */
        if (s_diag_left != 0u)
        {
            volatile pps_diag_t *d = &s_diag[s_diag_n % PPS_DIAG_N];
            d->status    = TC2_REGS->COUNT16.TC_STATUS;
            d->intflag   = TC2_REGS->COUNT16.TC_INTFLAG;
            d->ccbuf_pre = TC2_REGS->COUNT16.TC_CCBUF[0];
            d->chintflag = EVSYS_REGS->CHANNEL[PPS_EVSYS_CH].EVSYS_CHINTFLAG;
            d->chstatus  = EVSYS_REGS->CHANNEL[PPS_EVSYS_CH].EVSYS_CHSTATUS;
            /* PLAIN WAITING, no READSYNC, no other register touched.  If the
               value already changes from this, it is errata 2.20.1 (the
               done signal arrives too early), not a READSYNC issue. */
            {
                uint32_t w;
                for (w = 0u; w < 240u; w++) { __NOP(); }
                d->wait_cyc   = 240u;
                d->ccbuf_wait = TC2_REGS->COUNT16.TC_CCBUF[0];
                d->cc0_wait   = TC2_REGS->COUNT16.TC_CC[0];
            }

        }
        while ((TC2_REGS->COUNT16.TC_STATUS & TC2_STATUS_CCBUFV0) == 0u && spins < PPS_MC0_SPINS)
        {
            spins++;
        }
        if (spins >= PPS_MC0_SPINS)
        {
            s_mc0_timeouts++;   /* no capture: the guard below falls back to stage A */
        }
        s_mc0_spins = (uint16_t)spins;
    }
    while ((TC2_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_CC0_Msk) != 0u)
    {
    }
    /* WAIT ON THE VALUE, NOT ON THE FLAG.
     *
     * Measured 2026-08-19: CCBUFV0 is set and SYNCBUSY.CC0 is clear, and CC0
     * still delivers 0xFFFF - the value only appears about two microseconds
     * later.  This is errata 2.20.1 (the done signal arrives before the
     * register is valid again), which is why no flag can be the condition
     * here.  0xFFFF, on the other hand, is observable: it is the state of
     * a register that has not been updated yet.
     *
     * PRICE, named rather than hidden: a capture that genuinely lands on
     * 0xFFFF is discarded - one edge out of 65,536, at 1 Hz one in
     * 18 hours.  The fallback beneath it catches it (stage A), so it costs
     * accuracy for one second, not the time base.
     *
     * The bound is generous: 4000 loops at 120 MHz is a good 30 us, well
     * beyond the measured 2 us, but still two orders of magnitude below
     * TC2's 1.09 ms overflow, past which the reconstruction would become
     * ambiguous.
     */
    {
        uint16_t w = 0u;
        cap = TC2_REGS->COUNT16.TC_CC[0];
        while (cap == 0xFFFFu && w < 4000u)
        {
            w++;
            cap = TC2_REGS->COUNT16.TC_CC[0];
        }
        if (w != 0u)
        {
            s_cap_notready++;
            if (w > s_cap_wait_max) { s_cap_wait_max = w; }
        }
    }

    TC2_REGS->COUNT16.TC_INTFLAG = TC2_INTFLAG_MC0;      /* write-1-clear, ready for the next pulse */
    /* TC2 FIRST, THEN TC0 - and the two as close together as possible.
     *
     * Previously `syst` sat BEFORE the READSYNC and `now16` after it, so
     * the command and the wait loop sat between the two time axes.  The
     * formula below drops exactly that gap, and its variation was
     * therefore the timestamp jitter: the raw capture was reproducible to
     * 183 ns, and none of that reached the output (2026-08-19).
     *
     * What is left now is just the duration of one register read.  It is
     * measured (s_gap_*) rather than assumed. */
    TC2_REGS->COUNT16.TC_CTRLBSET = TC_CMD_READSYNC;
    while ((TC2_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_COUNT_Msk) != 0u)
    {
    }
    {
        uint32_t c0 = DWT->CYCCNT;
        now16 = TC2_REGS->COUNT16.TC_COUNT;
        syst  = SYS_TIME_Counter64Get();
        s_gap_last = DWT->CYCCNT - c0;
        if (s_gap_last < s_gap_min) { s_gap_min = s_gap_last; }
        if (s_gap_last > s_gap_max) { s_gap_max = s_gap_last; }
    }

    EIC_REGS->EIC_INTFLAG = (1u << BOARD_PPS_EXTINT);
    /* 16 bit at 60 MHz wraps every 1.09 ms; the handler answers within
       microseconds, so the masked difference is unambiguous. */
    if (s_diag_left != 0u)
    {
        volatile pps_diag_t *d = &s_diag[s_diag_n % PPS_DIAG_N];
        d->cc0        = cap;
        d->ccbuf_post = TC2_REGS->COUNT16.TC_CCBUF[0];
        d->count      = now16;
        d->spins      = s_mc0_spins;
        s_diag_n++;
        s_diag_left--;
    }
    s_edge_delta = (uint16_t)(now16 - cap);

    /* PLAUSIBILITY GATE - and the reason it exists is worth stating plainly.
     *
     * The true latency from the 1PPS edge to this handler was MEASURED against
     * the analyser: median 575 ns, i.e. about 35 ticks, with 950 ns of spread
     * (test_results.md E9).  The reconstruction above instead produces some
     * 31000 ticks, so it is wrong - the capture path delivers something, but not
     * what is being read out of it, and that is still open.
     *
     * Feeding the timebase from a computation known to be wrong is the one thing
     * that must not happen: a constant error would hide in the anchor, but the
     * observed values ranged over 386 us, and that would go straight into the
     * model as jitter.  So an implausible delta is DISCARDED and the tick falls
     * back to the entry read - stage A, which is measured and understood.
     *
     * The counter is the point as much as the guard: the day the capture starts
     * working, "capture used" begins to rise on its own and says so. */
    /* Produce the defect if requested - BEFORE the correction, so it sees
       exactly what SYS_TIME delivers in the fault case. */
    if (s_inject != 0u)
    {
        s_inject--;
        syst -= (uint64_t)PPS_WRAP_TK;
    }

    /* Make up the missing overflow BEFORE the tick is passed on - see
       pps_fix_wrap.  Both branches below derive from `syst`, so this runs
       once. */
    syst = pps_fix_wrap(syst);

    if (s_edge_delta <= PPS_DELTA_SANE)
    {
        s_edge_tick = syst - (uint64_t)s_edge_delta;
        s_cap_used++;
        if (s_edge_delta > s_delta_max) { s_delta_max = s_edge_delta; }
        if (s_edge_delta < s_delta_min || s_delta_min == 0u) { s_delta_min = s_edge_delta; }
    }
    else
    {
        s_edge_tick = syst;                 /* stage A: 575 ns bias, 950 ns jitter */
        s_cap_rejected++;
    }
    /* The counter-clock, right next to the SYS_TIME read: both quantities
       thereby date the same moment, and their difference cannot come from
       the gap between the two reads. */
    {
        uint32_t cyc = DWT->CYCCNT;
        if (s_have_edge_prev)
        {
            uint32_t dc = cyc - s_prev_cyc;   /* wraparound is correct here */
            s_cyc_last = dc;
            if (s_cyc_min == 0u || dc < s_cyc_min) { s_cyc_min = dc; }
            if (dc > s_cyc_max) { s_cyc_max = dc; }
        }
        s_prev_cyc = cyc;
    }

    /* Edge spacing against the target second - see the comment at
       s_iv_min_tk.  Pure tick arithmetic, no division: this runs in the
       ISR. */
    if (s_have_edge_prev && s_ticks_per_ms != 0u)
    {
        int64_t dev = (int64_t)(s_edge_tick - s_prev_edge_tick)
                    - (int64_t)((uint64_t)s_ticks_per_ms * 1000u);
        s_iv_last_tk = (int32_t)dev;
        if (!s_iv_seen)
        {
            s_iv_min_tk = (int32_t)dev;
            s_iv_max_tk = (int32_t)dev;
            s_iv_seen = true;
        }
        else
        {
            if (dev > (int64_t)s_iv_max_tk) { s_iv_max_tk = (int32_t)dev; }
            if (dev < (int64_t)s_iv_min_tk) { s_iv_min_tk = (int32_t)dev; }
        }
        /* 100 us in ticks, no division: ticks_per_ms / 10. */
        if (dev > (int64_t)(s_ticks_per_ms / 10u)
            || dev < -(int64_t)(s_ticks_per_ms / 10u))
        {
            s_iv_over_100us++;
        }
    }
    s_prev_edge_tick = s_edge_tick;
    s_have_edge_prev = true;

    s_edge_pending = true;
    s_edges++;
}

/* TC2 as a free-running capture counter, and the EIC edge routed into it.
 *
 * TC2 must run on the SAME clock as SYS_TIME's TC0 - GCLK1 at 60 MHz - or the
 * subtraction above would mix two rates.  GCLK peripheral channel 26 serves
 * TC2/TC3; that number is checked at run time by watching the counter move,
 * because a wrong channel leaves it standing still and every capture would then
 * read the same value. */
static void pps_tc2_capture_init(void)
{
    HW_ApbbClockEnable(MCLK_APBBMASK_TC2_Msk);
    GCLK_REGS->GCLK_PCHCTRL[TC2_GCLK_CH] = GCLK_PCHCTRL_GEN_GCLK1 | GCLK_PCHCTRL_CHEN_Msk;
    while ((GCLK_REGS->GCLK_PCHCTRL[TC2_GCLK_CH] & GCLK_PCHCTRL_CHEN_Msk) == 0u)
    {
    }

    TC2_REGS->COUNT16.TC_CTRLA = 0u;
    while ((TC2_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_ENABLE_Msk) != 0u) { }
    /* 16-bit, prescaler 1, capture on channel 0 */
    TC2_REGS->COUNT16.TC_CTRLA = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_CAPTEN0_Msk;
    TC2_REGS->COUNT16.TC_EVCTRL = TC_EVCTRL_TCEI_Msk | TC_EVCTRL_EVACT_STAMP;
    TC2_REGS->COUNT16.TC_CTRLA |= TC_CTRLA_ENABLE_Msk;                                  /* ENABLE */
    while ((TC2_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_ENABLE_Msk) != 0u) { }

    /* The EIC has to EMIT the event as well as the interrupt.  EVCTRL is
       enable-protected, so the whole instance goes down and up again for this -
       which is why it runs in `hw_shared.c`: that dance affects every other
       EXTINT, and a second copy of it in application code is a second place that
       has to get it right. */
    if (!HW_EicEventEnable(BOARD_PPS_EXTINT, true))
    {
        SYS_CONSOLE_PRINT("[PPS] EXTINT%u cannot emit an event -"
                          " not claimed.  The capture chain will NOT run.\r\n",
                          (unsigned)BOARD_PPS_EXTINT);
        return;
    }

    /* A RESYNCHRONIZED (or synchronous) EVSYS channel needs its OWN generic clock;
     * only the asynchronous path runs without one.  Leaving it out is silent: the
     * channel accepts the configuration, reads back correctly, and simply never
     * delivers.  That is exactly how this first failed - every register verified
     * fine while TC2_REGS->COUNT16.TC_CC[0] sat at its reset value of 0xFFFF.
     *
     * The peripheral channel index is not in the DFP header.  It follows from two
     * points measured on this device: the EIC is channel 4 (its configuration
     * works) and TC2/TC3 is 26 (TC2 counts), which matches the family table where
     * EVSYS_CHANNEL_n sits at 11 + n. */
    GCLK_REGS->GCLK_PCHCTRL[EVSYS_CH0_GCLK_CH + PPS_EVSYS_CH] =
        GCLK_PCHCTRL_GEN_GCLK1 | GCLK_PCHCTRL_CHEN_Msk;
    while ((GCLK_REGS->GCLK_PCHCTRL[EVSYS_CH0_GCLK_CH + PPS_EVSYS_CH]
            & GCLK_PCHCTRL_CHEN_Msk) == 0u)
    {
    }

    /* Channel 1 - channel 0 carries the trigger's TC1 -> PORT path.
     *
     * RESYNCHRONIZED, not asynchronous.  The paragraph above has argued for the
     * resynchronized path since this was written - and the code underneath it
     * said ASYNCHRONOUS, copied from the trigger's TC1 -> PORT channel where
     * async is right.  A TC capture user samples the event on the TC's own clock,
     * so on the asynchronous path the capture simply never happened: CC0 kept the
     * one value it had from bring-up, and the reconstruction subtracted a stale
     * number that drifted a whole second's worth modulo 65536 - the 31000 ticks
     * that stage B has been chasing.
     *
     * EDGSEL is not optional here and is the second half of the same bug: its
     * reset value is NO_EVT_OUTPUT, which on a resynchronized or synchronous path
     * means exactly what it says.  Switching the path without it would have
     * traded one silent failure for another. */
    EVSYS_REGS->CHANNEL[PPS_EVSYS_CH].EVSYS_CHANNEL =
        EVSYS_CHANNEL_EVGEN(EVENT_ID_GEN_EIC_EXTINT_12)
        | EVSYS_CHANNEL_PATH_RESYNCHRONIZED
        | EVSYS_CHANNEL_EDGSEL_RISING_EDGE;
    EVSYS_REGS->EVSYS_USER[EVENT_ID_USER_TC2_EVU] = PPS_EVSYS_CH + 1u;
}

static void pps_pin_and_eic_init(void)
{
    /* EIC bus clock.  Read-modify-write, interrupt-bracketed, in `hw_shared.c`:
       a bare write to APBAMASK would clear every other bit in it, TC0 among them
       - which is SYS_TIME, the very clock this module exists to improve. */
    HW_ApbaClockEnable(MCLK_APBAMASK_EIC_Msk);
    /* Generic clock for the EIC from generator 0.  `PCHCTRL[n]` is exclusive per
       channel (class A), so a full write is right here - an RMW would suggest a
       sharing that does not exist. */
    GCLK_REGS->GCLK_PCHCTRL[EIC_GCLK_ID] = GCLK_PCHCTRL_GEN_GCLK0 | GCLK_PCHCTRL_CHEN_Msk;
    while ((GCLK_REGS->GCLK_PCHCTRL[EIC_GCLK_ID] & GCLK_PCHCTRL_CHEN_Msk) == 0u)
    {
    }

    /* PC12 -> peripheral A, input buffer on.  `PMUX[n]` is one byte for TWO pins
       (here PC12 and PC13), so it is class B and goes through `hw_shared.c`. */
    HW_PinMux(BOARD_PPS_GROUP, BOARD_PPS_PIN, 0u /* peripheral A */, true);

    /* SENSE = RISE, and ASYNCH so a 640 ns pulse cannot fall between two clock
       edges.  `HW_EicClaim()` touches only this EXTINT's bits - the block that
       stood here until 2026-08-20 cleared EIC_INTENCLR and EIC_INTFLAG with
       0xFFFFFFFF, which is correct only as long as nobody else uses the EIC and
       becomes wrong without a line of code changing.  A refusal means the EXTINT
       is already claimed, i.e. a programming error, and it is loud on purpose. */
    if (!HW_EicClaim(BOARD_PPS_EXTINT, 1u /* RISE */, true))
    {
        SYS_CONSOLE_PRINT("[PPS] EXTINT%u could not be claimed -"
                          " already taken (mask 0x%08lX) or not allowed."
                          "  1PPS capture will NOT run.\r\n",
                          (unsigned)BOARD_PPS_EXTINT,
                          (unsigned long)HW_EicClaimed());
        return;
    }

    /* EXPLICITLY the highest priority - previously there was no assignment
       here, leaving the reset value, which happens to also be 0.  An
       implicitly correct value is not a deliberately set one: it sat level
       with TC1, and same priority means no preemption, so a running TC1
       ISR held off the timestamp.  The ordering and its measurement are in
       ptp_trigger.c at NVIC_SetPriority. */
    NVIC_SetPriority(EIC_EXTINT_12_IRQn, 0);
    NVIC_ClearPendingIRQ(EIC_EXTINT_12_IRQn);
    NVIC_EnableIRQ(EIC_EXTINT_12_IRQn);
}

/* --------------------------------------------------------------------------- */
/* naming the second                                                           */
/* --------------------------------------------------------------------------- */

static void pps_reg_cb(void *r1, bool success, uint32_t addr, uint32_t value,
                       void *tag, void *r2)
{
    (void)r1; (void)addr; (void)tag; (void)r2;
    s_read_busy = false;
    if (!success)
    {
        s_read_fail++;
        return;
    }
    /* CHECK THE SECOND NUMBER AGAINST THE PREVIOUS ONE before feeding it in.
     *
     * MEASURED 2026-08-16 (E28): the trigger had 8 outages of 999 ms each
     * on one board and 2 on the other, and `skipped periods` stood at
     * 70,003 - SEVEN TIMES EXACTLY 10,000 periods at a 100 us grid, i.e.
     * exactly one second.  A quantity off by exactly one second can only be
     * the second number.
     *
     * The cause is the timing of the read: the edge marks the start of the
     * second, `MAC_TSL` is read afterwards over SPI, and if that read slips
     * past the next boundary, the value belongs to the FOLLOWING second.
     * `s_late_drops` catches the case where the main loop was too slow -
     * but not the case where it was fast and the SPI reply came back late.
     *
     * Two edges are one second apart, so the number must grow by exactly 1.
     * If it does not, either an edge was lost (then the jump is a multiple
     * and the number is still correct) or the read landed wrong (then it
     * is not).  This is distinguishable by the LOCAL spacing: it says how
     * many seconds have really passed.
     *
     * DISCARDING rather than correcting is deliberate here.  A correction
     * would have to guess which of the two quantities is wrong; a
     * discarded pair costs one second of tracking, and the time base
     * handles that without trouble (holdover only kicks in after 3 s). */
    if (s_have_prev && s_ticks_per_ms != 0u)
    {
        uint64_t dl_ms = (s_read_tick > s_prev_tick)
                       ? ((s_read_tick - s_prev_tick) / s_ticks_per_ms) : 0u;
        /* How many seconds have passed locally, rounded to the nearest whole one? */
        uint32_t want = (uint32_t)((dl_ms + 500u) / 1000u);
        uint32_t got  = (value > s_prev_sec) ? (value - s_prev_sec) : 0u;

        if (want != 0u && got != want)
        {
            s_sec_jumps++;
            s_sec_jump_last = (int32_t)got - (int32_t)want;
            s_prev_sec  = value;         /* trotzdem mitfuehren, sonst reisst die */
            s_prev_tick = s_read_tick;   /* naechste Pruefung auch                */
            return;                      /* NICHT einspeisen                      */
        }
    }
    s_prev_sec  = value;
    s_prev_tick = s_read_tick;
    s_have_prev = true;

    s_last_sec = value;
    /* The pulse marks the START of the second the wall clock is now in, so the
       instant it dates is exactly value seconds, nanoseconds zero. */
    s_last_gm_ns = (uint64_t)value * 1000000000ULL;
    PTP_TB_SubmitPairFrom(PTP_TB_SRC_PPS, s_read_tick, s_last_gm_ns);
    s_submitted++;
}

/* Drives the two enable writes and then watches for the first edge.  Register
   access is asynchronous and single-slot (see CLAUDE.md 3.3), so this walks one
   step per call instead of blocking - the same reason the diag module exists. */
static void pps_enable_tasks(void)
{
    uint64_t now;

    if (s_en_step != 0u && !LAN865X_DIAG_Busy())
    {
        if (s_en_step == 1u)
        {
            if (LAN865X_DIAG_Rmw(PPS_PADCTRL, PPS_PADCTRL_MSK, PPS_PADCTRL_VAL))
            {
                s_en_step = 2u;
            }
        }
        else if (s_en_step == 2u)
        {
            if (LAN865X_DIAG_Write(PPS_PPSCTL, PPS_PPSCTL_EN))
            {
                s_en_step = 0u;
                s_en_deadline = SYS_TIME_Counter64Get()
                              + (PPS_ENABLE_GRACE_MS * s_ticks_per_ms);
            }
        }
    }

    /* Deliberately NOT falling back to PTP here.  A source that quietly repairs
       itself is a source whose failure nobody ever fixes; stage 0 already makes
       the failure safe, because a starving timebase stops the trigger instead of
       letting the boards drift apart.  So: say it, loudly, once. */
    if (s_en_deadline != 0u)
    {
        now = SYS_TIME_Counter64Get();
        if (s_edges != s_en_edges_at)
        {
            s_en_deadline = 0u;          /* it lives */
        }
        else if (now >= s_en_deadline && !s_en_complained)
        {
            s_en_complained = true;
            s_en_deadline = 0u;
            SYS_CONSOLE_PRINT(
                "[PPS] NO EDGE %u ms after enabling 1PPS - the timebase has no"
                " source and the trigger will stop.\r\n"
                "[PPS] Check the hardware modification (R37 / mikroBUS 13 ->"
                " PC12, LAN8651_1PPS_HARDWARE.md), then 'tbase pps'.\r\n",
                (unsigned)PPS_ENABLE_GRACE_MS);
        }
    }
}

void PPS_Tasks(void)
{
    uint64_t tick;
    uint64_t now;

    /* THE GOOD SOURCE IS THE DEFAULT, not a command someone has to know
     * about.
     *
     * WHY THIS IS NEEDED: `PADCTRL.A4SEL` and `PPSCTL.PPSEN` in the
     * LAN8651 are NOT persistent.  So after every reset the 1PPS was off
     * and the time base ran on PTP - which is the distinctly worse mode:
     * measured 2026-08-19, sigma 21 us against 2 us, `winner spread`
     * 39..56 us against 1.5 us, and there the offset drifts away as a
     * SAWTOOTH until it wraps past half a grid period (E52).  Whoever just
     * typed `trigper` after a reset therefore inevitably ended up in the
     * bad mode, with nothing pointing to it.
     *
     * WHY HERE AND NOT IN PPS_Initialize(): `PPS_FeedSet()` calls
     * `PTP_TB_SourceSet()`, and whether the time base is already
     * initialized at that point depends on the ordering in `app.c`.  By the
     * first Tasks pass everything is up; the two register writes are done
     * by `pps_enable_tasks()` afterwards, one per pass anyway.
     *
     * NO FALLBACK TO PTP if no edge arrives - that is the 2026-08-13
     * decision and it stays: a source that silently repairs itself is one
     * whose defect never gets fixed.  If the soldering change (R37/R40) is
     * missing, the module loudly reports after 2.5 s that no edge is
     * present - and that is meant to stand out.
     * Switching remains possible at any time: `tbase pps off`. */
    {
        static bool s_autostart_done = false;
        if (!s_autostart_done)
        {
            s_autostart_done = true;
            PPS_FeedSet(true);
        }
    }

    pps_enable_tasks();

    if (!s_edge_pending || s_read_busy)
    {
        return;
    }

    /* Take the edge out of the ISR's hands atomically - a new pulse a second from
       now must not overwrite the one being processed. */
    NVIC_DisableIRQ(EIC_EXTINT_12_IRQn);
    tick = s_edge_tick;
    s_edge_pending = false;
    NVIC_EnableIRQ(EIC_EXTINT_12_IRQn);

    now = SYS_TIME_Counter64Get();
    /* UNSIGNED SUBTRACTION WITH AN ORDERING CHECK.  If `tick` lies before
       `now`, the edge is young and not 292 years old - exactly what the
       first build of the wrap fix produced: a tick made too large by
       65536 ticks let `now - tick` overflow, and 435 of 548 edges counted
       as 200 ms too old.  The trigger is fixed, the guard stays: an age
       check that calls a tick from the future "ancient" is a trap. */
    if (now > tick && (now - tick) > (PPS_MAX_AGE_MS * s_ticks_per_ms))
    {
        s_drops++;                      /* too late to name the second safely */
        return;
    }
    if (!s_feed)
    {
        return;                         /* edges are counted, but nothing is fed */
    }

    s_read_tick = tick;
    s_read_busy = true;
    if (DRV_LAN865X_ReadRegister(PPS_IF, MAC_TSL, true, pps_reg_cb, NULL)
        != TCPIP_MAC_RES_OK)
    {
        s_read_busy = false;
        s_read_fail++;
    }
}

/* --------------------------------------------------------------------------- */
/* public                                                                      */
/* --------------------------------------------------------------------------- */

void PPS_FeedSet(bool on)
{
    s_feed = on;
    PTP_TB_SourceSet(on ? PTP_TB_SRC_PPS : PTP_TB_SRC_PTP);
    if (on)
    {
        /* Ask the PHY for pulses whether or not somebody remembered to.  Idempotent
           - writing an already-set bit costs two SPI transactions and nothing else. */
        s_en_step = 1u;
        s_en_edges_at = s_edges;
        s_en_complained = false;
        s_en_deadline = 0u;             /* set once the second write is away */
    }
}

bool PPS_FeedGet(void)          { return s_feed; }
uint32_t PPS_EdgeCount(void)    { return s_edges; }
uint32_t PPS_DropCount(void)    { return s_drops; }

/* The last second edge, as a (tick, number) pair - for peer_capture.c,
 * which computes the own grid's phase against the own second from it
 * (stage 0, PEER_CAPTURE_PLAN.md 9).  The number is part of the answer, not
 * decoration: without it the caller cannot tell a NEW edge from re-reading
 * the same old one and would put the same value into its statistics more
 * than once.
 *
 * Under PRIMASK, because the tick and the number only mean something
 * together and the ISR writes both - the same reasoning as at
 * PTP_TRIG_GridRef(). */
bool PPS_LastEdge(uint64_t *tick, uint32_t *seq)
{
    bool ok;
    uint32_t prim = __get_PRIMASK();

    __disable_irq();
    ok = (s_edges != 0u);
    if (ok)
    {
        *tick = s_edge_tick;
        *seq = s_edges;
    }
    if (prim == 0u)
    {
        __enable_irq();
    }
    return ok;
}

/* Hung off the "tbase" group rather than its own: MAX_CMD_GROUP in the generated
   sys_command.h is 8 and the project is at the limit, so a ninth SYS_CMD_ADDGRP
   would fail - quietly, from the caller's point of view.  Same reason the trigger
   commands live under tbase. */
bool PPS_CliTry(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "pps") != 0)
    {
        return false;
    }

    if (argc >= 4 && !strcmp(argv[2], "fix"))
    {
        s_fix_on = (strcmp(argv[3], "off") != 0);
        SYS_CONSOLE_PRINT("[PPS] wrap correction: %s%s\r\n",
                          s_fix_on ? "on" : "OFF",
                          s_fix_on ? "" : "  (the defect passes through - for the"
                                          " teeth half of the acceptance run)");
        return true;
    }

    if (argc >= 3 && !strcmp(argv[2], "inject"))
    {
        /* Produce the defect instead of waiting for it: the next n edges
           read short by exactly one 16-bit overflow - exactly what
           SYS_TIME delivers in the fault case.  This turns ten minutes of
           waiting into one second of measuring, and the fix is checkable
           at any time. */
        unsigned n = (argc >= 4) ? (unsigned)strtoul(argv[3], NULL, 0) : 1u;
        s_inject = (uint32_t)n;
        SYS_CONSOLE_PRINT("[PPS] injecting a lost wrap (-%d ticks) into the next"
                          " %u edge(s) - 'wrap fix' must rise by %u and"
                          " 'outliers' must stay put\r\n",
                          (int)PPS_WRAP_TK, n, n);
        return true;
    }

    if (argc >= 4 && !strcmp(argv[2], "mark"))
    {
        if (!strcmp(argv[3], "led"))
        {
            /* Richtung erst hier setzen, damit die LED im Normalbetrieb
               unangetastet bleibt.  OUTSET vor DIRSET: sonst blitzt sie beim
               Umschalten (dieselbe Reihenfolge wie im BSP-Rezept). */
            PORT_REGS->GROUP[BOARD_LED2_GROUP].PORT_OUTSET = BOARD_LED2_MASK;
            PORT_REGS->GROUP[BOARD_LED2_GROUP].PORT_DIRSET = BOARD_LED2_MASK;
            s_isr_mark = PPS_MARK_LED;
        }
        else if (!strcmp(argv[3], "on"))
        {
            s_isr_mark = PPS_MARK_PD10;
        }
        else
        {
            s_isr_mark = PPS_MARK_OFF;
        }
        SYS_CONSOLE_PRINT("[PPS] ISR pin marker: %s\r\n",
                          (s_isr_mark == PPS_MARK_PD10)
                              ? "PD10  (needs 'tbase pin off' and no armed trigger)"
                          : (s_isr_mark == PPS_MARK_LED)
                              ? "PA16/LED2  (works WITH an armed trigger; 'tbase led' must be off)"
                              : "off");
        return true;
    }

    /* `tbase pps eic` - who holds which EXTINT, and how often a claim was
       refused.  `tbase pps eic claim <n>` claims EXTINT n as a probe.
       This is the sign-off for stage 3 of RUNBOOK_REGISTERZUGRIFF: the old
       setup block would have switched off the first user with the second,
       because it reset the instance and cleared INTENCLR/INTFLAG with
       0xFFFFFFFF.  After the claim, `tbase pps` must still be counting
       edges.

       IT IS A REAL INTERVENTION, not a dry run: the instance briefly goes
       off and back on, and the channel stays claimed afterwards (there is
       deliberately no release - an EXTINT is handed out at setup, not
       returned during operation).  EXTINT n's pin is not muxed in the
       process, so nothing arrives.  Reset the board after the test. */
    if (argc >= 3 && !strcmp(argv[2], "eic"))
    {
        if (argc >= 4 && !strcmp(argv[3], "claim"))
        {
            unsigned long n = (argc >= 5) ? strtoul(argv[4], NULL, 0) : 99u;
            bool ok = HW_EicClaim((uint8_t)n, 1u /* RISE */, false);
            SYS_CONSOLE_PRINT("[EIC] claim EXTINT%lu: %s\r\n", n,
                              ok ? "accepted" : "REFUSED");
        }
        SYS_CONSOLE_PRINT("[EIC] claimed: 0x%08lX   refused: %lu\r\n",
                          (unsigned long)HW_EicClaimed(),
                          (unsigned long)HW_EicRefused());
        return true;
    }

    if (argc >= 3 && !strcmp(argv[2], "diag"))
        {
            uint32_t i;
            uint32_t want = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 0) : 5u;
            if (want == 0u || want > PPS_DIAG_N) { want = PPS_DIAG_N; }
            if (s_diag_n != 0u || s_diag_left != 0u)
            {
                /* Print what is there first - a second call is the pickup. */
                SYS_CONSOLE_PRINT("[DIAG] configuration, read back:\r\n");
                SYS_CONSOLE_PRINT("[DIAG]   TC2.CTRLA  %08lx  (CAPTEN0=Bit24, MODE=Bit3:2, ENABLE=Bit1)\r\n",
                                  (unsigned long)TC2_REGS->COUNT16.TC_CTRLA);
                SYS_CONSOLE_PRINT("[DIAG]   TC2.EVCTRL %04x      (TCEI=Bit5, EVACT=Bit2:0)\r\n",
                                  (unsigned)TC2_REGS->COUNT16.TC_EVCTRL);
                SYS_CONSOLE_PRINT("[DIAG]   EVSYS.CHANNEL[%u] %08lx   EVSYS_USER[TC2_EVU] %lu\r\n",
                                  (unsigned)PPS_EVSYS_CH,
                                  (unsigned long)EVSYS_REGS->CHANNEL[PPS_EVSYS_CH].EVSYS_CHANNEL,
                                  (unsigned long)EVSYS_REGS->EVSYS_USER[EVENT_ID_USER_TC2_EVU]);
                SYS_CONSOLE_PRINT("[DIAG]   EIC.EVCTRL %08lx   EIC.ASYNCH %08lx   EIC.CONFIG1 %08lx\r\n",
                                  (unsigned long)EIC_REGS->EIC_EVCTRL,
                                  (unsigned long)EIC_REGS->EIC_ASYNCH,
                                  (unsigned long)EIC_REGS->EIC_CONFIG[1]);
                SYS_CONSOLE_PRINT("[DIAG] n  cc0  ccbuf_pre | after waiting: ccbuf cc0 | after READSYNC: ccbuf  d(ccbuf)  count  sts int\r\n");
                for (i = 0u; i < s_diag_n && i < PPS_DIAG_N; i++)
                {
                    volatile pps_diag_t *d = &s_diag[i];
                    /* d(cc0) is the number that actually matters: it MUST be
                       constant and equal to the period mod 65536 (target
                       value from 2). */
                    uint16_t dcc = (i > 0u) ? (uint16_t)(d->ccbuf_post - s_diag[i - 1u].ccbuf_post) : 0u;
                    SYS_CONSOLE_PRINT("[DIAG] %lu  %5u  %5u | %5u %5u | %5u  %6u  %5u  %02x %02x\r\n",
                                      (unsigned long)i, (unsigned)d->cc0, (unsigned)d->ccbuf_pre,
                                      (unsigned)d->ccbuf_wait, (unsigned)d->cc0_wait,
                                      (unsigned)d->ccbuf_post, (unsigned)dcc,
                                      (unsigned)d->count,
                                      (unsigned)d->status, (unsigned)d->intflag);
                }
                SYS_CONSOLE_PRINT("[DIAG] still pending: %lu edges\r\n", (unsigned long)s_diag_left);
            }
            s_diag_n = 0u;
            s_diag_left = want;
            SYS_CONSOLE_PRINT("[DIAG] armed for %lu edges - call again in %lu s\r\n",
                              (unsigned long)want, (unsigned long)want + 1u);
            /* `true` = command handled.  A bare `return;` used to sit here in
               a bool function, i.e. undefined behaviour: the caller got an
               indeterminate value, and `tbase pps diag N` could count as
               NOT handled depending on register contents.  Found
               2026-08-20 while building stage 3 (compiler warning), the
               cause is older. */
            return true;
        }

    if (argc >= 3 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off")))
    {
        PPS_FeedSet(!strcmp(argv[2], "on"));
        SYS_CONSOLE_PRINT("[PPS] feeding the timebase: %s  (model cleared)\r\n",
                          s_feed ? "on" : "off");
        return true;
    }
    if (argc >= 3)
    {
        SYS_CONSOLE_PRINT("[PPS] usage: tbase pps | tbase pps on|off\r\n");
        return true;
    }

    SYS_CONSOLE_PRINT("[PPS] edges: %u   dropped (too late): %u   submitted: %u"
                      "   read fails: %u\r\n",
                      (unsigned)s_edges, (unsigned)s_drops,
                      (unsigned)s_submitted, (unsigned)s_read_fail);
    SYS_CONSOLE_PRINT("[PPS] feeding: %s   timebase source: %s\r\n",
                      s_feed ? "on" : "off",
                      (PTP_TB_SourceGet() == PTP_TB_SRC_PPS) ? "1PPS" : "PTP pairs");
    /* This line is the instrumentation for E28 and the only place where a
       second number wrong by one second becomes visible at all - before it
       ran silently into the time base and cost 10,000 periods there. */
    SYS_CONSOLE_PRINT("[PPS] second-number mismatches: %u   last off by %d s\r\n",
                      (unsigned)s_sec_jumps, (int)s_sec_jump_last);
    SYS_CONSOLE_PRINT("[PPS] capture used: %u   rejected as implausible: %u"
                      "   (limit %u ticks)   not ready: %u (max %u loops)\r\n",
                      (unsigned)s_cap_used, (unsigned)s_cap_rejected,
                      (unsigned)PPS_DELTA_SANE,
                      (unsigned)s_cap_notready, (unsigned)s_cap_wait_max);
    SYS_CONSOLE_PRINT("[PPS] TC2->TC0 read gap: last %lu   min %lu   max %lu cycles"
                      "   (120 MHz -> 8.3 ns per cycle; only the SPREAD matters)\r\n",
                      (unsigned long)s_gap_last,
                      (unsigned long)((s_gap_min == 0xFFFFFFFFu) ? 0u : s_gap_min),
                      (unsigned long)s_gap_max);
    SYS_CONSOLE_PRINT("[PPS] MC0: timeouts %u   last wait %u spins"
                      "   (0 spins = the capture was already there)\r\n",
                      (unsigned)s_mc0_timeouts, (unsigned)s_mc0_spins);
    SYS_CONSOLE_PRINT("[PPS] delta: last %u  min %u  max %u ticks"
                      "  (60 = 1 us; measured true latency is ~35)\r\n",
                      (unsigned)s_edge_delta, (unsigned)s_delta_min,
                      (unsigned)s_delta_max);
    /* THE LINE THAT CHECKS THE INPUT.  Everything above describes how the
       edge was processed; this says whether it ARRIVED at the right time.
       The second-number check rounds to whole seconds and is blind to one
       millisecond - hence this one. */
    {
        int32_t iv_lo = s_iv_min_tk, iv_hi = s_iv_max_tk;
        uint32_t tpm = (s_ticks_per_ms != 0u) ? s_ticks_per_ms : 1u;
        SYS_CONSOLE_PRINT("[PPS] edge interval vs 1 s: last %+ld  min %+ld"
                          "  max %+ld ticks  (= %+ld .. %+ld us)"
                          "   over 100 us: %u\r\n",
                          (long)s_iv_last_tk, (long)iv_lo, (long)iv_hi,
                          (long)(iv_lo * 1000 / (int32_t)tpm),
                          (long)(iv_hi * 1000 / (int32_t)tpm),
                          (unsigned)s_iv_over_100us);
    }
    /* THE COUNTER-CLOCK.  The same second, counted independently.  If the
       SYS_TIME line above deviates by ~65536 ticks while this one stays
       put, SYS_TIME has lost an overflow - and that is then measured, not
       inferred. */
    SYS_CONSOLE_PRINT("[PPS] same second in CPU cycles (DWT): last %lu  min %lu"
                      "  max %lu   spread %lu cycles   DWT %s\r\n",
                      (unsigned long)s_cyc_last, (unsigned long)s_cyc_min,
                      (unsigned long)s_cyc_max,
                      (unsigned long)((s_cyc_max > s_cyc_min)
                                      ? (s_cyc_max - s_cyc_min) : 0u),
                      PTP_TRIG_DwtOk() ? "running" : "NOT RUNNING - values void");
    /* THE FIX IS THE PROOF.  If a number > 0 shows here, the lost overflow
       has been caught at the point of reading - confirming the mechanism
       from E41..E43.  If it stays 0 while `outliers` keeps rising, the
       model is wrong, and it shows here instead of going unnoticed. */
    SYS_CONSOLE_PRINT("[PPS] wrap fix: %u corrections   last error %+lld ticks"
                      "   (jump vs previous edge: drift is ~90, a wrap is 65536)\r\n",
                      (unsigned)s_wrapfix, (long long)s_wrapfix_err);
    SYS_CONSOLE_PRINT("[PPS] last second: %u   -> %llu ns\r\n",
                      (unsigned)s_last_sec, (unsigned long long)s_last_gm_ns);
    if (s_edges == 0u)
    {
        /* "tbase pps on" enables the PHY itself now, so reaching this line means
           the PHY was asked and stayed quiet - that points at the wire, not at a
           forgotten register.  The manual pair is kept for bring-up on a board
           whose driver is not up yet. */
        SYS_CONSOLE_PRINT("[PPS] no edge yet.  'tbase pps on' enables the PHY;"
                          " if it is on and still nothing arrives, suspect the\r\n"
                          "[PPS] hardware mod (R37 / mikroBUS 13 -> PC12).  By hand:"
                          " lan_rmw 0x000A0088 0x0300 0x0100 ;"
                          " lan_write 0x000A0239 0x0001\r\n");
    }
    return true;
}

void PPS_Initialize(void)
{
    s_ticks_per_ms = (uint64_t)SYS_TIME_FrequencyGet() / 1000u;
    if (s_ticks_per_ms == 0u)
    {
        s_ticks_per_ms = 1u;
    }
    s_feed = false;
    s_en_step = 0u;
    s_en_deadline = 0u;
    s_en_complained = false;
    pps_pin_and_eic_init();
    pps_tc2_capture_init();
}
