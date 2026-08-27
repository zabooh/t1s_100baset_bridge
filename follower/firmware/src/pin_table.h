/*******************************************************************************
  Pin table: which protocol index sits on which pin, and who owns it

  File Name:
    pin_table.h

  Summary:
    One row per offered pin index.  Adding a pin means adding a row - neither
    the dispatcher, the encoding, nor the return codes know how many pins
    there are (some_ip.md 8.11.1).

  Why a table and not a switch:
    A switch spreads the answer to "does index n exist?" across the whole
    code, and every extension has to find it in several places.  The table
    is the one source of truth, and the protocol says the same thing
    everywhere that the table says.

  The ceiling comes from the foreign protocol and is hard: 16.
    `PinId` is a uint8_t there, documented as "PA00 to PA15",
    `ReleaseDigitalPinsVar_t.PinIdList` is uint8_t[16], and 0xFF is taken as
    "unused pin".  So the index space is 0..15; whoever exceeds it is
    non-conformant.

  The choice of indices is NOT free:
    It follows from the foreign tools' own defaults, so that
    `lan866x-ledblink` with its own defaults `2,6,10` runs through literally -
    two LEDs blink visibly, the third slot toggles the scope probe pin.  That
    is a conformance proof you can SEE without reading an instrument.

  Active-low is NOT abstracted away, it is noted in the table:
    `SetGpio` with value 1 must mean "logically on", or a conformant tool
    shows the opposite.  The inversion therefore belongs in the pin
    description, not in the protocol path.
*******************************************************************************/

#ifndef PIN_TABLE_H
#define PIN_TABLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_INDEX_MAX     16u     /* 0..15, 0xFF is "unused" (foreign protocol) */
#define PIN_INDEX_UNUSED  0xFFu

/* Who currently drives the pin.  Trigger and PWM have been THE SAME MEANS
 * since the decision for software PWM - a schedule on the grid that drives
 * the pin -, and a pin can have only one.  Exclusivity is therefore no
 * longer a hardware quirk but the definition of the resource, and it holds
 * for EVERY pin in the table, not just the probe pin. */
typedef enum
{
    PIN_OWNER_NONE = 0,
    PIN_OWNER_TRIGGER,      /* ptp_trigger.c, TC1 compare via EVSYS               */
    PIN_OWNER_GPIO,         /* OpenGpio handle                                    */
    PIN_OWNER_PWM,          /* OpenPwm handle                                     */
    PIN_OWNER_BLINK,        /* `tbase led` - the bench identification marker       */
    /* Trigger action 4: toggle LED1 ON THE GRID, started from the bridge
       (`trigper 4 500ms`).  Its own owner, not shared with PIN_OWNER_TRIGGER:
       that one's comment says "TC1 compare via EVSYS", and that is
       specifically NOT the path here - the LED is written from the firing
       path.  An owner name that denotes two different paths makes
       `tbase pins` worthless as information. */
    PIN_OWNER_GRIDLED,
    /* The GRID DIVIDER: TC6 counts the grid events and flips LED1 every Nth
       one via the event system - the same chain as PD10, just with a
       divider in front, and no CPU.
       Its OWN owner, not shared with GRIDLED even though both drive the same
       LED: `PIN_Claim()` lets the same owner re-register, so the two paths
       would write to the pin at the same time.  Kept separate they exclude
       each other, and `tbase pins` says which one is currently driving. */
    PIN_OWNER_GRIDDIV
} PIN_OWNER;

typedef struct
{
    uint8_t  index;         /* protocol index 0..15                               */
    uint8_t  group;         /* PORT group (0=PA, 1=PB, 2=PC, 3=PD)                */
    uint8_t  pin;           /* pin within the group                               */
    bool     invert;        /* active low: logical 1 means level 0                */
    bool     can_pwm;       /* schedule-capable (trigger/PWM)                     */
    const char *name;       /* for the console, not for the protocol              */
} PIN_ROW;

void      PIN_Initialize(void);

/* Row for an index, or NULL.  For the protocol, NULL means
   RT_NOT_REACHABLE 0x05 ("peripheral not configured on that node") - exactly
   the code their own firmware uses for it. */
const PIN_ROW *PIN_Find(uint8_t index);

/* Register and release a claim.  Claim fails if a DIFFERENT owner holds the
   pin; the same owner may register again (idempotent), so a repeated
   command does not fail. */
bool      PIN_Claim(uint8_t index, PIN_OWNER who);
void      PIN_Release(uint8_t index, PIN_OWNER who);
PIN_OWNER PIN_OwnerGet(uint8_t index);

/* Set and read the logical level - the inversion happens here, so it never
   needs to be considered anywhere else. */
void      PIN_Set(const PIN_ROW *row, bool on);
bool      PIN_Get(const PIN_ROW *row);

/* Print the table for the console (`tbase pins`). */
void      PIN_Print(void);

/* Name of an owner for messages - so every place that reports a conflict
   uses the same name as `tbase pins`. */
const char *PIN_OwnerName(PIN_OWNER o);

#ifdef __cplusplus
}
#endif
#endif /* PIN_TABLE_H */
