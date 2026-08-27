/*******************************************************************************
  Endpoint server: the LAN866x protocol (service 0xFF10) on this follower

  File Name:
    someip.h

  Summary:
    Everything the endpoint protocol speaks on UDP - socket, header, WTLV,
    dispatch, handles, return codes, stubs.  The EFFECTS live outside.

  Description:
    Prefix `EPSRV_`, not `SOMEIP_`: that namespace belongs to the vendor
    library (`SOMEIP_Client_*`, `SOMEIP_Server_*`, `SOMEIP_CB_*`,
    `SOMEIP_Generator_*`).  The file name `someip.c` collides with nothing -
    their files are named `someip-client.c`/`someip-server.c`.

    THE BOUNDARY IS "WIRE", NOT "TOPIC" (SOMEIP_ANGLEICHUNG_PLAN.md 6.0.5).
    Sockets, header, WTLV, dispatch, method ids, handles, return codes and
    the stubs belong here.  The effects stay outside (`PTP_TRIG_*`, `PIN_*`),
    as do the time layer, the CLI and bootstrap - which runs on raw Ethernet
    0x88B6 and is exempt from the alignment.  Without this rule this file
    becomes the hub everything depends on.

    WHY THIS AT ALL: the follower is meant to be operable by foreign,
    unmodified tools (`lan866x-gpio`, `lan866x-pwm`, `lan866x-ledblink`).
    That is not an end in itself - it is the only way to make our time layer
    usable in a rig that already speaks this protocol.

  Where the wire format comes from - and why it is not guessed:
    From the vendor sources under C:\work\lan866x-tools, read on 2026-08-16:
      someip/libsomeip/src/someip-gen.c   Fill_Tag / Fill_UINT8..64 / Fill_Header
      someip/libsomeip/src/someip-pars.c  Read_*_ExpectTag
      lan866x_c/lan866x_client.c          tag ORDER per method
      src/rcp.c                           what the tools actually send
    Every number here has a line there.  "An id from a proto file is a claim
    until it has answered on the wire" (some_ip.md 6) - hence each method
    notes which of the four sources it comes from.
*******************************************************************************/

#ifndef SOMEIP_H
#define SOMEIP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------- */
/* Wire format                                                                 */
/* --------------------------------------------------------------------------- */

#define EPSRV_SERVICE_ID        0xFF10u   /* lan866x_client.c:103               */
#define EPSRV_INSTANCE_ID       0x0001u
#define EPSRV_UDP_PORT          6800u     /* methods; the reply's source port
                                             is free - the "fixed expectation
                                             49153" is documentation only and
                                             is not checked by the host       */
#define EPSRV_EVENTGROUP        0x2000u

/* someip-common.h: SOMEIP_VERSION = 1.  The interface version is the
   service's and is DISCARDED on mismatch - their own discipline (rcp.c:424),
   and it guards against evaluating an unknown layout. */
#define EPSRV_PROTO_VER         0x01u
#define EPSRV_IFACE_VER         0x01u

/* someip.h:542..546 */
#define EPSRV_MSGTYPE_REQUEST       0x00u
#define EPSRV_MSGTYPE_REQ_NORET     0x01u   /* fire-and-forget: do NOT reply    */
#define EPSRV_MSGTYPE_NOTIFICATION  0x02u   /* events                           */
#define EPSRV_MSGTYPE_RESPONSE      0x80u
#define EPSRV_MSGTYPE_ERROR         0x81u

/* Return codes.  THE WIRE FIELD IS ONE BYTE, so only 0x00..0x0F exist:
   RT_INTERNAL_ERROR 0x1000, RT_SEND_ERROR 0x1001, RT_PARAMETER_NOT_VALID
   0x1002 and RT_DEVICE_NOT_AVAILABLE 0x1003 are HOST-INTERNAL and can never
   appear on the wire.  A code of our own here would not be unknown, it would
   be a WRONG error report. */
#define EPSRV_RT_OK             0x00u
#define EPSRV_RT_UNKNOWN_METHOD 0x03u   /* the service does not know the method */
#define EPSRV_RT_NOT_READY      0x04u   /* exists, just not available right now */
#define EPSRV_RT_NOT_REACHABLE  0x05u   /* "peripheral not configured on node"  */

/* Header, 16 bytes, big endian (someip-common.h POS_*):
     0  u32  Message ID   = Service << 16 | Method
     4  u32  Length       = everything from byte 8 on, i.e. consumed - 8
     8  u32  Request ID   = ClientId << 16 | SessionId
    12  u8   Protocol Version
    13  u8   Interface Version
    14  u8   Message Type
    15  u8   Return Code                                                       */
#define EPSRV_HDR_LEN           16u

/* WTLV, from someip-gen.c:
     Tag = 2 bytes:  b0 = (wiretype << 4) | ((tagId >> 8) & 0x0F)
                    b1 = tagId & 0xFF
     Wiretypes: 0 = u8, 1 = u16, 2 = u32, 3 = u64,
                4 = complex with static length,
                6 = complex with a 2-byte length field  (that is a BLOB)
     Largest tag: 0x0FFF.

   A TAG MAY BE MISSING, AND THAT IS NOT A FAULT: `Read_*_ExpectTag` treats a
   non-matching tag as "optional field omitted" and consumes NOTHING
   (someip-pars.c:315).  A decoder that aborts on a missing tag is stricter
   than their own and rejects valid frames. */
#define EPSRV_WT_U8             0x0u
#define EPSRV_WT_U16            0x1u
#define EPSRV_WT_U32            0x2u
#define EPSRV_WT_U64            0x3u
#define EPSRV_WT_BLOB           0x6u

/* --------------------------------------------------------------------------- */
/* Methods - required set from some_ip.md 8.8 plus 0x1356 and event 0x8000     */
/* --------------------------------------------------------------------------- */

#define EPSRV_M_GET_STATUS          0x1002u
#define EPSRV_M_RELEASE_PINS        0x1105u   /* the way back; there is NO CloseGpio */
#define EPSRV_M_OPEN_GPIO           0x1300u
#define EPSRV_M_SET_GPIO            0x1330u
#define EPSRV_M_GET_GPIO            0x1332u
#define EPSRV_M_SET_GPIO_FAF        0x1336u   /* stays silent                    */
#define EPSRV_M_ENABLE_PULSE        0x1350u   /* our trigger, in their words     */
#define EPSRV_M_ENABLE_CAPTURE      0x1356u
#define EPSRV_M_DISABLE_EVENT       0x1360u
#define EPSRV_M_OPEN_PWM            0x1800u
#define EPSRV_M_CLOSE_PWM           0x1802u
#define EPSRV_M_WRITE_PWM           0x1804u
#define EPSRV_M_WRITE_PWM_FAF       0x1806u   /* stays silent                    */
#define EPSRV_EV_GPIO_EVENTS        0x8000u

/* Handles.  From `OpenGpio` on, the pin is no longer addressed directly but
   through the handle - it lives on the endpoint and survives the host
   program ending.  It is therefore NOT an index into a table that looks
   different after the next start, but a running number. */
#define EPSRV_HANDLES_MAX       8u

typedef struct
{
    bool     open;
    uint16_t id;              /* handle, 0 is invalid                           */
    uint8_t  pin_index;       /* row of the pin table                           */
    uint8_t  kind;            /* 0 = GPIO, 1 = PWM                              */
    uint8_t  direction;       /* OpenGpio: 0 in, 1 out-low, 2 out-high, 3 OD    */
    uint32_t interval_ns;     /* PWM/pulse                                      */
    uint32_t duty_q31;        /* PWM: 0 = 0 %, 2^31 = 100 %                     */
} EPSRV_HANDLE;

typedef struct
{
    bool     sock_open;
    uint32_t rx_frames;
    uint32_t rx_bad;          /* header/version/length rejected                 */
    uint32_t tx_replies;
    uint32_t tx_fail;
    uint32_t silent;          /* fire-and-forget: deliberately no reply         */
    uint32_t unknown_method;
    uint32_t stub_calls;
    uint16_t last_method;
    uint16_t last_session;
    uint8_t  last_retcode;
    uint8_t  handles_open;
} EPSRV_STATUS;

/* --------------------------------------------------------------------------- */
/* Service Discovery - SEND only                                               */
/* --------------------------------------------------------------------------- */

/* SENDING IS ENOUGH, and that is not a shortcut.  Their host waits for a
 * RECEIVED `OfferService` (`rcp_is_ready()`), and the core repeats offers
 * cyclically - so the follower does not need to receive `FindService`.  That
 * means it only needs multicast in the send direction, and the whole
 * objection against the promiscuous filter drops out of the critical path
 * (some_ip.md 8.10).
 *
 * 224.0.0.1 is "all hosts": sending there needs no group join, and the host
 * listens on 30490. */
#define EPSRV_SD_SERVICE        0xFFFFu   /* someip-common.h:112               */
#define EPSRV_SD_METHOD         0x8100u   /* SOMEIP_Event_ID                   */
#define EPSRV_SD_IFACE_VER      0x01u
#define EPSRV_SD_PORT           30490u
#define EPSRV_SD_TTL            0x0000FFu /* seconds; 0 would mean StopOffer   */
/* MAJOR 1, MINOR 1 - and the 1 in the minor version is not cosmetic.
 *
 * MEASURED 2026-08-16: `lan866x-discovery` saw our offers and rejected them
 * with `SOME/IP parsing returned 0x1`.  The reason is in
 * `GetClientIndexService` (someip-client.c): on a matching service id their
 * client checks the version, and the minor rule is `offered >= requested`.
 * `rcp.c:426` requires `minorVersion = 1u` - a reported 0 is therefore TOO
 * OLD, and the endpoint does not exist as far as the tool is concerned.
 *
 * That is the opposite direction from the rule in the plan ("report the
 * LOWEST version that covers what we can do"): it must not be lower than
 * what is required.  So the floor is not freely chosen, it is the number
 * their host demands - and reporting more than 1 would be promising a
 * feature set we do not have. */
#define EPSRV_SD_MAJOR          0x01u
#define EPSRV_SD_MINOR          0x00000001u
/* Their own value (someip-cfg.h:51), so a host with default settings sees
   several offers within its search window (5 s). */
#define EPSRV_SD_PERIOD_MS      1000u

void EPSRV_Initialize(void);
void EPSRV_Tasks(void);

/* Switch cyclic OfferService on and off.  Default ON, because an endpoint
   that is not offered does not exist for a foreign tool - and because
   switching it off is the more interesting counter-check (`discovery` finds
   nothing). */
void EPSRV_OfferSet(bool on);
bool EPSRV_OfferGet(void);

/* Show the CLI's handles (`tbase ep`) - diagnostics, not protocol. */
void EPSRV_StatusGet(EPSRV_STATUS *out);
void EPSRV_Print(void);

/* Per-frame console logging, SWITCHABLE and OFF by default: 40 lines a
   second choke 115200 baud and look like crashed firmware. */
void EPSRV_LogSet(bool on);

#ifdef __cplusplus
}
#endif
#endif /* SOMEIP_H */
