/*******************************************************************************
  Pin table - implementation

  File Name:
    pin_table.c

  Summary:
    The one row per pin, the owner per pin, and the inversion in exactly one
    place.  Everything else is in the header.
*******************************************************************************/

#include "pin_table.h"
#include "definitions.h"

/* The table.
 *
 * The three indices 2, 6 and 10 are `lan866x-ledblink`'s own defaults
 * (`--pins 2,6,10`).  That is not a coincidence and must not be reordered:
 * with exactly these numbers the foreign tool runs through literally, and
 * you SEE the result - two LEDs blink, the third slot is the probe pin.
 *
 * Whoever offers a fourth pin appends a row.  Nothing else: the index space,
 * the response codes and the dispatcher all read from here.
 *
 * `can_pwm` is a PROPERTY OF THE PATH, not of the pin.  The schedule drives
 * the pin via TC1 compare and a PORT event, and the PORT event hangs off the
 * GROUP's EVCTRL: PD10 sits in group 3, where the two PID slots for rising
 * and falling edge are taken.
 *
 * WATCH OUT, a wrong justification stood here until 2026-08-22: that the LEDs
 * "would get their own EVSYS channel, their own user and their own PID
 * slots".  The resource part of that is DISPROVEN.  PORT_EVCTRL sits at
 * offset 0x2C INSIDE port_group_registers_t, so there are four slots per
 * group (16 total) for only four global users PORT_EV_0..3 - slot n is fed
 * by user n and takes effect in EVERY group whose PORTEIn is set.  Measured
 * on the device: slot 0 of group 2 set to PC21 with action SET, nothing
 * else, and PC21 latched on the first TC1_MC_0 event - no second channel, no
 * second user, no second timer, and PD10 kept running unchanged.  A
 * counter-check with `trigoff` stayed at 0, so no foreign writer.  Full
 * derivation in FALLSTRICKE_MCU.md.
 *
 * `false` remains correct anyway, but for the OTHER reason: an LED attached
 * this way inherits the TRIGGER'S GRID and cannot carry a frequency of its
 * own - which is exactly what OpenPwm requires.  So OpenPwm answers
 * RT_NOT_REACHABLE 0x05 on an LED - an honest no beats a yes whose signal
 * then originates in the main loop and cannot hold 100 us.  Whoever wants
 * the SAME signal (not one of their own) on an LED has the cheap path above.
 *
 * NOT 0x03: that would mean "the service does not know the method", and
 * that is false - it knows it, this pin just does not have the path.  The
 * wire field only has 0x00..0x0F anyway, there is no "RT_NOT_SUPPORTED"
 * there.
 */
static const PIN_ROW s_rows[] =
{
    /* idx  grp pin  inv    pwm   name                        */
    {  2u,  2u, 21u, true,  false, "PC21 (LED1)"              },
    {  6u,  0u, 16u, true,  false, "PA16 (LED2)"              },
    { 10u,  3u, 10u, false, true,  "PD10 (EXT1-5, probe pin)" },
};

#define ROW_COUNT (sizeof(s_rows) / sizeof(s_rows[0]))

/* Owner per index.  Fully populated rather than only for the existing rows,
   because `PIN_OwnerGet()` must be able to answer for every index in the
   allowed space. */
static PIN_OWNER s_owner[PIN_INDEX_MAX];

const char *PIN_OwnerName(PIN_OWNER o)
{
    switch (o)
    {
        case PIN_OWNER_TRIGGER: return "trigger";
        case PIN_OWNER_GPIO:    return "gpio";
        case PIN_OWNER_PWM:     return "pwm";
        case PIN_OWNER_BLINK:   return "blink";
        case PIN_OWNER_GRIDLED: return "gridled";
        case PIN_OWNER_GRIDDIV: return "griddiv";
        default:                return "-";
    }
}

void PIN_Initialize(void)
{
    for (unsigned i = 0u; i < PIN_INDEX_MAX; i++)
    {
        s_owner[i] = PIN_OWNER_NONE;
    }

    for (unsigned i = 0u; i < ROW_COUNT; i++)
    {
        const PIN_ROW *r = &s_rows[i];
        uint32_t mask = (1u << r->pin);

        /* Level first, then direction.  The other way round the output
           flashes briefly during boot - visible on an LED, and on a probe
           pin an edge nobody commanded. */
        if (r->invert) { PORT_REGS->GROUP[r->group].PORT_OUTSET = mask; }
        else           { PORT_REGS->GROUP[r->group].PORT_OUTCLR = mask; }
        PORT_REGS->GROUP[r->group].PORT_DIRSET = mask;
    }
}

const PIN_ROW *PIN_Find(uint8_t index)
{
    for (unsigned i = 0u; i < ROW_COUNT; i++)
    {
        if (s_rows[i].index == index) { return &s_rows[i]; }
    }
    return NULL;
}

bool PIN_Claim(uint8_t index, PIN_OWNER who)
{
    if (index >= PIN_INDEX_MAX || PIN_Find(index) == NULL) { return false; }
    /* The same owner may register again: a repeated command is not a
       conflict, and a caller that were not allowed to renew its own claim
       would have to remember whether it already holds it - a second
       truth. */
    if (s_owner[index] != PIN_OWNER_NONE && s_owner[index] != who) { return false; }
    s_owner[index] = who;
    return true;
}

void PIN_Release(uint8_t index, PIN_OWNER who)
{
    /* Only the owner releases.  A release by someone else would be a silent
       theft: the previous owner keeps driving but believes it is alone. */
    if (index < PIN_INDEX_MAX && s_owner[index] == who)
    {
        s_owner[index] = PIN_OWNER_NONE;
    }
}

PIN_OWNER PIN_OwnerGet(uint8_t index)
{
    return (index < PIN_INDEX_MAX) ? s_owner[index] : PIN_OWNER_NONE;
}

void PIN_Set(const PIN_ROW *row, bool on)
{
    if (row == NULL) { return; }
    uint32_t mask = (1u << row->pin);
    bool level = row->invert ? !on : on;
    if (level) { PORT_REGS->GROUP[row->group].PORT_OUTSET = mask; }
    else       { PORT_REGS->GROUP[row->group].PORT_OUTCLR = mask; }
}

bool PIN_Get(const PIN_ROW *row)
{
    if (row == NULL) { return false; }
    bool level = (PORT_REGS->GROUP[row->group].PORT_OUT & (1u << row->pin)) != 0u;
    return row->invert ? !level : level;
}

void PIN_Print(void)
{
    SYS_CONSOLE_PRINT("[PIN] %u pins, index space 0..%u (0x%02X = unused)\r\n",
                      (unsigned)ROW_COUNT, PIN_INDEX_MAX - 1u,
                      (unsigned)PIN_INDEX_UNUSED);
    SYS_CONSOLE_PRINT("      idx  pin                       inv  pwm  owner    level\r\n");
    for (unsigned i = 0u; i < ROW_COUNT; i++)
    {
        const PIN_ROW *r = &s_rows[i];
        SYS_CONSOLE_PRINT("      %3u  %-24s  %-3s  %-3s  %-7s  %s\r\n",
                          r->index, r->name,
                          r->invert ? "yes" : "no",
                          r->can_pwm ? "yes" : "no",
                          PIN_OwnerName(s_owner[r->index]),
                          PIN_Get(r) ? "on" : "off");
    }
}
