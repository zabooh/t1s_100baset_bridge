/*******************************************************************************
  hw_shared.c - see hw_shared.h for the reasoning behind exactly these three
  functions and no HAL.
*******************************************************************************/

#include "definitions.h"
#include "hw_shared.h"

/* Expected values pinned against the DFP, as in `pps_capture.c` and
   `ptp_trigger.c`: whatever stood here by hand would be the same mistake one
   level up. */
_Static_assert((uintptr_t)&EIC_REGS->EIC_CTRLA    == 0x40002800u, "EIC_CTRLA");
_Static_assert((uintptr_t)&EIC_REGS->EIC_INTENCLR == 0x4000280Cu, "EIC_INTENCLR");
_Static_assert((uintptr_t)&EIC_REGS->EIC_INTFLAG  == 0x40002814u, "EIC_INTFLAG");
_Static_assert((uintptr_t)&EIC_REGS->EIC_CONFIG[0] == 0x4000281Cu, "EIC_CONFIG0");
_Static_assert(EIC_CTRLA_ENABLE_Msk == 0x02u, "EIC ENABLE");

#define EIC_EXTINT_MAX      16u     /* EXTINT0..15 */
#define EIC_SENSE_MAX       5u      /* 1 RISE, 2 FALL, 3 BOTH, 4 HIGH, 5 LOW */
#define EIC_CONFIG_PER_REG  8u      /* eight EXTINTs per CONFIG register      */
#define EIC_SENSE_BITS      4u      /* four bits per EXTINT                   */
#define EIC_SENSE_MASK      0xFu

/* Who holds what.  Bit n = EXTINT n is claimed. */
static uint32_t s_eic_claimed;
static uint32_t s_eic_refused;

/* --------------------------------------------------------------------------- */
/* Class B: shared registers                                                   */
/* --------------------------------------------------------------------------- */

void HW_ApbaClockEnable(uint32_t mask)
{
    /* Interrupt bracket, because the read, the OR and the write-back are
       three steps.  Nothing runs in between during initialization - but this
       function is also built for the case where a module enables its clock
       later, and there a lost race would switch off a FOREIGN module's
       clock. */
    uint32_t prim = __get_PRIMASK();
    __disable_irq();
    MCLK_REGS->MCLK_APBAMASK |= mask;
    if (prim == 0u)
    {
        __enable_irq();
    }
}

void HW_ApbbClockEnable(uint32_t mask)
{
    uint32_t prim = __get_PRIMASK();
    __disable_irq();
    MCLK_REGS->MCLK_APBBMASK |= mask;
    if (prim == 0u)
    {
        __enable_irq();
    }
}

void HW_ApbcClockEnable(uint32_t mask)
{
    uint32_t prim = __get_PRIMASK();
    __disable_irq();
    MCLK_REGS->MCLK_APBCMASK |= mask;
    if (prim == 0u)
    {
        __enable_irq();
    }
}

void HW_ApbdClockEnable(uint32_t mask)
{
    uint32_t prim = __get_PRIMASK();
    __disable_irq();
    MCLK_REGS->MCLK_APBDMASK |= mask;
    if (prim == 0u)
    {
        __enable_irq();
    }
}

void HW_PinMux(uint8_t group, uint8_t pin, uint8_t func, bool input)
{
    uint8_t cfg = (uint8_t)PORT_PINCFG_PMUXEN_Msk;
    uint8_t idx = (uint8_t)(pin / 2u);
    uint8_t old;
    uint32_t prim;

    if (input)
    {
        cfg |= (uint8_t)PORT_PINCFG_INEN_Msk;
    }
    PORT_REGS->GROUP[group].PORT_PINCFG[pin] = cfg;

    /* PMUX: one byte for TWO pins.  Even pin -> lower four bits (PMUXE), odd
       -> upper (PMUXO).  A full write would reconfigure the neighbouring
       pin too; in the concrete case of PC12 that would be PC13. */
    prim = __get_PRIMASK();
    __disable_irq();
    old = PORT_REGS->GROUP[group].PORT_PMUX[idx];
    if ((pin & 1u) == 0u)
    {
        PORT_REGS->GROUP[group].PORT_PMUX[idx] =
            (uint8_t)((old & 0xF0u) | (func & 0x0Fu));
    }
    else
    {
        PORT_REGS->GROUP[group].PORT_PMUX[idx] =
            (uint8_t)((old & 0x0Fu) | (uint8_t)((func & 0x0Fu) << 4u));
    }
    if (prim == 0u)
    {
        __enable_irq();
    }
}

/* --------------------------------------------------------------------------- */
/* EIC                                                                          */
/* --------------------------------------------------------------------------- */

static void eic_wait_sync(void)
{
    while ((EIC_REGS->EIC_SYNCBUSY & EIC_SYNCBUSY_ENABLE_Msk) != 0u)
    {
    }
}

bool HW_EicClaim(uint8_t extint, uint8_t sense, bool asynch)
{
    uint32_t bit;
    uint8_t  reg;
    uint8_t  shift;

    if ((extint >= EIC_EXTINT_MAX) || (sense == 0u) || (sense > EIC_SENSE_MAX))
    {
        s_eic_refused++;
        return false;
    }

    bit = (uint32_t)1u << extint;
    if ((s_eic_claimed & bit) != 0u)
    {
        /* REPORT, don't overwrite.  A second claim on the same EXTINT is a
           programming error, and the silent version of it costs a
           measurement run. */
        s_eic_refused++;
        return false;
    }

    /* CONFIG is enable-protected: the instance must be off to write it.
       That holds for EVERY claim, not just the first - hence it is switched
       off here and back on afterwards.  The difference from the old block:
       the instance is not reset, and the other EXTINTs' enables and flags
       stay as they are. */
    EIC_REGS->EIC_CTRLA = 0u;
    eic_wait_sync();

    /* Only the own bit.  A full mask of 0xFFFFFFFF would clear the enable of
       other EXTINTs here - harmless today because there are none, and
       exactly for that reason a mistake that only surfaces with a second
       user. */
    EIC_REGS->EIC_INTENCLR = bit;

    reg = (uint8_t)(extint / EIC_CONFIG_PER_REG);
    shift = (uint8_t)((extint % EIC_CONFIG_PER_REG) * EIC_SENSE_BITS);
    EIC_REGS->EIC_CONFIG[reg] =
        (EIC_REGS->EIC_CONFIG[reg] & ~((uint32_t)EIC_SENSE_MASK << shift))
        | ((uint32_t)sense << shift);

    if (asynch)
    {
        EIC_REGS->EIC_ASYNCH |= bit;
    }
    else
    {
        EIC_REGS->EIC_ASYNCH &= ~bit;
    }

    EIC_REGS->EIC_CTRLA = (uint8_t)EIC_CTRLA_ENABLE_Msk;
    eic_wait_sync();

    /* Again only the own bit: EIC_INTFLAG is write-1-clear, a full mask
       would also clear other EXTINTs' pending flags. */
    EIC_REGS->EIC_INTFLAG  = bit;
    EIC_REGS->EIC_INTENSET = bit;

    s_eic_claimed |= bit;
    return true;
}

bool HW_EicSenseSet(uint8_t extint, uint8_t sense)
{
    uint32_t bit;
    uint8_t  reg;
    uint8_t  shift;

    if ((extint >= EIC_EXTINT_MAX) || (sense == 0u) || (sense > EIC_SENSE_MAX))
    {
        s_eic_refused++;
        return false;
    }
    bit = (uint32_t)1u << extint;
    if ((s_eic_claimed & bit) == 0u)
    {
        /* Whoever does not hold the channel may not reconfigure it - or one
           module changes the edge sense of someone else's pin, and the
           owner goes looking for the fault in their own hardware. */
        s_eic_refused++;
        return false;
    }

    /* The same dance as in the claim: CONFIG is enable-protected.  Only this
       channel's nibble is touched, the others' enables and flags stay. */
    EIC_REGS->EIC_CTRLA = 0u;
    eic_wait_sync();

    reg = (uint8_t)(extint / EIC_CONFIG_PER_REG);
    shift = (uint8_t)((extint % EIC_CONFIG_PER_REG) * EIC_SENSE_BITS);
    EIC_REGS->EIC_CONFIG[reg] =
        (EIC_REGS->EIC_CONFIG[reg] & ~((uint32_t)EIC_SENSE_MASK << shift))
        | ((uint32_t)sense << shift);

    EIC_REGS->EIC_CTRLA = (uint8_t)EIC_CTRLA_ENABLE_Msk;
    eic_wait_sync();

    /* Clear the own flag: while the instance was off, an edge may have been
       left pending, and it belongs to the OLD sense. */
    EIC_REGS->EIC_INTFLAG = bit;
    return true;
}

bool HW_EicEventEnable(uint8_t extint, bool on)
{
    uint32_t bit;

    if (extint >= EIC_EXTINT_MAX)
    {
        s_eic_refused++;
        return false;
    }
    bit = (uint32_t)1u << extint;
    if ((s_eic_claimed & bit) == 0u)
    {
        /* Enabling the event of a pin that is not claimed means someone made
           a mistake - and the silent version of it delivers a channel that
           never fires while every register reads back as correct. */
        s_eic_refused++;
        return false;
    }

    EIC_REGS->EIC_CTRLA = 0u;                 /* EVCTRL is enable-protected  */
    eic_wait_sync();
    if (on)
    {
        EIC_REGS->EIC_EVCTRL |= bit;
    }
    else
    {
        EIC_REGS->EIC_EVCTRL &= ~bit;
    }
    EIC_REGS->EIC_CTRLA = (uint8_t)EIC_CTRLA_ENABLE_Msk;
    eic_wait_sync();
    return true;
}

uint32_t HW_EicClaimed(void)
{
    return s_eic_claimed;
}

uint32_t HW_EicRefused(void)
{
    return s_eic_refused;
}
