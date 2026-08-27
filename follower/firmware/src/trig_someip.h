/*******************************************************************************
  Wire format for trigger commands - SOME/IP over UDP

  File Name:
    trig_someip.h

  Summary:
    The 16-byte SOME/IP header and the payloads the master uses to command
    "action N at instant Tx" to one or all followers.

  Description:
    Specified in PTP_TIMEBASE_PLAN.md D.2 to D.5; this header is the executable
    copy of that table and nothing here may drift from it.

    THE SAME FILE EXISTS IN BOTH TREES - firmware/src/ (master) and
    follower/firmware/src/ (follower).  They are separate MPLAB projects with no
    shared include path, so the copy is deliberate: `diff` across the two trees
    makes any divergence visible, which a pair of privately defined constants
    would not.

    WHY SOME/IP AND NOT SOMETHING SIMPLER.  Two things come for free and both
    are load-bearing here:

      - REQUEST_NO_RETURN (0x01) is the standard's fire-and-forget message type.
        This project's one-way rule stops being a shortcut and becomes a
        documented mode.  What is still missing is a reply - see the risk list
        in the plan.
      - The Session ID doubles as the deduplication key that C.7 requires, so
        no private sequence field is needed.  It is passed straight into
        PTP_TRIG_ScheduleAt()'s cmd_seq parameter, which exists for exactly this.

    WHY TIMING DOES NOT MATTER HERE (D.1).  A command must arrive IN TIME, not
    ON TIME.  UDP jitter, stack latency and main-loop delay are all irrelevant:
    the precision lives in tx_ns, an absolute grandmaster instant, not in the
    delivery.  That is also why one broadcast frame can make every follower fire
    together - they are not reacting to the frame's arrival, they are acting on
    the instant it names.
*******************************************************************************/

#ifndef TRIG_SOMEIP_H
#define TRIG_SOMEIP_H

#include <stdint.h>

/* D.3: broadcast for the group, unicast for the individual.  Deliberately NOT
   multicast - broadcast is always accepted by the MAC filter, while IPv4
   multicast depends on the stub filter and therefore on promiscuous mode.  The
   critical path must not hang on a line somebody may switch off for
   performance. */
/* BOOTSTRAP RUNS ON RAW LAYER 2, NOT ON IP.
 *
 * Measured 2026-08-14, and it is the reason this exists: in the factory state
 * every follower carries the compile default 192.168.0.200 - the same address the
 * BRIDGE uses on eth0.  A follower answering "to whoever asked" is then answering
 * to its own address; the stack accepts it, loops it back internally, and nothing
 * leaves the board.  `status replies: 3   failed to send: 0` on the follower, zero
 * reply frames on the mirror, and no counter anywhere revealing it - the same kind
 * of invisible fault as the duplicate node id.
 *
 * The fix is not to detect that case but to remove the dependency: **IP
 * presupposes exactly the identity that bootstrap is supposed to establish.**  So
 * STATUS and ASSIGN travel as raw Ethernet frames addressed by MAC - the one
 * identity a factory board already has, because it comes from the chip serial.
 * The reply goes to the SOURCE MAC of the request frame, so no address needs to
 * be configured, resolved or assumed anywhere.
 *
 * The bytes inside are the SAME SOME/IP message as on UDP: one wire format, one
 * parser on the follower, one Wireshark table.  Only the transport differs.
 *
 * The timing commands (SCHEDULE, SCHEDULE_PERIODIC, CANCEL, MODE) stay on UDP:
 * by the time they are used the identities exist, and UDP goes through
 * DRV_LAN865X_PacketTx() where the mirror hook already sits (plan D.4).  A raw
 * send bypasses that hook, which is why the raw path calls MIRROR_RawTx()
 * explicitly. */
#define TRIG_ETHERTYPE          0x88B6u   /* 0x88B5 is taken by noip_test.c */
#define TRIG_ETH_HDR_LEN        14u
/* Ethernet's 60-byte minimum, padded rather than left to the driver so a capture
   shows the same length every time. */
#define TRIG_ETH_MIN_FRAME      60u

#define TRIG_UDP_PORT           30509u

/* Replies go to a port of their own rather than back to the request's source
   port.  The master sends its commands from a CLIENT socket aimed at the
   broadcast address; making that same socket receive unicast answers from three
   different followers means fighting the socket's remote-address binding.  A
   dedicated server socket on the master is simpler, and it makes the two
   directions trivially separable in a capture. */
#define TRIG_REPLY_PORT         30510u

/* Shortest grid period a follower accepts - part of the contract, because
   BOTH sides check it: the master, so an operator sees the answer at once,
   and the follower, as the binding authority.  A second copy of the number
   would be a second source of truth, hence it lives here.

   LOWERED FROM 100 us TO 40 us ON 2026-08-19 - because the CAUSES that forced
   the old value are gone, not because the limit was too strict.  40 us is the
   lowest SAMPLED value at which no board skips a period and both channels
   carry the same edge count; at 35 us they reproducibly stop doing so.  The
   series and both measurements are in ptp_trigger.h at
   PTP_TRIG_MIN_PERIOD_US. */
#define TRIG_MIN_PERIOD_US      40u

#define TRIG_SERVICE_ID         0x0865u

#define TRIG_METHOD_SCHEDULE    0x0001u   /* payload: action, rsv, tx_ns       */
#define TRIG_METHOD_CANCEL      0x0002u   /* payload: action, rsv              */
#define TRIG_METHOD_SCHED_PER   0x0003u   /* payload: action, rsv, per, phase  */
/* STATUS is the answer to "who is out there, and did my last command land?".
   It replaces the mDNS discovery of plan D.6, which was dropped: the Harmony
   mDNS module is a RESPONDER ONLY - no browse, no query - so the master could
   never have used it to find anyone.  See MDNS_HANDPATCH_PLAN.md 0.

   This is strictly better for the master's purpose.  It needs no new stack
   module, no multicast (which on this bus depends on promiscuous mode), and no
   MCC.  And because the reply carries the LAST SESSION ID AND ITS RESULT, one
   STATUS after a command tells the master whether that specific command was
   accepted - the acknowledgement the plan lists as an open risk - without
   paying for a reply on every command. */
#define TRIG_METHOD_STATUS      0x0004u   /* request: no payload               */

/* ASSIGN hands a follower its identity, addressed BY MAC.
 *
 * This exists because of a measured fact, not a theoretical one: a fleet boots
 * with identical configuration apart from the MAC, and two nodes sharing a PLCA
 * node id lose 68 % of their traffic while every one of them reports zero send
 * errors and a healthy PLCA_STATUS (PLCA_BOOTSTRAP_RESULTS.md T4).  Nobody can
 * detect it from the inside, so backoff cannot resolve it - the collision is
 * deterministic, one per PLCA cycle, not a random contention.
 *
 * What makes a master-driven assignment work anyway is the asymmetry the same
 * tests measured: RECEPTION IS INDEPENDENT of a node's own PLCA state (T3), so
 * the downlink is reliable even while the uplink is wrecked.  A broadcast
 * ASSIGN therefore always lands; only the STATUS answers coming back need
 * repeating, and there a third still gets through. */
#define TRIG_METHOD_ASSIGN      0x0005u   /* payload: mac, id, cnt, ip, flags  */

/* MODE puts a follower into a given OPERATING state - as opposed to ASSIGN,
   which gives it an IDENTITY.  The distinction is why this is deliberately
   volatile and never touches the EEPROM: what a board IS should survive a power
   cut, what it is currently DOING should not.

   Its real value is that it makes trigwho actionable.  Today a board that
   reports "usable: NO" has to be walked to; with this the same window that shows
   the problem can fix it, which is the difference between operable and not once
   there are more than two boards. */
#define TRIG_METHOD_MODE        0x0006u   /* payload: listen, source, pin, hw  */

/* RESET restarts a follower - the zero state on demand.
 *
 * The purpose is a demonstration that starts reproducibly: `trigper <a> <p> r`
 * resets the fleet and then waits until it is reachable AND locked again.
 * Without it every board has to be touched by hand, and the starting state is
 * an assumption instead of a fact.
 *
 * Deliberately WITHOUT a payload and without a reply: the proof that it worked
 * is the fresh STATUS answer afterwards - an acknowledgement from a board that
 * is about to restart would be worthless anyway.  Additive to the wire format:
 * a new method id changes no existing layout, and an old follower ignores it. */
#define TRIG_METHOD_RESET       0x0007u   /* payload: node(1) rsv(3), or none  */

/* RESET's payload, since 2026-08-19 - the method had none before.
 *
 * Length 0 still means ALL, so the existing broadcast stays valid.  With a
 * payload, only the board with that PLCA number resets; 0 also means all,
 * because 0 is the coordinator's number (the bridge) and cannot be a
 * follower. */
#define TRIG_PL_RESET           4u        /* node(1) rsv(3)                    */

/* BUTTON is the FIRST message a follower sends UNSOLICITED.
 *
 * Everything else in this file is master-driven: the bridge commands, the
 * follower executes, and the only return traffic is the STATUS reply - which
 * exists only because it was asked for.  A button press has nobody asking.
 *
 * HENCE AN EVENT AND NOT AN EIGHTH METHOD.  The standard separates methods
 * (0x0000..0x7FFF) from events (0x8000..0xFFFF), and NOTIFICATION is the
 * message type for "unsolicited" - the same line that justifies
 * REQUEST_NO_RETURN above, and the reason a RESPONSE to a REQUEST_NO_RETURN
 * is rejected as non-conformant.
 *
 * TRANSPORT IS RAW ETHERNET, for the same reason as bootstrap: no socket, no
 * address, no ARP.  The bridge did NOT have to become a SOME/IP server for
 * this - it already has the return path, because `app.c` unconditionally
 * hands every frame with this EtherType to TRIGM_L2Rx().  What it needed was
 * a branch BEFORE its reply window: that window is open only while it is
 * waiting for STATUS answers, and an unsolicited event would have been
 * silently dropped there.
 *
 * DESTINATION MAC: the master's, if a command from it has already arrived,
 * broadcast otherwise.  Broadcast has no precondition - and its one side
 * effect is handled: other followers drop NOTIFICATION early, or they would
 * count their neighbour's event as `malformed`. */
#define TRIG_EVENT_BUTTON       0x8001u   /* payload: node, btn, usable, seq, ts */

/* BUTTON event payload, big endian:
     0  u16  node_id      PLCA number - which board was pressed
     2  u8   btn          1 = SWITCH1 (PD00), 2 = SWITCH2 (PD01)
     3  u8   tb_usable    1 = the time base was usable at the moment of the
                          press.  0 means: ts_ns is INVALID and is reported as
                          such rather than concealed - a suppressed report
                          would be the very defect one goes looking for
                          afterwards
     4  u32  seq          the board's press counter.  No loss protection, but
                          a lost frame becomes visible as a gap - the same
                          choice as with REQUEST_NO_RETURN
     8  u64  ts_ns        grandmaster time of the button press.  Formed on the
                          follower: the ISR takes the local tick, the
                          conversion is done afterwards, at leisure, by
                          PTP_TB_Convert()                                     */
#define TRIG_PL_BUTTON          16u

#define TRIG_PROTO_VER          0x01u
#define TRIG_IFACE_VER          0x01u
/* STATUS is the only command that gets ANSWERED, so the only true REQUEST.  A
   RESPONSE to a REQUEST_NO_RETURN contradicts the standard - 0x01 means
   precisely "no reply is coming" (plan 12.5). */
#define TRIG_MSGTYPE_REQUEST    0x00u     /* REQUEST, answered                 */
#define TRIG_MSGTYPE_REQ_NORET  0x01u     /* REQUEST_NO_RETURN                 */
#define TRIG_MSGTYPE_RESPONSE   0x80u     /* RESPONSE, for STATUS replies      */
/* NOTIFICATION: unsolicited, no reply expected.  Only BUTTON uses it. */
#define TRIG_MSGTYPE_NOTIFY     0x02u     /* NOTIFICATION, for events          */
#define TRIG_RETCODE_OK         0x00u

/* Header, big endian:
     0  u16  Service ID
     2  u16  Method ID
     4  u32  Length            = 8 + payload
     8  u16  Client ID
    10  u16  Session ID        <- dedup key
    12  u8   Protocol Version
    13  u8   Interface Version
    14  u8   Message Type
    15  u8   Return Code
    16  ...  payload                                                          */
#define TRIG_HDR_LEN            16u

/* The Length field counts from the Request ID onwards, NOT the whole message -
   a documented trap in the plan's fallstricke section, and the reason this is a
   named constant rather than an inline 8. */
#define TRIG_LEN_FIXED_PART     8u

/* THE BYTE AT OFFSET 2 IS THE NODE NUMBER, 0 = ALL.
   It sat unused as part of `reserved` and was explicitly written as 0 by BOTH
   senders - so the field is demonstrably free and demonstrably zero, not
   merely probably unused.  The whole appeal follows from that: payload
   lengths stay unchanged, the buffers and TRIG_PL_MAX stay untouched, and an
   old master (which sends 0) keeps hitting everyone.
   0 cannot be a follower number - it is the coordinator's, i.e. the bridge's.
   The meaning "all" is therefore not chosen, it is free.
   u8, because PLCA numbers are 1..255 and RESET/ASSIGN hold to the same rule -
   the same width for the same size (TRIG_ADRESSIERUNG_PLAN.md E1/E2). */
#define TRIG_PL_SCHEDULE        12u       /* action(2) node(1) rsv(1) tx_ns(8)   0 = all */
#define TRIG_PL_CANCEL          4u        /* action(2) node(1) rsv(1)            0 = all */
/* SCHEDULE_PERIODIC carries an ABSOLUTE start instant, and that is not
   convenience, it is the fix for a measured defect.

   Without it every follower picks its own first firing point from its own
   `now`.  Because both boards process the same command in different main-loop
   passes, the minimum-lead-time correction can add one extra step on one of
   them - and then it fires exactly one grid period later.  Measured
   (FIXPLAN_10KHZ.md T1): 3 of 30 armings with k = +-1, a 0.4 % deviation from
   the whole number.

   A grid was tried as a way out, and measured: it makes it WORSE (4 of 10 at
   100 us), because quantizing only replaces one hard threshold with a
   coarser one.  Only a number BOTH boards receive from the master hangs off
   nothing local any more.

   start_ns = 0 means "pick it yourself, as before" - so an old master still
   works with a new follower, and the field is recognisable from behaviour
   rather than from a version number.

   count = number of grid points; 0 means UNLIMITED and is thus today's
   behaviour - an old master that does not send the field reads as 0, and the
   extension is behaviour-neutral at that point.  The end is an ABSOLUTE
   instant (start + count * period), not a counter: a counter is a local
   quantity and drifts apart the moment a board skips a period - then the
   boards end at different times and the loss is invisible.  With an absolute
   end all of them end at the same grid point, and a board that lost a period
   emits N-1 pulses: the pulse count thereby becomes the check
   (PULSE_TRAIN_PLAN.md E1). */
#define TRIG_PL_SCHED_PER       32u       /* action(2) node(1) rsv(1) per(8) phase(8) start(8) count(4)   0 = all */

/* The SHORTEST still-valid SCHED_PER payload: action, rsv, period, phase.
   start_ns and count are optional and are checked INDIVIDUALLY against the
   actual length.  Previously the guard sat on the full length, and the
   comment at the read site promised tolerance for a 20-byte master that did
   not actually exist - a dead branch (PULSE_TRAIN_PLAN.md 2.4). */
#define TRIG_PL_SCHED_PER_MIN   20u

/* STATUS response payload, big endian:
     0  u16  node_id          PLCA node id - the identity 2 derives everything
                              else from, so it doubles as "which board is this"
     2  u8   tb_state         0 UNINIT, 1 ANCHORED, 2 LOCKED, 3 HOLDOVER
     3  u8   tb_usable        1 = the trigger would accept a command now
     4  u16  last_session     session id of the last command received
     6  u16  last_result      PTP_TRIG_RESULT for it (0 = OK)
     8  u32  accepted
    12  u32  refused
    16  u32  actions          bitmask of registered action ids (bit N = id N)
    20  u8[6] mac             eth0 MAC - the identity that is unique BEFORE one
                              is assigned, and therefore what ASSIGN addresses
    26  u32  ip               the follower's own eth0 address, REPORTED rather
                              than taken from the packet.  On the raw bootstrap
                              channel there is no source IP to read, and it is
                              wanted for its own sake: a listing where three
                              boards show the SAME address is what makes the
                              factory-state clash visible instead of merely
                              inferable                                        */
#define TRIG_PL_STATUS          30u

/* ASSIGN payload, big endian:
     0  u8[6] mac      target; every follower compares against its own and
                       ignores the message unless it matches
     6  u8    node_id  PLCA node id, applied to the PHY at once
     7  u8    node_cnt PLCA node count
     8  u32   ip       0 = leave unchanged.  Takes effect AT ONCE: the follower
                       re-addresses its running stack, no reset needed.  An
                       earlier version of this line claimed the opposite (the
                       stack is configured at boot) - disproved by the
                       bootstrap capture, see BOOTSTRAP.md 6
    12  u8    flags    bit 0 = persist (write the record to EEPROM)
    13  u8    reserved                                                          */
#define TRIG_PL_ASSIGN          14u
#define TRIG_ASSIGN_FLAG_PERSIST 0x01u

/* MODE payload, one byte per switch:
     0  u8  listen   0 = stop following, 1 = listen for Sync
     1  u8  source   0 = PTP pairs, 1 = 1PPS
     2  u8  pin      0/1  drive PD10 on every fire
     3  u8  hw       0/1  fire from TC1 (0 falls back to the software path)
     4  u8  trigmode 0 = STRICT, 1 = FREE   (since 2026-08-23)
     5  u16 div      LED grid divider: 0xFFFF unchanged, 0 off,
                     otherwise N (0x0000 = 65536)      (since 2026-08-24)
   Every byte may be TRIG_MODE_KEEP, so one field can be changed without having
   to know or restate the other three - a master that had to send a complete
   state would overwrite settings it never meant to touch.

   BYTE 4 WAS ADDED LATER, and compatibility hangs on TRIG_PL_MODE_MIN: the
   receiver checks against the MINIMUM (4) and reads byte 4 only if the
   message carries it.  So a new follower still accepts an old four-byte
   message, and an old follower silently ignores the fifth byte - the same
   pattern by which RESET grew from 0 to 4 bytes.

   WHAT BYTE 4 IS FOR: STRICT stops the trigger the moment the time base
   becomes unusable - correct in operation, exactly wrong for the drift
   demonstration, where the pin is SUPPOSED to keep running without a master.
   Without this path it would have to be typed on every follower separately.

   BYTES 5..6 WERE ADDED ON 2026-08-24, following the same pattern: the LED's
   GRID DIVIDER as a u16.  It belongs here rather than in a command of its
   own because it is exactly what MODE describes - an operating state,
   volatile, skippable per field.  `TRIG_DIV_KEEP` (0xFFFF) leaves it
   unchanged, 0 switches it off, 1..65536 sets it (65536 travels as 0x0000
   and is turned back into 65536 on the receiver - 0 is already spoken for as
   OFF, so 0x0000 cannot mean anything else).

   WHAT FOR: the divider makes the grid visible to the EYE - LED1 flips every
   Nth grid point, with no CPU involved at all (TC6 counts the grid events in
   hardware).  Without this path it would have to be typed on every follower
   separately. */
#define TRIG_PL_MODE            7u
#define TRIG_PL_MODE_MIN        4u        /* without bytes 4..6 - old senders  */
/* EACH FIELD GETS ITS OWN MINIMUM LENGTH, and that is the lesson from the
 * first growth step: there `pl_len >= TRIG_PL_MODE` stood as the condition
 * for byte 4.  As long as the payload was 5 bytes long that was correct -
 * once it grew to 7, it silently became "byte 4 only at length 7", and a
 * five-byte message would have lost its trigmode without anything, anywhere,
 * reporting it.  With one constant per field boundary the next extension
 * cannot repeat that. */
#define TRIG_PL_MODE_TRIGMODE   5u        /* byte 4 is present from here on    */
#define TRIG_PL_MODE_DIV        7u        /* bytes 5..6 are present from here  */
#define TRIG_MODE_KEEP          0xFFu
#define TRIG_TRIGMODE_STRICT    0u
#define TRIG_TRIGMODE_FREE      1u
/* Leave the divider unchanged.  Its own name and own width, because it is
   NOT a one-byte field - applying `TRIG_MODE_KEEP` to it would be 0x00FF,
   which is a valid divider of 255. */
#define TRIG_DIV_KEEP           0xFFFFu

/* TWO MAXIMA, and since the count field they are no longer the same.
 *
 * TRIG_PL_MAX is the maximum over ALL payloads and sizes the msg[] buffers.
 * Since SCHED_PER carries 32 bytes, STATUS is NO LONGER the longest; the
 * reference to TRIG_PL_STATUS that used to stand here would be a buffer
 * overflow on TRIGM_CmdTrigPer()'s stack, i.e. on the bridge, while SENDING.
 * Hence written as a maximum rather than a reference to a single field - the
 * next extension should not cost this round again.
 *
 * TRIG_PL_L2_MAX is the maximum of the payloads that travel over RAW
 * Ethernet: RESET, ASSIGN, the bare STATUS request and the STATUS reply.
 * Only it carries the invariant "a padded frame is exactly
 * TRIG_ETH_MIN_FRAME" (14 + 16 + 30 = 60).  SCHED_PER runs over UDP 30509
 * and does not touch it.  Without this separation the invariant would go
 * silently wrong through the count field, and the comment would keep
 * asserting it - exactly the kind of comment this project has paid for
 * dearly. */
#define TRIG_PL_MAX             ((TRIG_PL_STATUS > TRIG_PL_SCHED_PER) \
                                 ? TRIG_PL_STATUS : TRIG_PL_SCHED_PER)
#define TRIG_PL_L2_MAX          TRIG_PL_STATUS

#define TRIG_MSG_MAX            (TRIG_HDR_LEN + TRIG_PL_MAX)

/* From here on the buffer guards itself.  Every single payload must fit
   TRIG_PL_MAX, and the L2 invariant must come out exact - a _Static_assert
   costs nothing and catches exactly the mistake that until now was only an
   assertion in a comment above. */
_Static_assert(TRIG_PL_MAX >= TRIG_PL_SCHEDULE,  "TRIG_PL_MAX < SCHEDULE");
_Static_assert(TRIG_PL_MAX >= TRIG_PL_CANCEL,    "TRIG_PL_MAX < CANCEL");
_Static_assert(TRIG_PL_MAX >= TRIG_PL_SCHED_PER, "TRIG_PL_MAX < SCHED_PER");
_Static_assert(TRIG_PL_MAX >= TRIG_PL_STATUS,    "TRIG_PL_MAX < STATUS");
_Static_assert(TRIG_PL_MAX >= TRIG_PL_ASSIGN,    "TRIG_PL_MAX < ASSIGN");
_Static_assert(TRIG_PL_MAX >= TRIG_PL_MODE,      "TRIG_PL_MAX < MODE");
_Static_assert(TRIG_PL_MODE_MIN <= TRIG_PL_MODE,
               "MODE minimum above its own length");
_Static_assert(TRIG_PL_MODE_TRIGMODE <= TRIG_PL_MODE,
               "MODE: trigmode boundary above its own length");
_Static_assert(TRIG_PL_MODE_DIV == TRIG_PL_MODE,
               "MODE: div is the LAST field - the boundary must be the total length");
_Static_assert(TRIG_PL_MAX >= TRIG_PL_RESET,     "TRIG_PL_MAX < RESET");
_Static_assert(TRIG_PL_SCHED_PER_MIN <= TRIG_PL_SCHED_PER,
               "SCHED_PER_MIN greater than SCHED_PER");
_Static_assert(TRIG_PL_MAX >= TRIG_PL_BUTTON,    "TRIG_PL_MAX < BUTTON");
/* BUTTON travels over RAW Ethernet, so it falls under the L2 invariant.
   Today it fits (16 <= 30, STATUS stays the longest L2 payload) - and this
   line is why that is a CHECK and not an assertion: should an event grow
   past 30 bytes, TRIG_PL_L2_MAX and the invariant must be updated TOGETHER,
   and then the build breaks instead of the padding. */
_Static_assert(TRIG_PL_BUTTON <= TRIG_PL_L2_MAX,
               "BUTTON longer than TRIG_PL_L2_MAX - update the L2 invariant");
_Static_assert(TRIG_ETH_HDR_LEN + TRIG_HDR_LEN + TRIG_PL_L2_MAX
               == TRIG_ETH_MIN_FRAME, "L2-Invariante: 14+16+30 != 60");

/* Big-endian helpers.  Written out rather than pulled from the stack's byte
   order macros so that both copies of this file stay self-contained. */
static inline void trig_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void trig_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void trig_be64(uint8_t *p, uint64_t v)
{
    trig_be32(p, (uint32_t)(v >> 32));
    trig_be32(p + 4, (uint32_t)v);
}

static inline uint16_t trig_rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t trig_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline uint64_t trig_rd64(const uint8_t *p)
{
    return ((uint64_t)trig_rd32(p) << 32) | (uint64_t)trig_rd32(p + 4);
}

#endif /* TRIG_SOMEIP_H */
