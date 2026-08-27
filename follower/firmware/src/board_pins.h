/*******************************************************************************
  board_pins.h - board facts for the SAM E54 Curiosity Ultra (DM320210)

  Why this file exists:

  A register access mixes facts from three worlds, and each has exactly one
  allowed source (REGISTERZUGRIFF_PRINZIP.md 5).  Controller facts - address,
  offset, bitmask - come from the DFP header and are therefore never typed by
  hand anywhere in the code any more.  BOARD facts are something the DFP
  cannot know: it has no idea that PC12 carries a 1PPS signal or that PC21 is
  an LED.  They are rightly local - but they belong in ONE place, not in
  every functional module that needs them.

  The concrete trigger: PA16 was called `PA16_*` in `pps_capture.c` and
  `LED2_*` in `ptp_trigger.c`.  Two names for the same pin, in two files -
  whoever puts the marker on PA16 while also blinking the LED is contending
  for the same pin, and the names give no hint of it.  Here it has one name.

  The provenance note on each pin is the most valuable part of this file.  It
  is here because a pin without a citation gets guessed at on the next board.
*******************************************************************************/

#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/* --------------------------------------------------------------- 1PPS input
 * PC12 = mikroBUS pin 13 (TX) on this board, peripheral A = EIC/EXTINT12.
 * Both facts are from LAN8651_1PPS_HARDWARE.md, which derived them from the
 * Curiosity Ultra schematic and the MikroE pinout table.
 *
 * This pin carries the LAN8651's 1PPS only after the Two-Wire ETH Click has
 * been modified: set R37 AND remove R40, both on the underside.  Without
 * that change nothing arrives here - and the firmware only sees it as
 * `edges: 0`.
 */
#define BOARD_PPS_GROUP         2u          /* PORTC                        */
#define BOARD_PPS_PIN           12u
#define BOARD_PPS_EXTINT        12u         /* EXTINT12, peripheral function A */

/* -------------------------------------------------------------- Trigger output
 * PD10 = EXT1 pin 5 ("GPIO1"), a through-hole pin - the probe point the
 * reference implementation in zabooh/net_10base_t1s also uses.
 */
#define BOARD_TRIG_GROUP        3u          /* PORTD                        */
#define BOARD_TRIG_PIN          10u
#define BOARD_TRIG_MASK         (1u << 10)
/* The same pin, seen from the protocol side: index 10 of the pin table. */
#define BOARD_TRIG_PIN_INDEX    10u

/* --------------------------------------------------- Neighbour edge (cross-measurement)
 * PD11 = EXT1 pin 6 ("GPIO2"), peripheral function A = EIC/EXTINT6.
 *
 * PROVENANCE, and it took work: the board user guide (DS70005405A) contains
 * NO ordered header table, only schematic pages - and their text extract is
 * unsorted and unusable as a source.  What was read instead was the
 * RENDERED schematic image on page 24 (EXT1 extension header, connector
 * J602).  Cross-check against the same table: pin 5 = PD10 = GPIO1, which
 * matches the probe point already in use since E6 - so the source is
 * correct at the one point that is independently known.
 *
 * CONFIRMED ON THE DEVICE (2026-08-20, `python scripts/test_peer_wiring.py`):
 * follower A drives pin 5, follower B reads bit 11 (IN 0x00000000 ->
 * 0x00000800), and the same in reverse.  Meanwhile bit 10 of the listener
 * stays at 0 - so there is no second, uncrossed connection.
 *
 * Pin 6 sits DIRECTLY OPPOSITE pin 5 on the 2x10 connector (odd pins in one
 * row, even in the other).  A crossed two-wire cable on 5/6 therefore
 * connects both directions in one go.  Ground: EXT1 pin 2 or 19, not
 * optional - it is the return path of the current and matters for the edge,
 * not for the level.
 *
 * Fallback if pin 6 is taken: PD12 = EXT1 pin 9 = EXTINT7.
 */
#define BOARD_PEER_GROUP        3u          /* PORTD                        */
#define BOARD_PEER_PIN          11u
#define BOARD_PEER_MASK         (1u << 11)
#define BOARD_PEER_EXTINT       6u          /* EXTINT6, peripheral function A */

/* ----------------------------------------------------------------------- LEDs
 * Board identification LEDs.  Taken from the official BSP of this very board
 * (Harmony3 bsp repo, boards/sam_e54_cult/config/bsp.py), NOT from memory and
 * not from the Xplained Pro variant, whose LED sits elsewhere:
 *     pin 75 = PC21 = LED1      pin 66 = PA16 = LED2      both LED_AL
 * "LED_AL" is active low, and the BSP sets LAT=High, i.e. dark after reset.
 * Cross-checked against pin_configurations.csv of this project, where pins 75
 * and 66 read PC21/PA16 and are "Available" - so nothing here contends for them.
 * The board user guide does not name the pins at all; only the BSP does.
 *
 * The purpose is prosaic bench hygiene: with three boards and three terminals
 * open, `tbase led blink` answers "which window is this board?".
 *
 * WATCH OUT, LED2 = PA16: the same pin serves as a second marking spot for
 * the latency measurement in `pps_capture.c` (`PPS_MARK_LED`).  Both at once
 * do not work - `tbase led` must be off for that.
 */
#define BOARD_LED1_GROUP        2u          /* PORTC                        */
#define BOARD_LED1_MASK         (1u << 21)  /* PC21                         */
#define BOARD_LED2_GROUP        0u          /* PORTA                        */
#define BOARD_LED2_MASK         (1u << 16)  /* PA16                         */
#define BOARD_LED_COUNT         2u

/* ------------------------------------------------------------- Buttons SW1 / SW2
 * Both citations, because a pin without one gets guessed at on the next board:
 *
 *   - WHICH PINS: the Harmony BSP `boards/sam_e54_cult/config/bsp.py` sets
 *     `SWITCH1` to **PD00** and `SWITCH2` to **PD01**, both as `SWITCH_AL` -
 *     i.e. ACTIVE LOW - with `LAT High` and `INEN True`, meaning an input
 *     with a PULL-UP.  The board user guide does not name the pins; only the
 *     BSP does.
 *   - WHICH EXTINT: the device pack's ATDF (SAME54_DFP 3.9.244) lists
 *     `<signal group="EXTINT" index="0" function="A" pad="PD00"/>` and the
 *     same with index 1 for PD01.  So **EXTINT0 and EXTINT1, peripheral
 *     function A**.
 *
 * A press pulls the pin to ground, so the edge of interest is the FALLING
 * one (SENSE = 2).
 *
 * WATCH OUT, shared EXTINT numbers: EXTINT0 sits not only on PD00 but also on
 * PA00, PA16, PB00, PB16, PC00 and PC16 - and **PA16 is LED2**.  No conflict
 * today, because the LED is a plain output and does not claim the EIC path;
 * but whoever ever puts PA16 on peripheral function A contends with SW1 for
 * EXTINT0.
 *
 * PORT group 3 is NOT TOUCHED AT ALL by the generated `plib_port.c` (the
 * "GROUP 3 Initialization" block is empty).  These pins are therefore in
 * their reset state and are configured entirely by hand - like PD10 and
 * PD11 next to them.
 */
#define BOARD_BTN_GROUP         3u          /* PORTD                        */
#define BOARD_BTN1_PIN          0u          /* PD00, SWITCH1                */
#define BOARD_BTN1_MASK         (1u << 0)
#define BOARD_BTN1_EXTINT       0u          /* EXTINT0, peripheral function A */
#define BOARD_BTN2_PIN          1u          /* PD01, SWITCH2                */
#define BOARD_BTN2_MASK         (1u << 1)
#define BOARD_BTN2_EXTINT       1u          /* EXTINT1, peripheral function A */
#define BOARD_BTN_COUNT         2u

#endif /* BOARD_PINS_H */
