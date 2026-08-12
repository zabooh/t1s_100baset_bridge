/*******************************************************************************
 CLOCK PLIB

  Company:
    Microchip Technology Inc.

  File Name:
    plib_clock.c

  Summary:
    CLOCK PLIB Implementation File.

  Description:
    None

*******************************************************************************/

/*******************************************************************************
* Copyright (C) 2018 Microchip Technology Inc. and its subsidiaries.
*
* Subject to your compliance with these terms, you may use Microchip software
* and any derivatives exclusively with Microchip products. It is your
* responsibility to comply with third party license terms applicable to your
* use of third party software (including open source software) that may
* accompany Microchip software.
*
* THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
* EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
* WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
* PARTICULAR PURPOSE.
*
* IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
* INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
* WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
* BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
* FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
* ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
* THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
*******************************************************************************/

#include "plib_clock.h"
#include "device.h"

/* --- HANDPATCH auf generiertem Code, 2026-08-12 -----------------------------
 * Warum: die MCU-Zeitbasis hing am open-loop DFLL48M - kein XOSC, kein
 * DFLL-Closed-Loop, und OSCCTRL_Initialize()/DFLL_Initialize() waren leer.
 * Gemessen +601 ppm bzw. +783 ppm gegen die Master-Wallclock mit ~180 ppm
 * Wanderung in 20 Minuten. Damit ist SYS_TIME als Frequenzreferenz unbrauchbar
 * und PTP_TIMEBASE_PLAN.md Phase A nicht messbar (test_results.md, Phase A).
 *
 * Was: XOSC0 im EXTERNTAKT-Modus (XTALEN=0) als DPLL0-Referenz.
 *
 * Auf XIN0 liegen 50 MHz, NICHT die 12 MHz. Gemessen, nicht angenommen:
 * GCLK-Generator 3 aus XOSC0 -> TC2 (32 Bit, GCLK-Kanal 26, unabhaengig von
 * TC0/SYS_TIME) -> 1 100 457 527 Zaehlwerte in 21,945 s = 50,147 MHz. Es ist
 * also der DSC1001CI2-050.0000, der RMII-Referenztakt. Ein erster Versuch mit
 * der 12-MHz-Annahme (DIV=5) liess den DPLL nicht einrasten und das Board nicht
 * booten.
 *
 * Rechnung: DIV=9  -> 50 MHz / (2*(9+1)) = 2,5 MHz Referenz (Bereich 32 kHz bis
 *           3,2 MHz), LDR=47 -> 2,5 MHz * 48 = 120 MHz exakt, ohne LDRFRAC.
 *           GCLK0 = 120 MHz und GCLK1 = 60 MHz bleiben damit unveraendert.
 *
 * ACHTUNG: plib_clock.c ist GENERIERT. Das MCC-Modell fuehrt gar keine
 * Taktkomponente (weder in core.yml noch als eigenes yml), der Clock
 * Configurator stand nie auf etwas anderem als dem Default - ein Modell-Patch
 * nach dem MCC-Runbook haette Symbole erfinden muessen. Ein "Generate Code"
 * entfernt diesen Patch also lautlos. Dauerhaft gehoert er in den Clock
 * Configurator: XOSC0 an, External Clock, 50 MHz, DPLL0-Referenz = XOSC0.
 * -------------------------------------------------------------------------- */
static bool clk_xosc0_ready = false;

static void OSCCTRL_Initialize(void)
{
    uint32_t guard;

    /* ENABLE, XTALEN=0 (Externtakt), ONDEMAND=0 (laeuft immer), STARTUP=0:
     * ein anliegender Takt braucht keine Oszillator-Anlaufzeit. Ein zu grosses
     * STARTUP liess XOSCRDY0 beim ersten Probe-Versuch scheinbar ausbleiben. */
    OSCCTRL_REGS->OSCCTRL_XOSCCTRL[0] = OSCCTRL_XOSCCTRL_ENABLE_Msk;

    for (guard = 0u; guard < 1000000u; guard++)
    {
        if ((OSCCTRL_REGS->OSCCTRL_STATUS & OSCCTRL_STATUS_XOSCRDY0_Msk) != 0u)
        {
            clk_xosc0_ready = true;
            break;
        }
    }

    if (!clk_xosc0_ready)
    {
        OSCCTRL_REGS->OSCCTRL_XOSCCTRL[0] = OSCCTRL_XOSCCTRL_RESETVALUE;
    }
}

static void OSC32KCTRL_Initialize(void)
{

    OSC32KCTRL_REGS->OSC32KCTRL_RTCCTRL = OSC32KCTRL_RTCCTRL_RTCSEL(0U);
}

static void FDPLL0_Initialize(void)
{
    GCLK_REGS->GCLK_PCHCTRL[1] = GCLK_PCHCTRL_GEN(0x2U)  | GCLK_PCHCTRL_CHEN_Msk;
    while ((GCLK_REGS->GCLK_PCHCTRL[1] & GCLK_PCHCTRL_CHEN_Msk) != GCLK_PCHCTRL_CHEN_Msk)
    {
        /* Wait for synchronization */
    }

    /****************** DPLL0 Initialization  *********************************/

    /* Versuch 1: XOSC0 als Referenz. Der Lock-Wartelauf ist BEGRENZT - beim
     * ersten Anlauf dieses Patches (mit der falschen 12-MHz-Annahme) rastete der
     * DPLL nicht ein und der unbegrenzte Wartelauf machte das Board tot. Ein
     * falscher Takt darf hoechstens den Ruecksprung auf den DFLL-Weg kosten. */
    if (clk_xosc0_ready)
    {
        uint32_t guard;

        OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLCTRLB = OSCCTRL_DPLLCTRLB_FILTER(0U) | OSCCTRL_DPLLCTRLB_LTIME(0x0U)
                                                | OSCCTRL_DPLLCTRLB_REFCLK(OSCCTRL_DPLLCTRLB_REFCLK_XOSC0_Val)
                                                | OSCCTRL_DPLLCTRLB_DIV(9U) ;

        OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLRATIO = OSCCTRL_DPLLRATIO_LDRFRAC(0U) | OSCCTRL_DPLLRATIO_LDR(47U);

        while((OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLSYNCBUSY & OSCCTRL_DPLLSYNCBUSY_DPLLRATIO_Msk) == OSCCTRL_DPLLSYNCBUSY_DPLLRATIO_Msk)
        {
            /* Waiting for the synchronization */
        }

        OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLCTRLA = OSCCTRL_DPLLCTRLA_ENABLE_Msk   ;

        while((OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLSYNCBUSY & OSCCTRL_DPLLSYNCBUSY_ENABLE_Msk) == OSCCTRL_DPLLSYNCBUSY_ENABLE_Msk )
        {
            /* Waiting for the DPLL enable synchronization */
        }

        for (guard = 0u; guard < 1000000u; guard++)
        {
            if ((OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLSTATUS & (OSCCTRL_DPLLSTATUS_LOCK_Msk | OSCCTRL_DPLLSTATUS_CLKRDY_Msk))
                    == (OSCCTRL_DPLLSTATUS_LOCK_Msk | OSCCTRL_DPLLSTATUS_CLKRDY_Msk))
            {
                break;
            }
        }

        if (guard >= 1000000u)
        {
            /* Kein Lock: DPLL abschalten, XOSC0 aufgeben, unten den DFLL-Weg
             * nehmen. Das Board bootet dann mit der schlechten, aber
             * funktionierenden Zeitbasis - statt gar nicht. */
            OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLCTRLA = 0U;
            while((OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLSYNCBUSY & OSCCTRL_DPLLSYNCBUSY_ENABLE_Msk) == OSCCTRL_DPLLSYNCBUSY_ENABLE_Msk )
            {
                /* Waiting for the DPLL disable synchronization */
            }
            OSCCTRL_REGS->OSCCTRL_XOSCCTRL[0] = OSCCTRL_XOSCCTRL_RESETVALUE;
            clk_xosc0_ready = false;
        }
    }

    /* Versuch 2 / Rueckfall: der urspruenglich generierte Weg, Referenz ist der
     * GCLK-Kanal 1 aus Generator 2 (DFLL48M/48 = 1 MHz), LDR=119 -> 120 MHz. */
    if (!clk_xosc0_ready)
    {
        /* Configure DPLL    */
        OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLCTRLB = OSCCTRL_DPLLCTRLB_FILTER(0U) | OSCCTRL_DPLLCTRLB_LTIME(0x0U)| OSCCTRL_DPLLCTRLB_REFCLK(0U) ;


        OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLRATIO = OSCCTRL_DPLLRATIO_LDRFRAC(0U) | OSCCTRL_DPLLRATIO_LDR(119U);

        while((OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLSYNCBUSY & OSCCTRL_DPLLSYNCBUSY_DPLLRATIO_Msk) == OSCCTRL_DPLLSYNCBUSY_DPLLRATIO_Msk)
        {
            /* Waiting for the synchronization */
        }

        /* Enable DPLL */
        OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLCTRLA = OSCCTRL_DPLLCTRLA_ENABLE_Msk   ;

        while((OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLSYNCBUSY & OSCCTRL_DPLLSYNCBUSY_ENABLE_Msk) == OSCCTRL_DPLLSYNCBUSY_ENABLE_Msk )
        {
            /* Waiting for the DPLL enable synchronization */
        }

        while((OSCCTRL_REGS->DPLL[0].OSCCTRL_DPLLSTATUS & (OSCCTRL_DPLLSTATUS_LOCK_Msk | OSCCTRL_DPLLSTATUS_CLKRDY_Msk)) !=
                    (OSCCTRL_DPLLSTATUS_LOCK_Msk | OSCCTRL_DPLLSTATUS_CLKRDY_Msk))
        {
            /* Waiting for the Ready state */
        }
    }
}


static void DFLL_Initialize(void)
{
}


static void GCLK0_Initialize(void)
{

    /* selection of the CPU clock Division */
    MCLK_REGS->MCLK_CPUDIV = MCLK_CPUDIV_DIV(0x01U);

    while((MCLK_REGS->MCLK_INTFLAG & MCLK_INTFLAG_CKRDY_Msk) != MCLK_INTFLAG_CKRDY_Msk)
    {
        /* Wait for the Main Clock to be Ready */
    }
    GCLK_REGS->GCLK_GENCTRL[0] = GCLK_GENCTRL_DIV(1U) | GCLK_GENCTRL_SRC(7U) | GCLK_GENCTRL_GENEN_Msk;

    while((GCLK_REGS->GCLK_SYNCBUSY & GCLK_SYNCBUSY_GENCTRL_GCLK0) == GCLK_SYNCBUSY_GENCTRL_GCLK0)
    {
        /* wait for the Generator 0 synchronization */
    }
}

static void GCLK1_Initialize(void)
{
    GCLK_REGS->GCLK_GENCTRL[1] = GCLK_GENCTRL_DIV(2U) | GCLK_GENCTRL_SRC(7U) | GCLK_GENCTRL_GENEN_Msk;

    while((GCLK_REGS->GCLK_SYNCBUSY & GCLK_SYNCBUSY_GENCTRL_GCLK1) == GCLK_SYNCBUSY_GENCTRL_GCLK1)
    {
        /* wait for the Generator 1 synchronization */
    }
}

static void GCLK2_Initialize(void)
{
    GCLK_REGS->GCLK_GENCTRL[2] = GCLK_GENCTRL_DIV(48U) | GCLK_GENCTRL_SRC(6U) | GCLK_GENCTRL_GENEN_Msk;

    while((GCLK_REGS->GCLK_SYNCBUSY & GCLK_SYNCBUSY_GENCTRL_GCLK2) == GCLK_SYNCBUSY_GENCTRL_GCLK2)
    {
        /* wait for the Generator 2 synchronization */
    }
}

void CLOCK_Initialize (void)
{
    /* MISRAC 2012 deviation block start */
    /* MISRA C-2012 Rule 2.2 deviated in this file.  Deviation record ID - H3_MISRAC_2012_R_2_2_DR_2 */

    /* Function to Initialize the Oscillators */
    OSCCTRL_Initialize();

    /* Function to Initialize the 32KHz Oscillators */
    OSC32KCTRL_Initialize();

    DFLL_Initialize();
    GCLK2_Initialize();
    FDPLL0_Initialize();
    GCLK0_Initialize();
    GCLK1_Initialize();

    /* MISRAC 2012 deviation block end */

    /* Selection of the Generator and write Lock for SERCOM0_CORE */
    GCLK_REGS->GCLK_PCHCTRL[7] = GCLK_PCHCTRL_GEN(0x1U)  | GCLK_PCHCTRL_CHEN_Msk;

    while ((GCLK_REGS->GCLK_PCHCTRL[7] & GCLK_PCHCTRL_CHEN_Msk) != GCLK_PCHCTRL_CHEN_Msk)
    {
        /* Wait for synchronization */
    }
    /* Selection of the Generator and write Lock for SERCOM1_CORE */
    GCLK_REGS->GCLK_PCHCTRL[8] = GCLK_PCHCTRL_GEN(0x1U)  | GCLK_PCHCTRL_CHEN_Msk;

    while ((GCLK_REGS->GCLK_PCHCTRL[8] & GCLK_PCHCTRL_CHEN_Msk) != GCLK_PCHCTRL_CHEN_Msk)
    {
        /* Wait for synchronization */
    }
    /* Selection of the Generator and write Lock for TC0 TC1 */
    GCLK_REGS->GCLK_PCHCTRL[9] = GCLK_PCHCTRL_GEN(0x1U)  | GCLK_PCHCTRL_CHEN_Msk;

    while ((GCLK_REGS->GCLK_PCHCTRL[9] & GCLK_PCHCTRL_CHEN_Msk) != GCLK_PCHCTRL_CHEN_Msk)
    {
        /* Wait for synchronization */
    }

    /* Configure the AHB Bridge Clocks */
    MCLK_REGS->MCLK_AHBMASK = 0xffffffU;

    /* Configure the APBA Bridge Clocks */
    MCLK_REGS->MCLK_APBAMASK = 0x77ffU;


}
