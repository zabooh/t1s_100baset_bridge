/*******************************************************************************
  Button press, dated on the master's time - implementation

  File Name:
    button.c

  Summary:
    SW1 (PD00, EXTINT0) and SW2 (PD01, EXTINT1): date the edge in the
    interrupt, convert to grandmaster time in the main loop, and report it.

  Description:
    The reasoning behind the design is in button.h.  Here are the decisions
    that otherwise do not show in the code.
*******************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "definitions.h"
#include "config/default/system/console/sys_console.h"

#include "button.h"
#include "board_pins.h"
#include "hw_shared.h"
#include "ptp_timebase.h"
#include "trig_cmd.h"

/* --------------------------------------------------------------- addresses
   Same safeguard as in the neighbouring modules: one assertion per register
   that checks whether the SAME ADDRESS is meant.  A hex comparison
   fundamentally cannot do that (FALLSTRICKE_MCU.md, 2026-08-20). */
#define BTN_ADDR_OF(x)          ((uintptr_t)&(x))
_Static_assert(BTN_ADDR_OF(EIC_REGS->EIC_INTFLAG) == 0x40002814u, "EIC_INTFLAG");
_Static_assert(sizeof(EIC_REGS->EIC_INTFLAG) == 4u, "EIC_INTFLAG is 32 bit");

/* How long after an accepted press every further edge of the same button is
 * discarded.
 *
 * 50 ms: comfortably above a mechanical button's bounce (order of magnitude
 * 5..20 ms) and well below what a human means as two separate presses.
 * Changeable at runtime, because the right value depends on the button, not
 * on the code - and because a value that can only be checked by rebuilding
 * does not get checked. */
#define BTN_DEBOUNCE_MS_DEF     50u

/* Interrupt priority of the two button ISRs.
 *
 * NOT 0 and NOT 1, and that is a measured reason: the ordering in
 * ptp_trigger.c puts EXTINT12 (the timestamp) at 0, TC0 (SYS_TIME ITSELF) at
 * 1, and TC1 (the follow-up) at 2.  If TC0's overflow handler is held off
 * longer than one wrap period, SYS_TIME permanently loses an overflow - the
 * defect from E39, 1.0710 ms +- 0.6 us.  A button ISR is short, but it must
 * fundamentally never be able to preempt the clock.
 *
 * 3 therefore means: below the clock and the trigger, above the stack.
 * Above the stack, because a GMAC burst would otherwise shift the timestamp
 * by its duration - irrelevant for a button, but it costs nothing to have
 * it right. */
#define BTN_IRQ_PRIO            3u

typedef struct
{
    /* --- written by the ISR --- */
    volatile uint64_t tick;        /* SYS_TIME tick of the edge               */
    volatile bool     pending;     /* an edge is waiting to be handled        */
    volatile uint32_t edges;       /* EVERY edge, bounce included             */
    /* --- main loop only --- */
    uint64_t          last_tick;   /* tick of the last ACCEPTED press         */
    bool              have_last;
    uint32_t          presses;     /* accepted presses                        */
    uint32_t          bounces;     /* edges discarded by debouncing           */
    uint64_t          last_ns;     /* grandmaster time of the last press      */
    bool              last_valid;  /* was the time base usable at the time?   */
    uint8_t           extint;
    uint8_t           pin;
    IRQn_Type         irq;
    const char       *name;
} BTN_STATE;

static BTN_STATE s_btn[BOARD_BTN_COUNT];

static uint64_t s_ticks_per_ms = 1u;
static uint32_t s_debounce_ms  = BTN_DEBOUNCE_MS_DEF;
/* One counter shared across BOTH buttons, and that is deliberate: it is the
   event's identifier on the wire, and a gap in it marks a lost frame.  Two
   separate counters could not do that - the master would not see that
   something between #4 and #6 is missing. */
static uint32_t s_seq;
static bool     s_send = true;      /* send the event to the master           */
static bool     s_log  = true;      /* also log it locally to the console     */
static bool     s_ready;            /* init succeeded                         */
static uint32_t s_no_model;         /* press with no model - not convertible  */
/* Injected presses (`tbase btn fire`).  Counted separately, because a test
   hook that lands in the same number as the real events devalues a
   measurement - and because otherwise it does not show that injection
   happened at all. */
static uint32_t s_injected;

/* STAGGERED INJECTION - and the detour is the whole point.
 *
 * Bounce is by definition a sequence of edges millisecond apart, and the
 * HOST cannot produce that: two `cli.py` calls are SECONDS apart.  On
 * 2026-08-23 I measured "debouncing drops nothing" with exactly that
 * approach - and that was a statement about my test tool, not about the
 * firmware.
 *
 * A blocking loop in the command handler is no good either: it stalls the
 * main loop, BTN_Tasks() does NOT run in between, and then it is not
 * debouncing that applies but the ISR's pending rule.  Those are two
 * different mechanisms, and a test that confuses them tests neither.
 *
 * So schedule instead of loop: BTN_Tasks() fires the next edge once its time
 * has come.  That way debouncing sees exactly what a real button delivers
 * to it. */
static uint32_t s_inj_left;
static uint8_t  s_inj_btn;
static uint64_t s_inj_next;
static uint64_t s_inj_gap_tk;

/* --------------------------------------------------------------------------- */

/* Both ISRs do the same thing and must do it separately, because the NVIC
 * has two vectors.  The body is deliberately tiny: read the clock, remember
 * it, clear the flag.  No conversion (that needs the model), no printing
 * (that needs the console), no SPI.
 *
 * `SYS_TIME_Counter64Get()` AS THE FIRST STATEMENT - anything before it
 * would end up in the timestamp. */
static inline void btn_isr(BTN_STATE *b)
{
    uint64_t t = SYS_TIME_Counter64Get();

    b->tick = t;
    b->edges++;
    /* Do NOT overwrite if one is still waiting: the waiting one is the older
       and therefore the real press; whatever comes now is normally its
       bounce.  The edge is still counted, so it does not vanish invisibly. */
    if (!b->pending)
    {
        b->pending = true;
    }
    /* Only the OWN bit.  A full mask would also clear other EXTINTs' flags -
       here that would be the 1PPS. */
    EIC_REGS->EIC_INTFLAG = (1u << b->extint);
}

void EIC_EXTINT_0_Handler(void)
{
    btn_isr(&s_btn[0]);
}

void EIC_EXTINT_1_Handler(void)
{
    btn_isr(&s_btn[1]);
}

/* --------------------------------------------------------------------------- */

/* Set up one button pin: peripheral function A (EIC), input buffer on,
 * PULL-UP.
 *
 * The button pulls to ground; without a pull-up the input is an antenna and
 * noise turns into presses.  The DIRECTION of the pull is chosen by the OUT
 * bit as long as DIR = 0 and PULLEN = 1 - hence OUTSET and not OUTCLR
 * (peer_capture.c does the same with OUTCLR, because it needs a pull-DOWN
 * there).
 *
 * HW_PinMux() rewrites PINCFG, so the pull bit must come AFTER it; PINCFG is
 * one byte per pin and therefore exclusive, a read-modify-write on it shares
 * nothing.
 */
static void btn_pin_init(uint8_t pin, uint32_t mask)
{
    HW_PinMux(BOARD_BTN_GROUP, pin, 0u /* peripheral A */, true /* input */);
    PORT_REGS->GROUP[BOARD_BTN_GROUP].PORT_OUTSET = mask;
    PORT_REGS->GROUP[BOARD_BTN_GROUP].PORT_PINCFG[pin] |= PORT_PINCFG_PULLEN_Msk;
}

void BTN_Initialize(void)
{
    uint32_t i;

    s_ticks_per_ms = (uint64_t)SYS_TIME_FrequencyGet() / 1000u;
    if (s_ticks_per_ms == 0u)
    {
        s_ticks_per_ms = 1u;
    }

    memset(s_btn, 0, sizeof s_btn);
    s_btn[0].extint = BOARD_BTN1_EXTINT;
    s_btn[0].pin    = BOARD_BTN1_PIN;
    s_btn[0].irq    = EIC_EXTINT_0_IRQn;
    s_btn[0].name   = "SW1";
    s_btn[1].extint = BOARD_BTN2_EXTINT;
    s_btn[1].pin    = BOARD_BTN2_PIN;
    s_btn[1].irq    = EIC_EXTINT_1_IRQn;
    s_btn[1].name   = "SW2";

    btn_pin_init(BOARD_BTN1_PIN, BOARD_BTN1_MASK);
    btn_pin_init(BOARD_BTN2_PIN, BOARD_BTN2_MASK);

    for (i = 0u; i < BOARD_BTN_COUNT; i++)
    {
        /* SENSE = 2 (FALL): the button is active low, pressed is the falling
           edge.  ASYNCH, so a short pulse does not vanish between two clock
           edges - and because the EIC then needs no GCLK. */
        if (!HW_EicClaim(s_btn[i].extint, 2u /* FALL */, true /* ASYNCH */))
        {
            SYS_CONSOLE_PRINT("[BTN] EXTINT%u could not be claimed -"
                              " already taken (mask 0x%08lX).  %s will NOT report.\r\n",
                              (unsigned)s_btn[i].extint,
                              (unsigned long)HW_EicClaimed(), s_btn[i].name);
            return;
        }
        NVIC_SetPriority(s_btn[i].irq, BTN_IRQ_PRIO);
        /* Clear before enabling: HW_EicClaim() has already set INTENSET, and
           any edge left over from setup would otherwise be a press nobody
           made. */
        NVIC_ClearPendingIRQ(s_btn[i].irq);
        NVIC_EnableIRQ(s_btn[i].irq);
    }
    s_ready = true;
}

/* --------------------------------------------------------------------------- */

static void btn_handle(BTN_STATE *b, uint8_t btn_id)
{
    uint64_t tick;
    uint64_t ns = 0u;
    bool     usable;
    bool     have_ns;

    /* Take it out of the ISR's hand ATOMICALLY.  Without this a second press
       would overwrite the one currently being handled - the same bracket as
       for the 1PPS in pps_capture.c. */
    NVIC_DisableIRQ(b->irq);
    tick = b->tick;
    b->pending = false;
    NVIC_EnableIRQ(b->irq);

    /* DEBOUNCE ON THE TICK, not on a counting loop: the tick dates the edge,
       any other quantity dates the handling.
       Unsigned subtraction, so check the ordering first - a tick from the
       future (which can happen on a clock jump) would otherwise read as
       "292 years old" and slip through. */
    if (b->have_last && (tick > b->last_tick)
        && ((tick - b->last_tick) < ((uint64_t)s_debounce_ms * s_ticks_per_ms)))
    {
        b->bounces++;
        return;
    }
    b->last_tick = tick;
    b->have_last = true;
    b->presses++;

    /* The quality BELONGS TO THE EVENT and is sent along, instead of
       suppressing the press.  A button press that goes unreported because
       of a bad clock is the defect one goes looking for afterwards. */
    usable  = PTP_TB_IsUsable();
    have_ns = PTP_TB_Convert(tick, &ns);
    if (!have_ns)
    {
        /* No model (UNINIT) - then there is no number that could be sent.
           Counted, so "nothing happened" has a name. */
        s_no_model++;
        ns     = 0u;
        usable = false;
    }

    b->last_ns    = ns;
    b->last_valid = have_ns && usable;

    s_seq++;
    if (s_send)
    {
        (void)TRIGCMD_ButtonEvent(btn_id, b->last_valid, s_seq, ns);
    }
    if (s_log)
    {
        if (b->last_valid)
        {
            SYS_CONSOLE_PRINT("[BTN] %s  #%lu  %llu.%09llu s (master)\r\n",
                              b->name, (unsigned long)s_seq,
                              (unsigned long long)(ns / 1000000000ull),
                              (unsigned long long)(ns % 1000000000ull));
        }
        else
        {
            SYS_CONSOLE_PRINT("[BTN] %s  #%lu  time base not usable -"
                              " timestamp invalid\r\n",
                              b->name, (unsigned long)s_seq);
        }
    }
}

void BTN_Tasks(void)
{
    uint32_t i;

    if (!s_ready)
    {
        return;
    }

    /* Scheduled injection first: it is meant to SET an edge, which then gets
       handled completely normally in this or the next pass. */
    if (s_inj_left != 0u)
    {
        uint64_t now = SYS_TIME_Counter64Get();

        if (now >= s_inj_next)
        {
            /* Via btn_isr(), not around it: this way the real interrupt body
               runs along, including the rule not to overwrite a waiting
               edge. */
            btn_isr(&s_btn[s_inj_btn]);
            s_injected++;
            s_inj_left--;
            s_inj_next = now + s_inj_gap_tk;
        }
    }

    /* ONE event per pass, even if both buttons are waiting.  The raw sender
       hands the driver a POINTER to its static buffer; two frames in one
       pass would overwrite each other in it, and the capture would then show
       intent instead of the wire (FALLSTRICKE_LAN8651.md).  The second
       button comes in the next pass, i.e. microseconds later - and its
       TIMESTAMP is unaffected by that, because it was already fixed. */
    for (i = 0u; i < BOARD_BTN_COUNT; i++)
    {
        if (s_btn[i].pending)
        {
            btn_handle(&s_btn[i], (uint8_t)(i + 1u));
            return;
        }
    }
}

/* --------------------------------------------------------------------------- */

bool BTN_CliTry(int argc, char **argv)
{
    uint32_t i;

    if (argc < 2 || strcmp(argv[1], "btn") != 0)
    {
        return false;
    }

    if (argc >= 4 && !strcmp(argv[2], "send"))
    {
        s_send = (strcmp(argv[3], "on") == 0);
        SYS_CONSOLE_PRINT("[BTN] send: %s\r\n", s_send ? "on" : "off");
        return true;
    }
    if (argc >= 4 && !strcmp(argv[2], "log"))
    {
        s_log = (strcmp(argv[3], "on") == 0);
        SYS_CONSOLE_PRINT("[BTN] log: %s\r\n", s_log ? "on" : "off");
        return true;
    }
    /* INJECT ONE PRESS - the injection hook, for the same reason as
     * `tbase inject` on the outlier filter: a chain that can only be fired
     * with one finger does not get tested.
     *
     * It sets EXACTLY what the ISR sets (tick and flag), and leaves
     * everything after it running unchanged: debouncing, conversion,
     * sending, printing on the bridge.  What stays untested is therefore
     * only the ISR itself - four lines - and the edge at the pin; the
     * latter is shown by the level column in the status output.
     *
     * The edges ARE counted along, so an injected press does not look like
     * a real one in the statistics, and does not vanish either. */
    if (argc >= 3 && !strcmp(argv[2], "fire"))
    {
        uint32_t which = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 0) : 1u;
        uint32_t n     = (argc >= 5) ? (uint32_t)strtoul(argv[4], NULL, 0) : 1u;
        uint32_t gap   = (argc >= 6) ? (uint32_t)strtoul(argv[5], NULL, 0) : 5u;

        if (which < 1u || which > BOARD_BTN_COUNT || n < 1u || n > 100u)
        {
            SYS_CONSOLE_PRINT("[BTN] fire <1|2> [edges 1..100] [gap_ms]\r\n");
            return true;
        }
        s_inj_btn    = (uint8_t)(which - 1u);
        s_inj_left   = n;
        s_inj_gap_tk = (uint64_t)gap * s_ticks_per_ms;
        s_inj_next   = SYS_TIME_Counter64Get();
        SYS_CONSOLE_PRINT("[BTN] %s: injecting %lu edge(s) %lu ms apart"
                          " (not a real press)\r\n",
                          s_btn[s_inj_btn].name, (unsigned long)n,
                          (unsigned long)gap);
        return true;
    }

    if (argc >= 4 && !strcmp(argv[2], "debounce"))
    {
        uint32_t v = (uint32_t)strtoul(argv[3], NULL, 0);
        /* 0 is allowed and means "no debouncing" - for the counter-check
           that debouncing does anything at all.  Capped above so a typo
           does not silence the button. */
        if (v > 5000u)
        {
            SYS_CONSOLE_PRINT("[BTN] debounce: 0..5000 ms\r\n");
            return true;
        }
        s_debounce_ms = v;
        SYS_CONSOLE_PRINT("[BTN] debounce: %lu ms\r\n", (unsigned long)v);
        return true;
    }

    SYS_CONSOLE_PRINT("[BTN] %s   send %s   log %s   debounce %lu ms\r\n",
                      s_ready ? "ready" : "NOT READY (EXTINT taken?)",
                      s_send ? "on" : "off", s_log ? "on" : "off",
                      (unsigned long)s_debounce_ms);
    for (i = 0u; i < BOARD_BTN_COUNT; i++)
    {
        const BTN_STATE *b = &s_btn[i];
        /* THE LEVEL RIGHT NOW, and it is half the troubleshooting: active
           low means `released` at 1 and `PRESSED` at 0.  This makes the pin
           configuration checkable with no interrupt and no instrument - hold
           the button, issue the command.  If the pin reads 0 permanently,
           the pull-up is missing or the pin is wrong. */
        uint32_t lvl = PORT_REGS->GROUP[BOARD_BTN_GROUP].PORT_IN
                     & (uint32_t)(1u << b->pin);

        SYS_CONSOLE_PRINT("[BTN] %s  PD%02u/EXTINT%u  pin %s   edges %lu"
                          "   presses %lu   bounces discarded %lu\r\n",
                          b->name, (unsigned)b->pin, (unsigned)b->extint,
                          (lvl != 0u) ? "released" : "PRESSED",
                          (unsigned long)b->edges, (unsigned long)b->presses,
                          (unsigned long)b->bounces);
        if (b->presses != 0u)
        {
            if (b->last_valid)
            {
                SYS_CONSOLE_PRINT("[BTN]      last: %llu.%09llu s (master)\r\n",
                                  (unsigned long long)(b->last_ns / 1000000000ull),
                                  (unsigned long long)(b->last_ns % 1000000000ull));
            }
            else
            {
                SYS_CONSOLE_PRINT("[BTN]      last: no valid time\r\n");
            }
        }
    }
    /* "edges" against "presses" is the real information in this printout:
       the difference IS the bounce, and whoever does not look at it goes
       hunting for the fault in the reporting chain instead. */
    SYS_CONSOLE_PRINT("[BTN] seq %lu   discarded for no model: %lu"
                      "   injected: %lu\r\n",
                      (unsigned long)s_seq, (unsigned long)s_no_model,
                      (unsigned long)s_injected);
    return true;
}
