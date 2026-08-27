/*******************************************************************************
  Trigger commands from the master - UDP receiver on the follower

  File Name:
    trig_cmd.h

  Summary:
    Listens on UDP 30509 for SOME/IP-formatted commands and hands them to
    ptp_trigger.c.  Phase D of PTP_TIMEBASE_PLAN.md.

  Description:
    Until this existed, every trigger was armed by hand over the serial console,
    on each board separately.  That is the opposite of what the plan is for: the
    master is supposed to say "action N at instant Tx" and several followers are
    supposed to carry it out together.

    The trigger interface was built for this from the start - ScheduleAt() and
    SchedulePeriodic() already take a cmd_seq, which is where the SOME/IP
    Session ID goes - so this module is only the receiver in front of it.  It
    contains no timing logic whatsoever, and that is the point: D.1 says a
    command must arrive in time, not on time.

    Control and time are separate paths.  Sync and Follow_Up stay raw L2
    (0x88F7); only the commands are IP, so they can be addressed, discovered and
    read with ordinary tools.
*******************************************************************************/

#ifndef TRIG_CMD_H
#define TRIG_CMD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Does not open the socket - the stack is not up yet at init time.  Tasks()
   opens it on the first attempt that succeeds. */
void TRIGCMD_Initialize(void);

/* Main loop: drains the socket and dispatches.  Never blocks. */
void TRIGCMD_Tasks(void);

/* A raw Ethernet frame of TRIG_ETHERTYPE arrived on eth0.  `frame` points at the
   MAC header, so the source MAC - the only address this path needs - is read from
   the frame itself.  Called from the eth0 packet handler in app.c.

   This is the bootstrap transport: it works before the board has a usable IP,
   which matters because in the factory state every follower shares the bridge's
   address and a unicast reply would never leave the board (see trig_someip.h). */
void TRIGCMD_L2Rx(const uint8_t *frame, uint16_t len);

/* Send a BUTTON event to the master - the only message this board sends
   UNSOLICITED.  Called from button.c, from the main loop.

   Lives here because the wire format, the raw sender, the node number and
   the master MAC live here: button.c recognises and dates, this module
   sends.

   `usable` is the time base's quality at the moment of the press and is
   SENT ALONG instead of suppressing the press - a button press that goes
   unreported because of a bad clock is the defect one goes looking for
   afterwards.

   Returns false if the frame did not go out (counter in `tbase cmd`). */
bool TRIGCMD_ButtonEvent(uint8_t btn, bool usable, uint32_t seq, uint64_t ts_ns);

/* Served from the 'tbase' group, like the trigger and 1PPS subcommands:
   MAX_CMD_GROUP in the generated sys_command.h is 8 and both projects sit at
   the ceiling, so a group of its own would be refused - quietly, from the
   caller's point of view.  Returns true if it consumed the arguments. */
bool TRIGCMD_CliTry(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* TRIG_CMD_H */
