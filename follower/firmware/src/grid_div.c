/*******************************************************************************
  Grid divider - implementation

  File Name:
    grid_div.c

  Summary:
    TC6 counts the grid events, flips LED1 every Nth one.  The reasoning
    behind the design is in grid_div.h; here are the decisions that
    otherwise do not show in the code.
*******************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "definitions.h"
#include "config/default/system/console/sys_console.h"

#include "grid_div.h"
#include "board_pins.h"
#include "hw_shared.h"
#include "pin_table.h"
#include "ptp_trigger.h"

/* --------------------------------------------------------------- addresses
   One assertion per register, as in the neighbouring modules: it checks
   whether the SAME ADDRESS is meant, which a hex comparison fundamentally
   cannot do (FALLSTRICKE_MCU.md, 2026-08-20).  Costs zero flash. */
#define GDIV_ADDR_OF(x)     ((uintptr_t)&(x))
_Static_assert(GDIV_ADDR_OF(TC6_REGS->COUNT16.TC_CTRLA) == 0x43001400u, "TC6_CTRLA");
_Static_assert(GDIV_ADDR_OF(TC6_REGS->COUNT16.TC_CC[0]) == 0x4300141Cu, "TC6_CC0");
_Static_assert(sizeof(TC6_REGS->COUNT16.TC_CC[0]) == 2u, "TC6_CC0 is 16 bit");

/* TWO channels, and the split is not arbitrary - reasoning in the header.
   Taken today: 0 (trigger, rising), 1 (1PPS), 2 (trigger, falling) and
   3 (neighbour edge); 4 and 5 are the next free ones. */
#define GDIV_CH_IN          4u      /* grid event -> TC6, RESYNCHRONIZED     */
#define GDIV_CH_OUT         5u      /* TC6 overflow -> PORT, ASYNCHRONOUS    */
#define EVSYS_CH_GCLK_BASE  EVSYS_GCLK_ID_0     /* EVSYS_CHANNEL_n = PCHCTRL[11+n] */

/* The PORT slot.  User n serves slot n in EVERY group (test_results.md
   E63), so LED1 in group 2 needs slot 2 - slots 0 and 1 belong to the
   trigger in group 3.  `PID2`/`EVACT2`/`PORTEI2` are written. */
#define GDIV_PORT_USER      EVENT_ID_USER_PORT_EV_2

/* LED1 in the pin table - index 2, group 2, pin 21, inverted. */
#define GDIV_PIN_INDEX      2u

static uint32_t s_n;                /* divider, 0 = off                       */
static bool     s_own;              /* claim on LED1 held                     */
static bool     s_ready;            /* init succeeded                         */
static const PIN_ROW *s_led;

/* Alignment.  `volatile`, because the flag is set in task context and read
   and cleared in the grid interrupt. */
static volatile bool     s_align_req;   /* alignment requested                */
static volatile uint32_t s_align_done;  /* how many times aligned             */
static volatile uint32_t s_align_defer; /* how many times deferred (too close to overflow) */
static volatile uint64_t s_align_idx;   /* grid index of the last alignment   */

/* --------------------------------------------------------------------------- */

/* Set up TC6: APB clock, GCLK, 16 bit, MFRQ, count events.
 *
 * MFRQ makes CC0 the overflow value - the counter resets on a match, and
 * that is exactly the divider characteristic.  Without MFRQ it would run to
 * 0xFFFF and the divider would be fixed at 65536.
 *
 * `OVFEO` instead of `MCEO0` as the event source: what should be reported
 * is "counter reached TOP", and TOP under MFRQ is exactly CC0.  Should that
 * turn out wrong on the device (this is the ONE register claim I could not
 * back from the header), the fix is one line: the generator id on channel
 * OUT to `EVENT_ID_GEN_TC6_MC_0`, and `MCEO0` added here.  `tbase div`
 * shows the counter state, so it is visible at a glance whether TC6 counts
 * at all and whether the event comes out. */
static void gdiv_tc6_init(void)
{
    /* TC6 hangs off APB **D** - there was no user on it in this project
       before, hence HW_ApbdClockEnable() is new.  Read-modify-write,
       because TCC2..TCC4, ADC, DAC and SERCOM4..7 also hang off this bus. */
    HW_ApbdClockEnable(MCLK_APBDMASK_TC6_Msk);

    /* GCLK for TC6.  The counter counts events, but the instance still needs
       its peripheral clock for register access and synchronization -
       without it every SYNCBUSY wait hangs.  TC6 and TC7 SHARE this channel
       (TC6_GCLK_ID == TC7_GCLK_ID == 39); TC7 stays free but is supplied
       along with it. */
    GCLK_REGS->GCLK_PCHCTRL[TC6_GCLK_ID] = GCLK_PCHCTRL_GEN_GCLK1 | GCLK_PCHCTRL_CHEN_Msk;
    while ((GCLK_REGS->GCLK_PCHCTRL[TC6_GCLK_ID] & GCLK_PCHCTRL_CHEN_Msk) == 0u)
    {
    }

    TC6_REGS->COUNT16.TC_CTRLA = 0u;
    while ((TC6_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_ENABLE_Msk) != 0u)
    {
    }
    TC6_REGS->COUNT16.TC_CTRLA = TC_CTRLA_MODE_COUNT16;
    TC6_REGS->COUNT16.TC_WAVE  = TC_WAVE_WAVEGEN_MFRQ;
    TC6_REGS->COUNT16.TC_CC[0] = 0xFFFFu;        /* until `tbase div` says otherwise */
    TC6_REGS->COUNT16.TC_EVCTRL = TC_EVCTRL_TCEI_Msk | TC_EVCTRL_EVACT_COUNT
                                | TC_EVCTRL_OVFEO_Msk;
    TC6_REGS->COUNT16.TC_INTENCLR = 0xFFu;       /* no interrupt - that is the point */
    TC6_REGS->COUNT16.TC_INTFLAG  = 0xFFu;
}

/* The two event channels.  Once, and never touched again afterwards: they
 * cost nothing as long as no event arrives, and reconfiguring at runtime
 * would be exactly the kind of intervention that loses a 1PPS pulse
 * elsewhere. */
static void gdiv_evsys_init(void)
{
    /* INPUT: grid event -> TC6.  RESYNCHRONIZED as with the neighbour
       capture, and a resynchronized channel needs its OWN GCLK - the detail
       one otherwise spends a long time looking for (peer_capture.c). */
    GCLK_REGS->GCLK_PCHCTRL[EVSYS_CH_GCLK_BASE + GDIV_CH_IN] =
        GCLK_PCHCTRL_GEN_GCLK1 | GCLK_PCHCTRL_CHEN_Msk;
    while ((GCLK_REGS->GCLK_PCHCTRL[EVSYS_CH_GCLK_BASE + GDIV_CH_IN]
            & GCLK_PCHCTRL_CHEN_Msk) == 0u)
    {
    }
    EVSYS_REGS->CHANNEL[GDIV_CH_IN].EVSYS_CHANNEL =
        EVSYS_CHANNEL_EVGEN(PTP_TRIG_GridEventGen())
        | EVSYS_CHANNEL_PATH_RESYNCHRONIZED
        | EVSYS_CHANNEL_EDGSEL_RISING_EDGE;
    EVSYS_REGS->EVSYS_USER[EVENT_ID_USER_TC6_EVU] = GDIV_CH_IN + 1u;

    /* OUTPUT: TC6 overflow -> PORT.  ASYNCHRONOUS like the trigger path: no
       GCLK, no resynchronization delay, shortest way to the edge. */
    EVSYS_REGS->CHANNEL[GDIV_CH_OUT].EVSYS_CHANNEL =
        EVSYS_CHANNEL_EVGEN(EVENT_ID_GEN_TC6_OVF) | EVSYS_CHANNEL_PATH_ASYNCHRONOUS;
    EVSYS_REGS->EVSYS_USER[GDIV_PORT_USER] = GDIV_CH_OUT + 1u;
}

/* Claim slot 2 of the LED group on PC21, or release it again.
 *
 * Read-modify-write on ONLY the three fields of slot 2: `PORT_EVCTRL` is one
 * register for FOUR slots, a full write would reconfigure the others too.
 * Nobody else is in group 2 today (E63 established that with a 2 s
 * counter-check), but relying on that would be the same mistake as a
 * `write32` on an enable mask. */
static void gdiv_port_slot(bool on)
{
    uint32_t v = PORT_REGS->GROUP[BOARD_LED1_GROUP].PORT_EVCTRL;

    v &= ~(PORT_EVCTRL_PID2_Msk | PORT_EVCTRL_EVACT2_Msk | PORT_EVCTRL_PORTEI2_Msk);
    if (on)
    {
        v |= PORT_EVCTRL_PID2(21u)                              /* PC21 = LED1 */
           | PORT_EVCTRL_EVACT2(PORT_EVCTRL_EVACT0_TGL_Val)     /* toggle      */
           | PORT_EVCTRL_PORTEI2_Msk;
    }
    PORT_REGS->GROUP[BOARD_LED1_GROUP].PORT_EVCTRL = v;
}

/* Read the counter state - TWO-STAGE, because this project has already paid
 * for the one-stage version (`peer_capture.c`, measured 2026-08-20).
 *
 * `COUNT` is read-synchronized: first a `READSYNC` command, then wait.
 * Whoever runs straight into `while (SYNCBUSY.COUNT) {}` finishes
 * IMMEDIATELY - the command still has to cross the clock boundary itself,
 * so the flag has not appeared yet - and reads the state from BEFORE the
 * synchronization.  So: wait, bounded, for the flag to APPEAR (the
 * synchronization may already be done), then for it to disappear. */
#define GDIV_SYNC_SPINS 64u

static uint16_t gdiv_count_raw(void)
{
    uint32_t w;

    TC6_REGS->COUNT16.TC_CTRLBSET = TC_CTRLBSET_CMD_READSYNC;
    for (w = 0u; w < GDIV_SYNC_SPINS; w++)
    {
        if ((TC6_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_COUNT_Msk) != 0u)
        {
            break;
        }
    }
    while ((TC6_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_COUNT_Msk) != 0u)
    {
    }
    return TC6_REGS->COUNT16.TC_COUNT;
}

static void gdiv_enable(bool on)
{
    if (on)
    {
        TC6_REGS->COUNT16.TC_CTRLA |= TC_CTRLA_ENABLE_Msk;
    }
    else
    {
        TC6_REGS->COUNT16.TC_CTRLA &= ~TC_CTRLA_ENABLE_Msk;
    }
    while ((TC6_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_ENABLE_Msk) != 0u)
    {
    }
}

/* --------------------------------------------------------------------------- */

void GDIV_Initialize(void)
{
    s_led = PIN_Find(GDIV_PIN_INDEX);
    if (s_led == NULL)
    {
        SYS_CONSOLE_PRINT("[GDIV] LED1 not in the pin table -"
                          " the grid divider will NOT run.\r\n");
        return;
    }
    gdiv_tc6_init();
    gdiv_evsys_init();
    gdiv_port_slot(false);      /* slot defined OFF, not "however it happened to be" */
    s_n = 0u;
    s_own = false;
    s_ready = true;
}

bool GDIV_Set(uint32_t n)
{
    if (!s_ready || n < 1u || n > 65536u)
    {
        return false;
    }
    if (!PIN_Claim(s_led->index, PIN_OWNER_GRIDDIV))
    {
        return false;           /* someone else holds LED1 - change NOTHING */
    }
    s_own = true;

    /* BRIEFLY DISABLE instead of going through CCBUF: under MFRQ, CC0 is
       double-buffered, and the take-over hangs off the next cycle - at a
       divider of 65535 that would be seconds later.  An off/on costs at
       most half an LED period, is irrelevant for a command, and avoids the
       buffer synchronization along with its traps. */
    gdiv_enable(false);
    TC6_REGS->COUNT16.TC_CC[0] = (uint16_t)(n - 1u);
    while ((TC6_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_CC0_Msk) != 0u)
    {
    }
    /* Counter to 0, so the first toggle comes after exactly n grid points
       and not after a remainder from the previous divider. */
    TC6_REGS->COUNT16.TC_COUNT = 0u;
    while ((TC6_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_COUNT_Msk) != 0u)
    {
    }
    gdiv_enable(true);

    PIN_Set(s_led, false);      /* defined starting level */
    gdiv_port_slot(true);
    s_n = n;

    /* The starting level above is defined, but LOCAL - it says nothing
       about what the other boards are showing right now.  So request an
       alignment as soon as the grid reaches the next suitable point. */
    GDIV_AlignRequest();
    return true;
}

void GDIV_Off(void)
{
    if (!s_ready)
    {
        return;
    }
    /* Disconnect the pin from the event first, then clear it, then release:
       after release this module must not touch the pin any more, and an
       event still connected would overwrite the new owner. */
    gdiv_port_slot(false);
    gdiv_enable(false);
    if (s_own)
    {
        PIN_Set(s_led, false);
        PIN_Release(s_led->index, PIN_OWNER_GRIDDIV);
        s_own = false;
    }
    s_n = 0u;
    s_align_req = false;        /* otherwise a later switch-on would align
                                   with a request left over from now */
}

void GDIV_AlignRequest(void)
{
    s_align_req = true;
}

/* The reasoning behind the conditions is in the header; here only what gets
 * computed.
 *
 * `r = idx % n` is the count phase TC6 SHOULD have at this grid point, and
 * `(idx / n) & 1` the level the LED should then have - both functions of
 * the absolute index, so the same on every board.  Exactly the construction
 * by which action 4 forms its parity (E24), just one divider stage further.
 *
 * The 64-bit modulo is expensive (no 64-bit division instruction on the M4)
 * and therefore sits BEHIND the flag test: in normal operation this
 * function costs one register read and one branch. */
void GDIV_AlignTick(uint64_t idx)
{
    uint32_t r;
    uint16_t want;

    if (!s_align_req)
    {
        return;                     /* the normal case, and it is free */
    }
    if (!s_ready || !s_own || s_n == 0u)
    {
        return;                     /* request stays pending until the divider runs */
    }
    if (s_n < 4u)
    {
        r = 0u;                     /* too short for a safety band, and at
                                       n < 4 the LED is unwatchably fast anyway */
    }
    else
    {
        r = (uint32_t)(idx % s_n);
        if (r < (s_n / 4u) || r > ((3u * s_n) / 4u))
        {
            s_align_defer++;        /* too close to the overflow - next point */
            return;
        }
    }

    want = (uint16_t)r;
    TC6_REGS->COUNT16.TC_COUNT = want;
    while ((TC6_REGS->COUNT16.TC_SYNCBUSY & TC_SYNCBUSY_COUNT_Msk) != 0u)
    {
    }
    PIN_Set(s_led, (((idx / s_n) & 1u) != 0u));

    s_align_idx  = idx;
    s_align_done++;
    s_align_req  = false;
}

void GDIV_GridStopped(void)
{
    /* The divider stays SET - only the LED goes off.  Otherwise it would
       stay in whatever state the last grid point left it in, and an LED
       that stays lit after `trigoff` looks like a stuck divider. */
    if (s_ready && s_own)
    {
        PIN_Set(s_led, false);
    }
}

bool GDIV_CliTry(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "div") != 0)
    {
        return false;
    }

    if (argc >= 3 && (!strcmp(argv[2], "off") || !strcmp(argv[2], "0")))
    {
        GDIV_Off();
        SYS_CONSOLE_PRINT("[GDIV] off - LED1 is free again\r\n");
        return true;
    }
    if (argc >= 3)
    {
        uint32_t n = (uint32_t)strtoul(argv[2], NULL, 0);

        if (n < 1u || n > 65536u)
        {
            SYS_CONSOLE_PRINT("[GDIV] divider 1..65536 (or 'off')\r\n");
            return true;
        }
        if (!GDIV_Set(n))
        {
            SYS_CONSOLE_PRINT("[GDIV] LED1 is held by '%s' - nothing changed\r\n",
                              PIN_OwnerName(PIN_OwnerGet(s_led != NULL
                                                         ? s_led->index : 0u)));
            return true;
        }
    }

    if (!s_ready)
    {
        SYS_CONSOLE_PRINT("[GDIV] NOT ready\r\n");
        return true;
    }
    SYS_CONSOLE_PRINT("[GDIV] divider: %s   LED1: %s\r\n",
                      (s_n == 0u) ? "off" : "on", s_own ? "held" : "free");
    if (s_n != 0u)
    {
        /* THE PROPERTY THAT OTHERWISE LOOKS LIKE A DEFECT: what is counted
           are GRID POINTS, not seconds.  So write out the calculation
           instead of asserting a frequency that depends on the grid
           period. */
        SYS_CONSOLE_PRINT("[GDIV]   n = %lu -> LED flips every %lu-th grid point;"
                          " cycle = 2 * %lu * grid period\r\n",
                          (unsigned long)s_n, (unsigned long)s_n,
                          (unsigned long)s_n);
        SYS_CONSOLE_PRINT("[GDIV]   at a 100 us grid that is a %lu ms cycle\r\n",
                          (unsigned long)(2u * s_n / 10u));
    }
    /* The counter state is the diagnostic: if it stands still, no grid
       event is arriving (trigger off? wrong channel?); if it runs and the
       LED stays dark, the overflow event is not coming out - then OVFEO is
       the wrong source and MC_0 is the right one (see gdiv_tc6_init). */
    SYS_CONSOLE_PRINT("[GDIV]   TC6 COUNT = %u   CC0 = %u   EVCTRL = 0x%04X\r\n",
                      (unsigned)gdiv_count_raw(),
                      (unsigned)TC6_REGS->COUNT16.TC_CC[0],
                      (unsigned)TC6_REGS->COUNT16.TC_EVCTRL);

    /* The alignment is the difference between "both boards blink" and "both
       boards blink IN PHASE", and without this line there is no way to see
       whether it ever ran.  `pending: yes` while `aligned` stands still
       means: no grid point is arriving (trigger off). */
    SYS_CONSOLE_PRINT("[GDIV]   aligned: %lu   deferred: %lu"
                      "   pending: %s   last index: %lu\r\n",
                      (unsigned long)s_align_done, (unsigned long)s_align_defer,
                      s_align_req ? "yes" : "no",
                      (unsigned long)s_align_idx);

    /* THE SELF-CHECK, and it is built so it needs NO second console port:
     * after alignment, count phase and level must be functions of the GRID
     * INDEX, and the index is the same on every board.  If the relation
     * holds on each board BY ITSELF, the boards are necessarily in phase -
     * without having to read two outputs at the same instant (exactly the
     * comparison that answered wrongly on 4 of 4 samples on 2026-08-24).
     *
     * `COUNT` may deviate by a few points here: between reading the index
     * and reading COUNT the grid keeps running, by one point per 100 us at
     * a 100 us period.  The LEVEL, on the other hand, only changes every n
     * points and is the statement that matters - it is what was inverted. */
    if (s_n != 0u)
    {
        uint64_t idx    = PTP_TRIG_GridIndex();
        uint32_t want   = (uint32_t)(idx % s_n);
        uint32_t is     = (uint32_t)gdiv_count_raw();
        bool     lwant  = (((idx / s_n) & 1u) != 0u);
        bool     lis    = PIN_Get(s_led);
        int32_t  d      = (int32_t)is - (int32_t)want;
        bool     cok    = ((d > -4) && (d < 4));

        /* THE LEVEL IS CHECKED AT THE PIN, not just computed.  The first
           version only printed the target value - and the level is exactly
           the quantity that was inverted; a check that does not compare it
           checks past the illness.  At the edge of a toggle it may deviate
           (the index is read before the pin), so the deviation only counts
           as a fault when COUNT is far enough from 0 and n. */
        bool ledge = (s_n >= 8u) && ((want < 4u) || (want > (s_n - 4u)));
        bool lok   = (lis == lwant) || ledge;

        SYS_CONSOLE_PRINT("[GDIV]   self-check: index %lu   COUNT want %lu"
                          " is %lu (%+ld)   level want %u is %u%s   %s\r\n",
                          (unsigned long)idx, (unsigned long)want,
                          (unsigned long)is, (long)d,
                          (unsigned)(lwant ? 1u : 0u), (unsigned)(lis ? 1u : 0u),
                          ledge ? " (edge)" : "",
                          (cok && lok) ? "ok" : "MISMATCH");
    }
    return true;
}
