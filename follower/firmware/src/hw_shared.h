/*******************************************************************************
  hw_shared.h - the three registers several modules genuinely share

  NOT A HAL.  That is explicitly not the goal (REGISTERZUGRIFF_PRINZIP.md
  5.4): an abstraction layer without an answered ownership question only
  pushes the problem up one level, and with the ownership question answered -
  it is in CONFIG_BASELINE.md 6a - it is superfluous for most registers.
  What is left are the class-B registers, i.e. those where a full write
  switches off a FOREIGN function, and the EIC, which has exactly one user
  today and two tomorrow.

  What does NOT belong here:
    * PORT OUTSET/OUTCLR/OUTTGL - class C, already atomic.  A wrapper would
      be slower and would suggest a sharing that does not exist.
    * TC1 and TC2 - class A, exactly one owner each.
    * LAN8651 - a foreign chip on SPI, asynchronous, its own serialization.
*******************************************************************************/

#ifndef HW_SHARED_H
#define HW_SHARED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- Class B */

/* Enable one bit in MCLK_APBAMASK without touching the others.

   Why not just `|=` at the call site: the read, the OR and the write-back
   are three steps.  Nothing runs in between during initialization, but it
   does later - and a lost race switches off a foreign module's clock here.
   In this project APBAMASK was nearly WRITTEN once with the bare EIC bit,
   which would have turned off TC0 and thereby SYS_TIME of the board being
   measured at that moment.

   Ordering: `plib_clock.c` writes APBAMASK ONCE, completely (0x77ff).
   Whoever enables their clock BEFORE `SYS_Initialize()` gets silently
   switched off again.  So this function may only run afterwards. */
void HW_ApbaClockEnable(uint32_t mask);

/* The same for APBBMASK (TC2 hangs off it). */
void HW_ApbbClockEnable(uint32_t mask);

/* The same for APBCMASK (TC4/TC5 hang off it, the capture pair of the
   cross-measurement).  Same reason for the interrupt bracket as above - and
   the same reason there is no bare `write32`: TCC0..TCC1 and the SERCOMs
   also hang off this bus, and a full write switches them off. */
void HW_ApbcClockEnable(uint32_t mask);

/* The same for APBDMASK - TC6 and TC7, the free timer pair, hang off it, and
   the LED's grid divider uses TC6.  This bus also carries TCC2..TCC4,
   ADC0/1, DAC, I2S, PCC, AC and SERCOM4..7; a bare write would therefore be
   the same mistake as on the other three buses, just with different
   victims. */
void HW_ApbdClockEnable(uint32_t mask);

/* Put a pin on a peripheral function.  `PORT_PMUX[n]` is one byte for TWO
   pins (even in the lower four bits, odd in the upper) - a full write would
   reconfigure the neighbouring pin too.  Also sets `PINCFG.PMUXEN`; `INEN`
   only if `input` is set. */
void HW_PinMux(uint8_t group, uint8_t pin, uint8_t func, bool input);

/* ---------------------------------------------------------------------- EIC */

/* Claim and set up one EXTINT.  Replaces the EIC setup block that sat in
   `pps_capture.c` until 2026-08-20, and does three things differently:

     1. `CTRLA = 0` (resetting the instance) only on the FIRST claim.  A
        second user would otherwise switch off the first.
     2. `INTENCLR`/`INTFLAG` only with the OWN bit instead of 0xFFFFFFFF - a
        full mask clears the enable and the flags of other EXTINTs too.
     3. A double claim on the same EXTINT is REPORTED (returns false)
        instead of silently overwritten.

   Today the EIC has exactly one user, and the old block was therefore
   correct.  It goes wrong without a single line changing, the moment a
   second EXTINT is added - that is the reason for this function, not
   tidiness.

   `sense` is the SENSE value from the datasheet (1 = RISE, 2 = FALL,
   3 = BOTH, 4 = HIGH, 5 = LOW).  `asynch` enables asynchronous detection, so
   a short pulse does not vanish between two clock edges.

   Returns false for: EXTINT >= 16, sense == 0 or > 5, or if this EXTINT is
   already claimed.  The interrupt is NOT enabled in the NVIC - priority and
   enable stay with the caller, because ordering is a property of the system,
   not of this register. */
bool HW_EicClaim(uint8_t extint, uint8_t sense, bool asynch);

/* Change the edge sense of an ALREADY CLAIMED EXTINT.

   Why separate from HW_EicClaim(): the claim explicitly rejects a second
   grant (a double claim is a programming error), and that should stay so.
   But whoever wants to reconfigure their OWN channel - such as the mirror
   mode in peer_capture.c, which switches between RISE and BOTH - needs a
   named path for that instead of an exception in the claim rule.

   `CONFIG` is enable-protected, so the instance briefly goes off and back
   on.  THIS HITS EVERY OTHER EXTINT TOO: a 1PPS pulse on EXTINT12 can be
   lost during it.  So this is a switch for bring-up or a diagnostic mode,
   not for running operation.

   Returns false if the EXTINT is not claimed or `sense` is outside 1..5.
   Only this channel's nibble is changed. */
bool HW_EicSenseSet(uint8_t extint, uint8_t sense);

/* Turn a claimed EXTINT's event output to EVSYS on or off.

   `EIC_EVCTRL` is enable-protected like `CONFIG`, so the instance must be
   off to write it - and exactly that dance belongs here and not in the
   module: it briefly switches off the WHOLE instance, affecting every other
   EXTINT, and a second copy of it in application code is a second place
   that has to get it right.  Only the own bit is changed.

   Returns false if this EXTINT is not claimed - whoever enables the event
   of someone else's pin has made a mistake. */
bool HW_EicEventEnable(uint8_t extint, bool on);

/* Who holds which EXTINT?  Bit n = EXTINT n is claimed.  For the diagnostic
   printout and for a test that checks the second-claim rejection. */
uint32_t HW_EicClaimed(void);

/* Counter of refused double claims.  A state counter you never print is one
   you do not have - hence it is in the header. */
uint32_t HW_EicRefused(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_SHARED_H */
