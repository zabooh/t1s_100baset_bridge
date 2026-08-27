/*******************************************************************************
  Trigger commands from the master - UDP receiver on the follower

  File Name:
    trig_cmd.c
*******************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "definitions.h"
#include "config/default/system/console/sys_console.h"
#include "system/command/sys_command.h"
#include "trig_cmd.h"
#include "trig_someip.h"
#include "ptp_trigger.h"
#include "ptp_timebase.h"
#include "env.h"
#include "lan865x_diag.h"
#include "ptp_follower.h"
#include "pps_capture.h"
#include "grid_div.h"
#include "config/default/driver/lan865x/drv_lan865x.h"

/* Retry throttle for opening the socket.  The stack is not ready when
   TRIGCMD_Initialize() runs, and hammering ServerOpen() from every main-loop
   pass would cost more than it gains. */
#define TRIGCMD_OPEN_RETRY_MS   1000u

/* eth0 - the 10BASE-T1S segment the master is on. */
#define TRIGCMD_IF              0u

static UDP_SOCKET s_sock = INVALID_UDP_SOCKET;
static uint64_t   s_next_open;
static uint64_t   s_ticks_per_ms;

static uint32_t s_rx;            /* datagrams taken off the socket        */
static uint32_t s_bad;           /* rejected before reaching the trigger  */
static uint32_t s_accepted;      /* handed to ptp_trigger and accepted    */
static uint32_t s_refused;       /* handed over and refused by the trigger*/
static uint16_t s_last_session;
static uint16_t s_last_method;
static uint16_t s_last_action;
static PTP_TRIG_RESULT s_last_result = PTP_TRIG_OK;
static uint32_t s_replies;       /* STATUS answers sent                   */
static uint32_t s_reply_fail;    /* asked, but the answer did not go out  */
static uint32_t s_assign_taken;  /* ASSIGN addressed to this board        */
static uint32_t s_assign_other;  /* ASSIGN for somebody else - not a fault*/
static uint32_t s_mode_taken;    /* MODE commands applied                 */
static uint32_t s_div_refused;   /* MODE div refused - LED1 belongs to someone else */
/* Commands this board legitimately received but was not addressed by.  Its
   OWN counter, not shared with s_assign_other: that name belongs to ASSIGN,
   and a counter that counts two things can prove neither.  And not s_bad
   either: this is not a fault, and an error counter that counts normal
   operation loses its meaning. */
static uint32_t s_cmd_notmine;
static bool     s_log = true;

/* One deferred answer, scheduled by trigcmd_reply_delay_ms(). A second
   request before the first has gone out replaces it: the master only cares
   about the newest question, and queueing would answer a stale one. */
static bool             s_reply_pending;
static uint64_t         s_reply_due;
static uint16_t         s_reply_session;
static IP_MULTI_ADDRESS s_reply_to;
/* Which way the answer goes.  A request that arrived over raw L2 is answered over
   raw L2, to the MAC it came from - never over IP, because the whole point of the
   L2 path is that no address is configured yet. */
static bool             s_reply_l2;
static uint8_t          s_reply_mac[6];

static uint32_t s_l2_rx;         /* bootstrap frames received             */
static uint32_t s_l2_tx;         /* bootstrap answers sent                */
static uint32_t s_l2_tx_fail;
static uint32_t s_l2_other;      /* another follower's answer, seen and ignored */
/* A NEIGHBOUR's event, seen and dropped - its own counter, not shared with
   s_l2_other and definitely not s_bad.  Without this branch a neighbour's
   BUTTON broadcast landed in the dispatch and there in `default: s_bad++`,
   making a healthy board look defective - exactly the mistake s_l2_other
   exists to prevent. */
static uint32_t s_l2_event;

/* The master's MAC, so an UNSOLICITED event does not have to broadcast.
 *
 * Latched in TRIGCMD_L2Rx(), AFTER the two early exits, and therefore only
 * for frames that genuinely come from the master: other boards' replies
 * (RESPONSE) and events (NOTIFICATION) are already filtered out by then.
 * This ordering is the whole point - an earlier version latched for EVERY
 * frame and overwrote the master's MAC with a neighbour's (measured
 * 2026-08-14, see the comment in TRIGCMD_L2Rx).
 *
 * As long as it is unknown, an event goes by broadcast - that has no
 * precondition and is thereby the more honest starting state. */
static uint8_t  s_master_mac[6];
static bool     s_master_known;

static uint32_t s_btn_tx;        /* BUTTON events sent                     */
static uint32_t s_btn_tx_fail;

/* --------------------------------------------------------------------------- */

/* Answers a STATUS request by unicast to whoever asked.
 *
 * The requester's address comes from the socket rather than from the payload:
 * a master that had to put its own address into the request would be stating
 * something it can get wrong, and a follower that believed it could be aimed
 * at a third party by a forged field. */
/* Delay before answering, derived from the MAC.
 *
 * Answering immediately is what a single follower should do and what a FLEET
 * must not.  Measured 2026-08-14: with both boards on the same PLCA node id -
 * the state a fresh fleet boots into - a trigwho got ZERO answers, while the
 * same two boards on distinct ids both replied.  T4 measured 68 % loss over a
 * stream of frames; for two frames sent at the same instant the loss is
 * effectively total, because they are not merely likely to collide, they are
 * scheduled into the same PLCA transmit opportunity.
 *
 * The MAC is the only thing that differs on a fresh fleet, so it is what the
 * stagger has to come from.  All six bytes are folded rather than just the last
 * one: consecutive serials differ in the low byte, and a fleet from one reel is
 * exactly the case that must not land in one slot.  32 slots of 10 ms fit
 * inside the master's 400 ms window.
 *
 * RETRYING DOES NOT HELP, and that is the flip side of the fixed slot: the
 * slot depends only on the MAC, so two boards that collide land together
 * again on EVERY trigwho.  An earlier comment here claimed the opposite -
 * it confused 'fixed' with 'random'.
 *
 * And the probability is not 1 in 32: the question is whether ANY TWO of N
 * boards share a slot, i.e. the birthday problem over 32 slots - around
 * 61 % for 8 boards.  The derivation is in BOOTSTRAP.md 9, and the fix is
 * not more slots (that scales quadratically badly), but a
 * collision-free slot derived from the node id, for the configured fleet -
 * RUNBOOK_PLCA_NODECOUNT.md 12. */
static uint32_t trigcmd_reply_delay_ms(const uint8_t *mac)
{
    uint32_t h = 2166136261u;               /* FNV-1a, folded over all six */
    int i;

    if (mac == NULL)
    {
        return 0u;
    }
    for (i = 0; i < 6; i++)
    {
        h ^= (uint32_t)mac[i];
        h *= 16777619u;
    }
    return (h % 32u) * 10u;
}

/* Sends an already-built message as a raw Ethernet frame to one MAC.
 *
 * THE BUFFER COMES FROM THE CALLER, and that has not been a matter of
 * style since the BUTTON event.  SendRawEthFrame() keeps the POINTER, so
 * the buffer must be static and must not be reused while the driver still
 * holds it.  Up to this point there was exactly one sender (the STATUS
 * reply, which replaces itself instead of queueing), so a single static
 * buffer inside this function was correct.  With a SECOND, independent
 * sender it becomes wrong: a button press and a STATUS reply in the same
 * main-loop pass would overwrite each other, and the result would be a
 * frame whose capture shows intent rather than the wire - the same trap as
 * in FALLSTRICKE_LAN8651.md, for the third time.  Every sender now brings
 * its own; 60 bytes per sender is the price. */
static bool trigcmd_l2_send(const uint8_t *msg, uint16_t mlen, const uint8_t *dst,
                            uint8_t *frame)
{
    TCPIP_NET_HANDLE net = TCPIP_STACK_IndexToNet(TRIGCMD_IF);
    const uint8_t *own = TCPIP_STACK_NetAddressMac(net);
    uint16_t flen = (uint16_t)(TRIG_ETH_HDR_LEN + mlen);

    /* `frame` is now a pointer, `sizeof` would be the pointer size - so the
       length has to be named.  Every buffer is TRIG_ETH_MIN_FRAME in size,
       and the header's L2 invariant guarantees every L2 payload fits. */
    if (flen > (uint16_t)TRIG_ETH_MIN_FRAME)
    {
        flen = (uint16_t)TRIG_ETH_MIN_FRAME;   /* cannot happen with today's sizes */
    }
    memset(frame, 0, TRIG_ETH_MIN_FRAME);
    memcpy(frame, dst, 6);
    if (own != NULL)
    {
        memcpy(frame + 6, own, 6);
    }
    frame[12] = (uint8_t)(TRIG_ETHERTYPE >> 8);
    frame[13] = (uint8_t)TRIG_ETHERTYPE;
    memcpy(frame + TRIG_ETH_HDR_LEN, msg, mlen);

    /* Padded to the Ethernet minimum on purpose rather than leaving it to the
       driver, so every capture shows the same length. */
    if (!DRV_LAN865X_SendRawEthFrame(TRIGCMD_IF, frame,
                                     (uint16_t)TRIG_ETH_MIN_FRAME, 0x00u, NULL, NULL))
    {
        s_l2_tx_fail++;
        return false;
    }
    s_l2_tx++;
    return true;
}

static void trigcmd_reply_status(uint16_t session, IP_MULTI_ADDRESS to)
{
    uint8_t msg[TRIG_HDR_LEN + TRIG_PL_STATUS];
    uint8_t *pl = msg + TRIG_HDR_LEN;
    UDP_SOCKET sock;
    PTP_TB_STATUS tb;

    PTP_TB_StatusGet(&tb);

    trig_be16(msg + 0, TRIG_SERVICE_ID);
    trig_be16(msg + 2, TRIG_METHOD_STATUS);
    trig_be32(msg + 4, (uint32_t)(TRIG_LEN_FIXED_PART + TRIG_PL_STATUS));
    trig_be16(msg + 8, 0u);                 /* client id: echoed, unused    */
    trig_be16(msg + 10, session);           /* same session: this is ITS answer */
    msg[12] = TRIG_PROTO_VER;
    msg[13] = TRIG_IFACE_VER;
    msg[14] = TRIG_MSGTYPE_RESPONSE;
    msg[15] = TRIG_RETCODE_OK;

    trig_be16(pl + 0, (uint16_t)env_plca_id());
    pl[2] = (uint8_t)tb.state;
    pl[3] = PTP_TB_IsUsable() ? 1u : 0u;
    trig_be16(pl + 4, s_last_session);
    trig_be16(pl + 6, (uint16_t)s_last_result);
    trig_be32(pl + 8, s_accepted);
    trig_be32(pl + 12, s_refused);
    trig_be32(pl + 16, PTP_TRIG_ActionMask());
    {
        const uint8_t *mac = env_mac(0);
        if (mac != NULL)
        {
            memcpy(pl + 20, mac, 6);
        }
        else
        {
            memset(pl + 20, 0, 6);
        }
    }
    /* The board reports its OWN address rather than letting the master read it
       off the packet - on the raw channel there is nothing to read it off.  Asked
       of the stack and not of the env record, so that a saved-but-not-yet-booted
       address shows as what is actually in effect. */
    {
        TCPIP_NET_HANDLE net = TCPIP_STACK_IndexToNet(TRIGCMD_IF);
        IPV4_ADDR ip;
        ip.Val = (net != NULL) ? TCPIP_STACK_NetAddress(net) : 0u;
        memcpy(pl + 26, ip.v, 4);
    }

    /* Raw L2 when the question came that way.  No socket, no address, no ARP -
       and in the factory state that is the ONLY way an answer gets out at all. */
    if (s_reply_l2)
    {
        /* Eigener statischer Puffer, siehe trigcmd_l2_send(). */
        static uint8_t frame[TRIG_ETH_MIN_FRAME];

        if (trigcmd_l2_send(msg, (uint16_t)sizeof msg, s_reply_mac, frame))
        {
            s_replies++;
        }
        else
        {
            s_reply_fail++;
        }
        return;
    }

    sock = TCPIP_UDP_ClientOpen(IP_ADDRESS_TYPE_IPV4, TRIG_REPLY_PORT, &to);
    if (sock == INVALID_UDP_SOCKET)
    {
        s_reply_fail++;
        return;
    }
    if (TCPIP_UDP_PutIsReady(sock) >= sizeof msg
        && TCPIP_UDP_ArrayPut(sock, msg, (uint16_t)sizeof msg) == (uint16_t)sizeof msg)
    {
        (void)TCPIP_UDP_Flush(sock);
        s_replies++;
    }
    else
    {
        s_reply_fail++;
    }
    TCPIP_UDP_Close(sock);
}

/* Send a BUTTON event out - the only message this board sends UNSOLICITED.
 *
 * It lives here and not in button.c, because the wire format and the raw
 * sender already live here, and because the node number and the master MAC
 * live here.  button.c recognises and dates, this module sends.
 *
 * Over RAW Ethernet, like the STATUS reply: no socket, no address, no ARP -
 * a button press should be able to report even when the IP side is not up
 * yet.  The destination is the master's MAC once it is known, broadcast
 * otherwise. */
bool TRIGCMD_ButtonEvent(uint8_t btn, bool usable, uint32_t seq, uint64_t ts_ns)
{
    static const uint8_t bcast[6] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu };
    /* Its own static buffer - the reason is at trigcmd_l2_send(). */
    static uint8_t frame[TRIG_ETH_MIN_FRAME];
    uint8_t msg[TRIG_HDR_LEN + TRIG_PL_BUTTON];
    uint8_t *pl = msg + TRIG_HDR_LEN;

    trig_be16(msg + 0, TRIG_SERVICE_ID);
    trig_be16(msg + 2, TRIG_EVENT_BUTTON);
    trig_be32(msg + 4, (uint32_t)(TRIG_LEN_FIXED_PART + TRIG_PL_BUTTON));
    trig_be16(msg + 8, 0u);                 /* client id: unused                 */
    /* SESSION 0: there is no question this is the answer to, and 0
       explicitly means "no session tracking" per the standard.  The press
       counter in the payload is the identifier that counts here - it
       survives a wraparound and makes a lost frame visible. */
    trig_be16(msg + 10, 0u);
    msg[12] = TRIG_PROTO_VER;
    msg[13] = TRIG_IFACE_VER;
    msg[14] = TRIG_MSGTYPE_NOTIFY;
    msg[15] = TRIG_RETCODE_OK;

    trig_be16(pl + 0, (uint16_t)env_plca_id());
    pl[2] = btn;
    pl[3] = usable ? 1u : 0u;
    trig_be32(pl + 4, seq);
    trig_be64(pl + 8, ts_ns);

    if (trigcmd_l2_send(msg, (uint16_t)sizeof msg,
                        s_master_known ? s_master_mac : bcast, frame))
    {
        s_btn_tx++;
        return true;
    }
    s_btn_tx_fail++;
    return false;
}

static void trigcmd_schedule_reply(uint16_t session, IP_MULTI_ADDRESS to,
                                  bool via_l2, const uint8_t *dst_mac)
{
    s_reply_session = session;
    s_reply_to      = to;
    s_reply_l2      = via_l2;
    /* Latched HERE, with the session it belongs to.  Keeping it in a variable that
       every received frame updated is what made a board answer its neighbour. */
    if (via_l2 && (dst_mac != NULL))
    {
        memcpy(s_reply_mac, dst_mac, 6);
    }
    /* THE DEADLINE IS NOT PUSHED BACK if an answer is already pending.
     *
     * The CONTENT is allowed to be the newest question - session and
     * address are overwritten above, and that stays correct.  The DEADLINE
     * must not be.  The wait staggers the fleet by up to 310 ms; if the
     * master asks faster than that, every new request pushes the deadline
     * back again, and a board whose slot lies beyond the polling cadence
     * NEVER answers.
     *
     * Measured 2026-08-19 with the new `trigper`'s quarter-second cadence:
     * node 1 (slot 130 ms) answered 98 of 98 times, node 2 (slot 260 ms)
     * exactly once - and thereby looked like a hung board, even though it
     * had received and correctly processed every request.  The stagger was
     * built for the one-shot `trigwho`, where 310 ms is harmless. */
    if (!s_reply_pending)
    {
        s_reply_due = SYS_TIME_Counter64Get()
                    + (uint64_t)trigcmd_reply_delay_ms(env_mac(0)) * s_ticks_per_ms;
    }
    s_reply_pending = true;
}

static void trigcmd_dispatch(const uint8_t *msg, uint16_t len,
                             IP_MULTI_ADDRESS from, bool via_l2,
                             const uint8_t *src_mac)
{
    uint16_t method;
    uint16_t session;
    uint32_t req_id;
    uint32_t length;
    uint16_t action;
    uint16_t pl_len;
    const uint8_t *pl;
    PTP_TRIG_RESULT r;

    if (len < TRIG_HDR_LEN)
    {
        s_bad++;
        return;
    }

    if (trig_rd16(msg) != TRIG_SERVICE_ID
        || msg[12] != TRIG_PROTO_VER
        || msg[13] != TRIG_IFACE_VER
        || (msg[14] != TRIG_MSGTYPE_REQ_NORET
            && msg[14] != TRIG_MSGTYPE_REQUEST))
    {
        s_bad++;
        return;
    }

    method  = trig_rd16(msg + 2);
    length  = trig_rd32(msg + 4);
    session = trig_rd16(msg + 10);
    /* THE DEDUP KEY IS THE WHOLE REQUEST ID, not the session alone.  The
       bridge has two senders on this service with INDEPENDENT session
       counters - client 0x0001 in trig_master.c (trigto, trigwho,
       trigassign, trigmode) and 0xBEEF in someip.c (trig, trigat, trigper).
       Without the client id in the key the two collide, and a valid
       SCHEDULE gets refused as a repeat; visible only in `refused`.
       Reproduced on the device on 2026-08-21: `trigto ... 1` (counter A,
       session 3) followed by `trigat 1 <t>` (counter B, also session 3)
       gave `last: action 1 session 3 -> duplicate action+sequence` and
       refused +1, even though they were two different commands.
       Derivation: SOMEIP_ANGLEICHUNG_PLAN.md 12.3, RUNBOOK_SOMEIP_HEADER_FIX.md. */
    req_id  = ((uint32_t)trig_rd16(msg + 8) << 16) | (uint32_t)session;
    pl      = msg + TRIG_HDR_LEN;
    pl_len  = (uint16_t)(len - TRIG_HDR_LEN);

    /* The Length field counts from the Request ID onwards, not the whole
       message.  Getting this backwards is the documented trap of this format,
       so it is checked rather than trusted. */
    if (length < TRIG_LEN_FIXED_PART
        || (uint32_t)(length - TRIG_LEN_FIXED_PART) > (uint32_t)pl_len)
    {
        s_bad++;
        return;
    }
    pl_len = (uint16_t)(length - TRIG_LEN_FIXED_PART);

    /* STATUS carries no payload and is answered, not executed - handled before
       the action-id parse below, which the other methods all need. */
    if (method == TRIG_METHOD_STATUS)
    {
        s_last_method = method;
        trigcmd_schedule_reply(session, from, via_l2, src_mac);
        return;
    }

    /* ASSIGN is addressed by MAC, so a broadcast reaches the whole fleet and
       exactly one board acts on it.  Silently ignoring a mismatch is right here
       - every other follower is a legitimate recipient of the frame and simply
       not the addressee - but the counter distinguishes "not for me" from
       "malformed", because otherwise a fleet-wide assignment that reached
       nobody looks identical to one that reached everybody. */
    if (method == TRIG_METHOD_ASSIGN)
    {
        const uint8_t *own = env_mac(0);

        s_last_method = method;
        if (pl_len < TRIG_PL_ASSIGN || own == NULL)
        {
            s_bad++;
            return;
        }
        if (memcmp(pl, own, 6) != 0)
        {
            s_assign_other++;
            return;
        }

        {
            uint8_t  id      = pl[6];
            uint8_t  cnt     = pl[7];
            uint32_t ip_wire = trig_rd32(pl + 8);
            bool     persist = (pl[12] & TRIG_ASSIGN_FLAG_PERSIST) != 0u;
            IPV4_ADDR a;
            bool ip_live = false;

            /* The wire carries the four octets in order; IPV4_ADDR.Val holds them
               in the opposite order on this target, because v[0] is the FIRST
               octet and the core is little endian.  Converted through v[] rather
               than by shifting Val, so the wire format stays correct regardless of
               the target's endianness - and converted HERE, because this file owns
               the wire format and env.c does not. */
            a.v[0] = (uint8_t)(ip_wire >> 24);
            a.v[1] = (uint8_t)(ip_wire >> 16);
            a.v[2] = (uint8_t)(ip_wire >> 8);
            a.v[3] = (uint8_t)ip_wire;

            (void)env_set_identity(id, cnt, (ip_wire != 0u) ? a.Val : 0u, persist);
            /* The PHY takes the node id NOW.  Waiting for a reset would leave
               the bus in the state this whole mechanism exists to end. */
            LAN865X_DIAG_ApplyPlca(id, cnt);
            /* And so does the address, for the same reason.  It used to be left
               for a reset on the belief that the stack can only be addressed at
               boot; it cannot - this is the identical call env_apply() makes at
               start-up.  The consequence of the old belief was a fleet sharing one
               address with the bridge. */
            if (ip_wire != 0u)
            {
                ip_live = env_apply_ip((int)TRIGCMD_IF);
            }
            s_assign_taken++;
            SYS_CONSOLE_PRINT("[TRIGCMD] assigned: node %u of %u%s%s\r\n",
                              (unsigned)id, (unsigned)cnt,
                              (ip_wire == 0u) ? ""
                                  : (ip_live ? "  + IP live now"
                                             : "  + IP stored, NOT applied"),
                              persist ? "  [saved]" : "  [volatile]");
        }
        trigcmd_schedule_reply(session, from, via_l2, src_mac); /* same path back */
        return;
    }

    /* MODE is addressed to everyone at once - unlike ASSIGN, which picks one
       board by MAC.  That is intended: "all of you get ready" is the normal
       case, and a single board is reached with the unicast form. */
    if (method == TRIG_METHOD_RESET)
    {
        /* WITHOUT a payload the broadcast applies to everyone - that is how
           `trigper ... r` has always sent it, and this branch must keep
           holding for that unchanged.  WITH a payload a number is meant;
           if it does not match, this board is a legitimate recipient of
           the frame and simply not the addressee - that is counted, not
           reported as an error (the same rule as ASSIGN). */
        if (pl_len >= TRIG_PL_RESET && pl[0] != 0u
            && pl[0] != (uint8_t)env_plca_id())
        {
            s_assign_other++;
            return;
        }
        /* No state to tear down, no reply to send: the intent is exactly
           the zero state.  The message still goes out because it fits in
           the UART buffer; if it did not, that would be the right price -
           the proof of effect is the fresh STATUS reply after boot-up. */
        SYS_CONSOLE_PRINT("[TRIG] RESET from the master - restarting\r\n");
        NVIC_SystemReset();
        return;                     /* never reached */
    }

    if (method == TRIG_METHOD_MODE)
    {
        s_last_method = method;
        /* Check against the MINIMUM, not the full length: byte 4 (trigmode)
           was added later, and an old four-byte message is still valid.
           Requiring TRIG_PL_MODE here would refuse exactly the senders
           written before the extension. */
        if (pl_len < TRIG_PL_MODE_MIN)
        {
            s_bad++;
            return;
        }
        if (pl[0] != TRIG_MODE_KEEP)
        {
            if (pl[0] != 0u) { (void)PTP_FOL_Start(); } else { PTP_FOL_Stop(); }
        }
        if (pl[1] != TRIG_MODE_KEEP)
        {
            bool want_pps = (pl[1] != 0u);

            /* ONLY on an actual change.  Switching the source CLEARS the model,
               and trigmode is meant to be safe to issue on a healthy fleet - a
               command that silently throws away two minutes of convergence every
               time it is typed would be worse than the problem it solves.  This
               cost one test round: after "trigmode off" the fleet would not come
               back, because each "trigmode" wiped what it had just rebuilt.

               Through PPS_FeedSet rather than PTP_TB_SourceSet, because it also
               turns the 1PPS on in the PHY - which does not survive a reset and
               was a silent dead source until it did. */
            if ((PTP_TB_SourceGet() == PTP_TB_SRC_PPS) != want_pps)
            {
                PPS_FeedSet(want_pps);
            }
        }
        if (pl[2] != TRIG_MODE_KEEP)
        {
            (void)PTP_TRIG_ArmPin(pl[2] != 0u);
        }
        if (pl[3] != TRIG_MODE_KEEP)
        {
            PTP_TRIG_HwSet(pl[3] != 0u);
        }
        /* Every field added later is checked against ITS OWN minimum
           length, not against the total length - or a shorter, valid
           message silently loses its last field on the next growth (see
           the header). */
        if ((pl_len >= TRIG_PL_MODE_TRIGMODE) && (pl[4] != TRIG_MODE_KEEP))
        {
            PTP_TRIG_ModeSet((pl[4] != 0u) ? PTP_TRIG_MODE_FREE
                                           : PTP_TRIG_MODE_STRICT);
        }
        if (pl_len >= TRIG_PL_MODE_DIV)
        {
            uint16_t d = trig_rd16(pl + 5);

            if (d != TRIG_DIV_KEEP)
            {
                if (d == 0u)
                {
                    GDIV_Off();
                }
                else
                {
                    /* 0x0000 cannot mean OFF, that is already spoken for -
                       it is the only way to carry 65536 in 16 bit. */
                    if (!GDIV_Set(d))
                    {
                        /* `(void)GDIV_Set(d)` USED TO STAND HERE, AND THAT WAS A DEFECT.
                         *
                         * On 2026-08-24 the operator issued `trigmode div 2500`
                         * while action 4 was running.  Action 4 holds LED1, the
                         * divider's claim fails, `GDIV_Set()` deliberately
                         * changes NOTHING - and because the return value was
                         * discarded, nobody said anything: the bridge reported
                         * `sent`, the follower stayed silent, and the command
                         * looked successful.
                         *
                         * The line is therefore NOT gated by `s_log`: a refused
                         * instruction is an exceptional case, not routine
                         * logging. */
                        s_div_refused++;
                        SYS_CONSOLE_PRINT("[TRIGCMD] divider %u REFUSED -"
                                          " LED1 is taken (action 4?);"
                                          " nothing changed\r\n", (unsigned)d);
                    }
                }
            }
        }
        s_mode_taken++;
        if (s_log)
        {
            SYS_CONSOLE_PRINT("[TRIGCMD] mode: listen %u  source %u  pin %u  hw %u"
                              "  trigmode %u  (0xFF = unchanged)\r\n",
                              (unsigned)pl[0], (unsigned)pl[1],
                              (unsigned)pl[2], (unsigned)pl[3],
                              (pl_len >= TRIG_PL_MODE_TRIGMODE)
                                  ? (unsigned)pl[4] : (unsigned)TRIG_MODE_KEEP);
        }
        trigcmd_schedule_reply(session, from, via_l2, src_mac);
        return;
    }

    if (pl_len < TRIG_PL_CANCEL)
    {
        s_bad++;
        return;
    }
    action = trig_rd16(pl);

    /* IS THIS COMMAND FOR ME AT ALL?  The byte at offset 2 is the node
     * number, 0 means all - word-for-word the same rule as the existing
     * RESET handling.
     *
     * The check sits BEFORE booking s_last_method/session/action, and that
     * is not a style choice: a command that does not concern this board
     * must not change its state report.  Otherwise the STATUS reply would
     * claim success for a command that never touched this board, and a
     * state report that mixes politeness with facts is worthless as a
     * verification tool (TRIG_ADRESSIERUNG_PLAN.md E4).
     *
     * pl_len >= 4 is checked even though CANCEL's 4-byte payload is the
     * shortest one and the field is therefore always present - the check
     * costs nothing, and "always present" is exactly the assumption that
     * breaks later. */
    if (pl_len >= 4u && pl[2] != 0u && pl[2] != (uint8_t)env_plca_id())
    {
        s_cmd_notmine++;
        return;                 /* a legitimate receiver, just not the addressee */
    }

    s_last_method  = method;
    s_last_session = session;
    s_last_action  = action;

    switch (method)
    {
    case TRIG_METHOD_SCHEDULE:
        if (pl_len < TRIG_PL_SCHEDULE)
        {
            s_bad++;
            return;
        }
        r = PTP_TRIG_ScheduleAt(action, req_id, trig_rd64(pl + 4));
        break;

    case TRIG_METHOD_SCHED_PER:
        /* THE BOUND IS THE SHORTEST STILL-VALID LENGTH, not the full one.
           This used to read TRIG_PL_SCHED_PER, which made the ternary's
           condition below always true and the branch for the short master
           never ran: the comment promised a backward compatibility that
           did not exist (PULSE_TRAIN_PLAN.md 2.4).  Now every OPTIONAL
           field is checked individually against the actual length, and the
           short case is reachable - which it must be, because count is
           added later by the same pattern. */
        if (pl_len < TRIG_PL_SCHED_PER_MIN)
        {
            s_bad++;
            return;
        }
        {
            /* start_ns = 0 means "follower picks the first point itself",
               count = 0 means "unlimited" - both are exactly the behaviour
               of a master that does not send the field.  The extension is
               therefore behaviour-neutral for every old sender. */
            uint64_t start = (pl_len >= 28u) ? trig_rd64(pl + 20) : 0u;
            uint32_t count = (pl_len >= TRIG_PL_SCHED_PER)
                                 ? trig_rd32(pl + 28) : 0u;

            r = PTP_TRIG_SchedulePeriodic(action, req_id,
                                         trig_rd64(pl + 4), trig_rd64(pl + 12),
                                         start, count);
        }
        break;

    case TRIG_METHOD_CANCEL:
        PTP_TRIG_Cancel();
        r = PTP_TRIG_OK;
        break;

    default:
        s_bad++;
        return;
    }

    s_last_result = r;
    if (r == PTP_TRIG_OK)
    {
        s_accepted++;
    }
    else
    {
        s_refused++;
    }

    /* One line per command is the right volume here: commands are rare (a
       periodic schedule is ONE command, not one per period).  The console flood
       that killed a measurement day came from a line per FRAME, which this is
       not - but the switch exists because a burst of unicast diagnostics could
       still get loud. */
    if (s_log)
    {
        SYS_CONSOLE_PRINT("[TRIGCMD] method 0x%04X action %u session %u -> %s\r\n",
                          (unsigned)method, (unsigned)action, (unsigned)session,
                          PTP_TRIG_ResultName(r));
    }
}

/* --------------------------------------------------------------------------- */

void TRIGCMD_Initialize(void)
{
    s_ticks_per_ms = (uint64_t)SYS_TIME_FrequencyGet() / 1000u;
    if (s_ticks_per_ms == 0u)
    {
        s_ticks_per_ms = 1u;
    }
    s_sock = INVALID_UDP_SOCKET;
    s_next_open = 0u;
}

void TRIGCMD_Tasks(void)
{
    uint8_t  msg[TRIG_MSG_MAX];
    uint16_t avail;

    if (s_sock == INVALID_UDP_SOCKET)
    {
        uint64_t now = SYS_TIME_Counter64Get();
        if (now < s_next_open)
        {
            return;
        }
        s_next_open = now + (uint64_t)TRIGCMD_OPEN_RETRY_MS * s_ticks_per_ms;

        /* A server socket with no remote address bound: it accepts unicast to
           this follower AND the broadcast that addresses the whole group.  One
           socket for both cases is deliberate - the group command must not
           travel a different code path than the individual one, or only one of
           them would ever be tested. */
        s_sock = TCPIP_UDP_ServerOpen(IP_ADDRESS_TYPE_IPV4, TRIG_UDP_PORT, NULL);
        if (s_sock == INVALID_UDP_SOCKET)
        {
            return;                     /* stack not up yet - try again later */
        }
        SYS_CONSOLE_PRINT("[TRIGCMD] listening on UDP %u\r\n",
                          (unsigned)TRIG_UDP_PORT);
    }

    /* A scheduled answer goes out here, in its own slot.  Sending it from the
       dispatch above would put every follower's reply on the bus in the same
       instant, which is exactly what a fleet on duplicate node ids cannot
       survive. */
    if (s_reply_pending && SYS_TIME_Counter64Get() >= s_reply_due)
    {
        s_reply_pending = false;
        trigcmd_reply_status(s_reply_session, s_reply_to);
    }

    while ((avail = TCPIP_UDP_GetIsReady(s_sock)) > 0u)
    {
        uint16_t take = (avail > (uint16_t)sizeof msg)
                      ? (uint16_t)sizeof msg : avail;
        uint16_t got  = TCPIP_UDP_ArrayGet(s_sock, msg, take);
        UDP_SOCKET_INFO info;
        IP_MULTI_ADDRESS from;

        /* Read the sender BEFORE discarding: sourceIPaddress documents "the
           last packet", and after Discard that is no longer this one.  A STATUS
           reply aimed at a stale address would go to whoever asked previously -
           a fault that only shows up with more than one master. */
        from.v4Add.Val = 0u;
        if (TCPIP_UDP_SocketInfoGet(s_sock, &info))
        {
            from.v4Add = info.sourceIPaddress.v4Add;
        }

        /* Whatever is left of this datagram is not part of the next one; drop
           it so a long or malformed message cannot shift the parse of every
           command after it. */
        (void)TCPIP_UDP_Discard(s_sock);

        s_rx++;
        trigcmd_dispatch(msg, got, from, false, NULL);
    }
}

/* --------------------------------------------------------------------------- */

void TRIGCMD_L2Rx(const uint8_t *frame, uint16_t len)
{
    const uint8_t *msg = frame + TRIG_ETH_HDR_LEN;
    IP_MULTI_ADDRESS none;

    if (frame == NULL || len < (uint16_t)(TRIG_ETH_HDR_LEN + TRIG_HDR_LEN))
    {
        s_bad++;
        return;
    }
    s_l2_rx++;

    /* ANOTHER FOLLOWER'S ANSWER, dropped here and not counted as malformed.
     *
     * On a shared bus every board sees every frame, so each one receives not only
     * the master's request but its neighbour's reply as well.  Passing those into
     * dispatch had two costs, and the second one was a real defect:
     *   - they were counted as malformed, which made a healthy fleet look broken;
     *   - the reply address used to be latched here, for EVERY frame, so a board
     *     whose stagger slot came later overwrote the master's MAC with its
     *     neighbour's and answered to the neighbour.
     *
     * Measured 2026-08-14: node 2 addressed its STATUS reply to node 1's MAC.  It
     * still worked - the bridge's driver is promiscuous for the MAC bridge, so the
     * answer arrived and the console listed the board - but the port mirror clones
     * only frames addressed to the bridge, so that board was absent from every
     * capture, 0 of 8 replies over five rounds, with no counter reporting a
     * problem.  The reply target is now latched where the reply is SCHEDULED. */
    if (msg[14] == TRIG_MSGTYPE_RESPONSE)
    {
        s_l2_other++;
        return;
    }

    /* A NEIGHBOUR'S EVENT, for the same reason and with the same consequence.
     *
     * A BUTTON event goes out by broadcast while the master's MAC is still
     * unknown - every board sees it then.  Without this early-out it would
     * fall through to the dispatch and into `default: s_bad++`, i.e. the
     * MALFORMED counter: a key press on board 1 would have made board 2 look
     * broken.  Its own counter, so the opposite can be PROVEN too. */
    if (msg[14] == TRIG_MSGTYPE_NOTIFY)
    {
        s_l2_event++;
        return;
    }

    /* HERE, and not further up: what reaches this point is a command, and
       only the master sends commands.  Replies and events from other boards
       are already sorted out by the two branches above.  This exact
       ordering was the 2026-08-14 defect - back then the latch ran for
       EVERY frame, and the result was a reply to the neighbour. */
    memcpy(s_master_mac, frame + 6, 6);
    s_master_known = true;

    none.v4Add.Val = 0u;
    trigcmd_dispatch(msg, (uint16_t)(len - TRIG_ETH_HDR_LEN), none, true,
                     frame + 6);
}

bool TRIGCMD_CliTry(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "cmd") != 0)
    {
        return false;
    }

    if (argc >= 4 && !strcmp(argv[2], "log"))
    {
        s_log = (strcmp(argv[3], "on") == 0);
        SYS_CONSOLE_PRINT("[TRIGCMD] log: %s\r\n", s_log ? "on" : "off");
        return true;
    }

    SYS_CONSOLE_PRINT("[TRIGCMD] socket: %s   port %u   log %s\r\n",
                      (s_sock == INVALID_UDP_SOCKET) ? "NOT OPEN" : "open",
                      (unsigned)TRIG_UDP_PORT, s_log ? "on" : "off");
    SYS_CONSOLE_PRINT("[TRIGCMD] rx: %lu   accepted: %lu   refused: %lu"
                      "   malformed: %lu\r\n",
                      (unsigned long)s_rx, (unsigned long)s_accepted,
                      (unsigned long)s_refused, (unsigned long)s_bad);
    SYS_CONSOLE_PRINT("[TRIGCMD] assign: taken %lu   for others %lu\r\n",
                      (unsigned long)s_assign_taken,
                      (unsigned long)s_assign_other);
    /* Without this number a refused divider command looks like a command
       with no effect, and the bridge cannot possibly know - it does not
       know the follower's pin-ownership situation. */
    SYS_CONSOLE_PRINT("[TRIGCMD] grid divider refused: %lu   (LED1 held elsewhere)\r\n",
                      (unsigned long)s_div_refused);
    /* Without this number a board that is deliberately skipped looks like a
       broken board - and one goes looking at the delivery path or the time
       layer instead. */
    SYS_CONSOLE_PRINT("[TRIGCMD] commands: taken %lu   for other nodes %lu\r\n",
                      (unsigned long)s_accepted,
                      (unsigned long)s_cmd_notmine);
    /* The raw channel separately: an answer counted under "status replies"
       may have gone out over EITHER transport, and over IP in the factory
       state it goes nowhere at all.

       "frames out" instead of "answers": since the BUTTON event, s_l2_tx no
       longer counts only replies, and a label that lumps two things
       together is useless as evidence for either. */
    SYS_CONSOLE_PRINT("[TRIGCMD] raw L2: rx %lu   frames out %lu"
                      "   failed %lu   answers of others %lu"
                      "   events of others %lu\r\n",
                      (unsigned long)s_l2_rx, (unsigned long)s_l2_tx,
                      (unsigned long)s_l2_tx_fail, (unsigned long)s_l2_other,
                      (unsigned long)s_l2_event);
    /* The master MAC decides whether an event goes out unicast or as a
       broadcast - "unknown" therefore does not mean broken, it means "no
       command seen from the master yet". */
    SYS_CONSOLE_PRINT("[TRIGCMD] button events sent: %lu   failed: %lu"
                      "   master mac: %s\r\n",
                      (unsigned long)s_btn_tx, (unsigned long)s_btn_tx_fail,
                      s_master_known ? "known" : "unknown (broadcast)");
    SYS_CONSOLE_PRINT("[TRIGCMD] status replies: %lu   failed to send: %lu"
                      "   node %u   actions 0x%08lX\r\n",
                      (unsigned long)s_replies, (unsigned long)s_reply_fail,
                      (unsigned)env_plca_id(),
                      (unsigned long)PTP_TRIG_ActionMask());
    SYS_CONSOLE_PRINT("[TRIGCMD] last: method 0x%04X   action %u   session %u"
                      "   -> %s\r\n",
                      (unsigned)s_last_method, (unsigned)s_last_action,
                      (unsigned)s_last_session,
                      PTP_TRIG_ResultName(s_last_result));
    if (s_sock == INVALID_UDP_SOCKET)
    {
        SYS_CONSOLE_PRINT("[TRIGCMD] no socket yet - is the stack up?"
                          "  'showenv' shows the configured address\r\n");
    }
    return true;
}
