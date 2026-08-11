/*******************************************************************************
  PTP grandmaster on the 10BASE-T1S segment

  File Name:
    ptp_gm.c

  Summary:
    Implementation of the one-way PTP grandmaster described in ptp_gm.h.

  Description:
    The whole module is one state machine driven from PTP_GM_Tasks(). Nothing
    blocks, and nothing loops over sends: the raw transmit queue holds four
    entries and is only drained when the main loop services the driver, so a
    burst of more than five frames fails by construction (measured 2026-08-11,
    see CLAUDE.md section 6). One Sync plus one Follow_Up per cycle, each after
    returning to the main loop, stays far away from that limit.

    Register work is deliberately small. Frame timestamping needs FTSE and FTSS
    in OA_CONFIG0 - both, never FTSE alone, because the receive path skips a
    fixed eight bytes and a 32-bit stamp would eat payload silently
    (LAN8651_TIME_SYNC.md section 10.3). The transmit pattern matcher needs
    nothing: the driver's init table already writes TXMLOC = 0, mask = all ones
    and TXME = 1, which is the documented "match every frame at the SFD"
    configuration. A frame is stamped only when its data header carries a
    non-zero TSC, so ordinary stack traffic is unaffected.
 *******************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>                                          /* strtoul() */
#include <string.h>                                          /* memcpy/memset */

#include "definitions.h"
#include "config/default/system/console/sys_console.h"
#include "config/default/library/tcpip/tcpip.h"
#include "config/default/system/time/sys_time.h"
#include "config/default/driver/lan865x/drv_lan865x.h"
#include "system/command/sys_command.h"
#include "ptp_gm.h"
#include "port_mirror.h"                                     /* MIRROR_RawTx() */

/* Interface the frames go out of: 0 = eth0, the 10BASE-T1S MAC-PHY. */
#define PTP_GM_IF               0u

/* --- registers, MMS in the upper 16 bits (CLAUDE.md section 3) -------------- */
#define OA_CONFIG0              0x00000004u
#define OA_CONFIG0_FTS_MASK     0x000000C0u   /* FTSE (bit 7) + FTSS (bit 6)    */
#define OA_STATUS0              0x00000008u
#define OA_STATUS0_TTSCAA       0x00000100u   /* capture A available, write-1-clear */
#define TTSCAH                  0x00000010u   /* capture A, TIMESTAMP[63:32] = seconds */
#define TTSCAL                  0x00000011u   /* capture A, TIMESTAMP[31:0], ns in 29:0 */
#define TS_NS_MASK              0x3FFFFFFFu

/* TSC value in the transmit data header: 1 selects capture register pair A. */
#define TSC_CAPTURE_A           0x01u
#define TSC_NONE                0x00u

/* --- frame layout ----------------------------------------------------------- */
#define ETH_HDR_LEN             14u
#define PTP_HDR_LEN             34u
#define PTP_TS_LEN              10u           /* 6 byte seconds + 4 byte nanoseconds */
#define PTP_MSG_LEN             (PTP_HDR_LEN + PTP_TS_LEN)          /* 44 */
#define PTP_FRAME_LEN           60u           /* minimum legal Ethernet frame, padded */

#define PTP_MSGTYPE_SYNC        0x0u
#define PTP_MSGTYPE_FOLLOW_UP   0x8u
#define PTP_CTRL_SYNC           0x00u
#define PTP_CTRL_FOLLOW_UP      0x02u
#define PTP_FLAG0_TWO_STEP      0x02u         /* flagField octet 0, bit 1 */
#define PTP_DOMAIN              0u
#define PTP_PORT_NUMBER         1u

/* majorSdoId / transportSpecific nibble. 0 = plain PTP over Ethernet; 802.1AS
   would use 1. Kept at 0 so a transmit matcher narrowed to PTP later on uses the
   pattern 0x88F700 documented in LAN8651_TIME_SYNC.md section 2. */
#define PTP_TRANSPORT_SPECIFIC  0x0u

/* --- timing ----------------------------------------------------------------- */
#define REG_TIMEOUT_MS          200u          /* same budget as lan865x_diag.c   */
#define TS_TIMEOUT_MS           20u           /* capture must appear within this */
#define TS_POLL_GAP_MS          1u            /* between two capture read-backs  */

typedef enum {
    ST_OFF = 0,
    ST_TS_ENABLE,        /* turning FTSE/FTSS on, RMW in flight    */
    ST_RUN,              /* idle, waiting for the next interval    */
    ST_TS_READ_H,        /* Sync is out, reading TTSCAH            */
    ST_TS_READ_L,        /* reading TTSCAL                         */
    ST_TS_GAP,           /* capture still stale, short pause       */
    ST_STOP_CLR,         /* stopping: clearing TTSCAA              */
    ST_STOP_TS           /* stopping: turning FTSE/FTSS off        */
} ptp_state_t;

static ptp_state_t s_state = ST_OFF;
static bool     s_run_requested = false;      /* what the operator asked for    */

/* Register operation in flight. The callback only sets flags; everything is
   evaluated in PTP_GM_Tasks(). */
static volatile bool     s_reg_done = false;
static volatile bool     s_reg_ok   = false;
static volatile uint32_t s_reg_val  = 0u;
static bool     s_reg_busy = false;
static uint64_t s_reg_deadline = 0u;

static uint64_t s_ticks_per_ms = 0u;
static uint64_t s_next_cycle   = 0u;          /* tick of the next Sync          */
static uint64_t s_ts_deadline  = 0u;          /* give up on the capture at this */
static uint64_t s_poll_at      = 0u;

static uint32_t s_interval_ms = PTP_GM_INTERVAL_DEFAULT_MS;
static uint16_t s_seq = 0u;                   /* sequenceId of the current cycle */

/* Timestamp shadow. Survives stop/start on purpose: it is what tells a fresh
   capture from the one still standing in the register from the last run. */
static uint32_t s_ts_hi = 0u;                 /* TTSCAH of the cycle being read */
static uint64_t s_ts_last = 0u;               /* last accepted 64-bit capture   */
static bool     s_ts_valid = false;

/* Autostart from the persistent configuration. */
static bool s_auto_enabled = false;
static bool s_auto_pending = false;
static bool s_auto_applied = false;

/* Counters, all reported by "ptp status". */
static uint32_t s_cnt_sync = 0u;
static uint32_t s_cnt_fup = 0u;
static uint32_t s_cnt_ts_timeout = 0u;
static uint32_t s_cnt_send_fail = 0u;
static uint32_t s_cnt_reg_err = 0u;

/* Frame buffers. Two of them, because the driver queues the POINTER and does not
   copy: a buffer must stay untouched until its frame has left. Sync is only
   rewritten one interval later, which is at least PTP_GM_INTERVAL_MIN_MS. */
static uint8_t s_sync[PTP_FRAME_LEN];
static uint8_t s_fup[PTP_FRAME_LEN];
static uint8_t s_mac[6];
static uint8_t s_clock_id[8];

/* --------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* --------------------------------------------------------------------------- */

static uint64_t ptp_now(void)
{
    return SYS_TIME_Counter64Get();
}

static uint64_t ptp_ms(uint32_t ms)
{
    return (uint64_t)ms * s_ticks_per_ms;
}

static bool ptp_elapsed(uint64_t deadline)
{
    return ((int64_t)(ptp_now() - deadline) >= 0);
}

static void ptp_reg_cb(void *reserved1, bool success, uint32_t addr, uint32_t value,
                       void *pTag, void *reserved2)
{
    (void)reserved1; (void)addr; (void)pTag; (void)reserved2;
    s_reg_ok  = success;
    s_reg_val = value;
    s_reg_done = true;
}

/* Issue one register operation. Returns false if the driver would not take it
   (control queue full) - the caller stays in its state and tries again. */
static bool ptp_reg_start(TCPIP_MAC_RES res)
{
    if (res != TCPIP_MAC_RES_OK) {
        return false;
    }
    s_reg_busy = true;
    s_reg_deadline = ptp_now() + ptp_ms(REG_TIMEOUT_MS);
    return true;
}

static bool ptp_reg_read(uint32_t addr)
{
    s_reg_done = false;
    s_reg_ok = false;
    return ptp_reg_start(DRV_LAN865X_ReadRegister(PTP_GM_IF, addr, true, ptp_reg_cb, NULL));
}

static bool ptp_reg_write(uint32_t addr, uint32_t value)
{
    s_reg_done = false;
    s_reg_ok = false;
    return ptp_reg_start(DRV_LAN865X_WriteRegister(PTP_GM_IF, addr, value, true, ptp_reg_cb, NULL));
}

static bool ptp_reg_rmw(uint32_t addr, uint32_t value, uint32_t mask)
{
    s_reg_done = false;
    s_reg_ok = false;
    return ptp_reg_start(DRV_LAN865X_ReadModifyWriteRegister(PTP_GM_IF, addr, value, mask, true,
                                                             ptp_reg_cb, NULL));
}

/* logMessageInterval is a base-2 logarithm of seconds and cannot express an
   arbitrary millisecond value. Pick the closest power of two and report what it
   actually stands for, so "ptp status" can say when the two disagree - otherwise
   someone later chases a mismatch that a PTP analyser rightly complains about
   and that is not a fault. */
static int8_t ptp_log_interval(uint32_t ms, uint32_t *represented_ms)
{
    int8_t best = 0;
    uint32_t best_err = 0xFFFFFFFFu;
    uint32_t best_ms = 1000u;
    int k;
    for (k = -7; k <= 4; k++) {
        uint32_t v = (k >= 0) ? (1000u << k) : (1000u >> (-k));
        uint32_t err = (v > ms) ? (v - ms) : (ms - v);
        if (err < best_err) {
            best_err = err;
            best = (int8_t)k;
            best_ms = v;
        }
    }
    if (represented_ms != NULL) {
        *represented_ms = best_ms;
    }
    return best;
}

/* Build one complete frame: Ethernet header, PTP common header, timestamp body,
   zero padding to the 60-byte minimum. */
static void ptp_build(uint8_t *f, uint8_t msgtype, uint8_t ctrl, uint8_t flag0,
                      uint16_t seq, uint32_t sec, uint32_t ns)
{
    uint8_t *p = &f[ETH_HDR_LEN];
    uint8_t *t = &f[ETH_HDR_LEN + PTP_HDR_LEN];

    memset(f, 0, PTP_FRAME_LEN);

    /* Ethernet: broadcast, our MAC, EtherType 0x88F7. Broadcast rather than the
       PTP multicast groups - the LAN865x receive filter is not set up for those
       and drops them silently (LAN8651_TIME_SYNC.md section 6). */
    memset(&f[0], 0xFFu, 6u);
    memcpy(&f[6], s_mac, 6u);
    f[12] = (uint8_t)((PTP_GM_ETHERTYPE >> 8u) & 0xFFu);
    f[13] = (uint8_t)(PTP_GM_ETHERTYPE & 0xFFu);

    p[0] = (uint8_t)((PTP_TRANSPORT_SPECIFIC << 4u) | msgtype);
    p[1] = 0x02u;                                    /* minorVersionPTP 0, versionPTP 2 */
    p[2] = (uint8_t)((PTP_MSG_LEN >> 8u) & 0xFFu);
    p[3] = (uint8_t)(PTP_MSG_LEN & 0xFFu);
    p[4] = PTP_DOMAIN;
    /* p[5] reserved, p[8..15] correctionField, p[16..19] reserved: all zero */
    p[6] = flag0;
    memcpy(&p[20], s_clock_id, 8u);                  /* sourcePortIdentity      */
    p[28] = (uint8_t)((PTP_PORT_NUMBER >> 8u) & 0xFFu);
    p[29] = (uint8_t)(PTP_PORT_NUMBER & 0xFFu);
    p[30] = (uint8_t)((seq >> 8u) & 0xFFu);
    p[31] = (uint8_t)(seq & 0xFFu);
    p[32] = ctrl;
    p[33] = (uint8_t)ptp_log_interval(s_interval_ms, NULL);

    /* 48-bit secondsField, 32-bit nanosecondsField, both big endian. The
       hardware capture is 32-bit seconds, so the top two bytes stay zero. */
    t[0] = 0u;
    t[1] = 0u;
    t[2] = (uint8_t)((sec >> 24u) & 0xFFu);
    t[3] = (uint8_t)((sec >> 16u) & 0xFFu);
    t[4] = (uint8_t)((sec >>  8u) & 0xFFu);
    t[5] = (uint8_t)(sec & 0xFFu);
    t[6] = (uint8_t)((ns >> 24u) & 0xFFu);
    t[7] = (uint8_t)((ns >> 16u) & 0xFFu);
    t[8] = (uint8_t)((ns >>  8u) & 0xFFu);
    t[9] = (uint8_t)(ns & 0xFFu);
}

/* Fetch this interface's MAC and derive the EUI-64 clockIdentity from it. */
static bool ptp_load_identity(void)
{
    TCPIP_NET_HANDLE net = TCPIP_STACK_IndexToNet(PTP_GM_IF);
    const uint8_t *mac = (net != NULL) ? TCPIP_STACK_NetAddressMac(net) : NULL;
    if (mac == NULL) {
        return false;
    }
    memcpy(s_mac, mac, 6u);
    s_clock_id[0] = mac[0];
    s_clock_id[1] = mac[1];
    s_clock_id[2] = mac[2];
    s_clock_id[3] = 0xFFu;
    s_clock_id[4] = 0xFEu;
    s_clock_id[5] = mac[3];
    s_clock_id[6] = mac[4];
    s_clock_id[7] = mac[5];
    return true;
}

/* --------------------------------------------------------------------------- */
/* Send cycle                                                                  */
/* --------------------------------------------------------------------------- */

static void ptp_send_sync(void)
{
    s_seq++;
    /* Two-step: the originTimestamp of Sync carries no useful time - the real
       egress time follows in Follow_Up. Zero is the honest value here. */
    ptp_build(s_sync, PTP_MSGTYPE_SYNC, PTP_CTRL_SYNC, PTP_FLAG0_TWO_STEP, s_seq, 0u, 0u);

    if (!DRV_LAN865X_SendRawEthFrame(PTP_GM_IF, s_sync, (uint16_t)PTP_FRAME_LEN,
                                     TSC_CAPTURE_A, NULL, NULL)) {
        s_cnt_send_fail++;
        s_next_cycle = ptp_now() + ptp_ms(s_interval_ms);
        return;
    }
    s_cnt_sync++;
    MIRROR_RawTx(s_sync, (uint16_t)PTP_FRAME_LEN);

    s_next_cycle  = ptp_now() + ptp_ms(s_interval_ms);
    s_ts_deadline = ptp_now() + ptp_ms(TS_TIMEOUT_MS);
    s_poll_at     = ptp_now();
    s_state = ST_TS_READ_H;
}

static void ptp_send_follow_up(uint32_t sec, uint32_t ns)
{
    uint64_t total = ((uint64_t)sec * 1000000000ULL) + (uint64_t)ns
                   + (uint64_t)PTP_GM_STATIC_OFFSET_NS;
    uint32_t osec = (uint32_t)(total / 1000000000ULL);
    uint32_t ons  = (uint32_t)(total % 1000000000ULL);

    ptp_build(s_fup, PTP_MSGTYPE_FOLLOW_UP, PTP_CTRL_FOLLOW_UP, 0u, s_seq, osec, ons);

    if (!DRV_LAN865X_SendRawEthFrame(PTP_GM_IF, s_fup, (uint16_t)PTP_FRAME_LEN,
                                     TSC_NONE, NULL, NULL)) {
        s_cnt_send_fail++;
        return;
    }
    s_cnt_fup++;
    MIRROR_RawTx(s_fup, (uint16_t)PTP_FRAME_LEN);
}

/* --------------------------------------------------------------------------- */
/* Public API                                                                  */
/* --------------------------------------------------------------------------- */

bool PTP_GM_IsRunning(void)
{
    return s_run_requested;
}

uint32_t PTP_GM_Interval(void)
{
    return s_interval_ms;
}

bool PTP_GM_SetInterval(uint32_t ms)
{
    if (ms < PTP_GM_INTERVAL_MIN_MS || ms > PTP_GM_INTERVAL_MAX_MS) {
        return false;
    }
    s_interval_ms = ms;
    return true;
}

bool PTP_GM_Start(void)
{
    if (s_run_requested) {
        return true;
    }
    if (!ptp_load_identity()) {
        SYS_CONSOLE_PRINT("[PTP] cannot read the eth0 MAC - is the stack up?\r\n");
        return false;
    }
    s_run_requested = true;
    s_state = ST_TS_ENABLE;
    s_reg_busy = false;
    return true;
}

void PTP_GM_Stop(void)
{
    if (!s_run_requested) {
        return;
    }
    s_run_requested = false;
    /* A cycle waiting for its capture is abandoned here, not finished: a
       Follow_Up whose Sync belongs to a stopped run must not go out. */
    s_state = ST_STOP_CLR;
    s_reg_busy = false;
}

void PTP_GM_ConfigureAutoStart(bool enable, uint32_t interval_ms)
{
    s_auto_enabled = enable;
    if (!s_run_requested && interval_ms >= PTP_GM_INTERVAL_MIN_MS
                         && interval_ms <= PTP_GM_INTERVAL_MAX_MS) {
        s_interval_ms = interval_ms;
    }
    if (!s_auto_applied) {
        s_auto_applied = true;
        s_auto_pending = enable;
    }
}

/* --------------------------------------------------------------------------- */
/* State machine                                                               */
/* --------------------------------------------------------------------------- */

void PTP_GM_Tasks(void)
{
    if (s_ticks_per_ms == 0u) {
        s_ticks_per_ms = (uint64_t)SYS_TIME_FrequencyGet() / 1000ULL;
        if (s_ticks_per_ms == 0u) {
            return;
        }
    }

    /* Autostart, once, and only from here: register access is not serviced
       before the application has settled, so an earlier start would run its
       first cycle into nothing and look like a register fault. */
    if (s_auto_pending) {
        s_auto_pending = false;
        if (PTP_GM_Start()) {
            SYS_CONSOLE_PRINT("[PTP] autostart from env: interval %u ms\r\n",
                              (unsigned)s_interval_ms);
        }
    }

    /* One shared timeout for whatever register operation is in flight. */
    if (s_reg_busy && !s_reg_done && ptp_elapsed(s_reg_deadline)) {
        s_reg_busy = false;
        s_cnt_reg_err++;
        SYS_CONSOLE_PRINT("[PTP] register operation timed out\r\n");
        if (s_state == ST_TS_ENABLE) {
            s_run_requested = false;
            s_state = ST_OFF;
        } else if (s_state == ST_STOP_CLR || s_state == ST_STOP_TS) {
            s_state = ST_OFF;
        } else {
            s_state = ST_RUN;
        }
        return;
    }

    switch (s_state) {
        case ST_OFF:
            break;

        case ST_TS_ENABLE:
            if (!s_reg_busy) {
                /* FTSE and FTSS together. FTSE alone would corrupt received
                   frames, and without FTSE no egress timestamp is captured at
                   all - the transmit side needs this bit too, which is easy to
                   miss (LAN8651_TIME_SYNC.md section 10). */
                (void)ptp_reg_rmw(OA_CONFIG0, OA_CONFIG0_FTS_MASK, OA_CONFIG0_FTS_MASK);
            } else if (s_reg_done) {
                s_reg_busy = false;
                if (!s_reg_ok) {
                    s_cnt_reg_err++;
                    s_run_requested = false;
                    s_state = ST_OFF;
                    SYS_CONSOLE_PRINT("[PTP] enabling frame timestamps failed\r\n");
                } else {
                    s_next_cycle = ptp_now();
                    s_state = ST_RUN;
                }
            }
            break;

        case ST_RUN:
            if (ptp_elapsed(s_next_cycle)) {
                ptp_send_sync();
            }
            break;

        case ST_TS_READ_H:
            if (!s_reg_busy) {
                if (ptp_elapsed(s_ts_deadline)) {
                    s_cnt_ts_timeout++;
                    s_state = ST_RUN;
                } else if (ptp_elapsed(s_poll_at)) {
                    (void)ptp_reg_read(TTSCAH);
                }
            } else if (s_reg_done) {
                s_reg_busy = false;
                if (!s_reg_ok) {
                    s_cnt_reg_err++;
                    s_state = ST_RUN;
                } else {
                    s_ts_hi = s_reg_val;
                    s_state = ST_TS_READ_L;
                }
            }
            break;

        case ST_TS_READ_L:
            if (!s_reg_busy) {
                (void)ptp_reg_read(TTSCAL);
            } else if (s_reg_done) {
                s_reg_busy = false;
                if (!s_reg_ok) {
                    s_cnt_reg_err++;
                    s_state = ST_RUN;
                } else {
                    uint64_t cap = ((uint64_t)s_ts_hi << 32u) | (uint64_t)s_reg_val;
                    if (cap != 0u && (!s_ts_valid || cap != s_ts_last)) {
                        s_ts_last = cap;
                        s_ts_valid = true;
                        ptp_send_follow_up(s_ts_hi, s_reg_val & TS_NS_MASK);
                        s_state = ST_RUN;
                    } else {
                        /* Same value as last cycle: the capture has not landed
                           yet. Wait a moment and look again. */
                        s_poll_at = ptp_now() + ptp_ms(TS_POLL_GAP_MS);
                        s_state = ST_TS_GAP;
                    }
                }
            }
            break;

        case ST_TS_GAP:
            if (ptp_elapsed(s_ts_deadline)) {
                s_cnt_ts_timeout++;
                s_state = ST_RUN;
            } else if (ptp_elapsed(s_poll_at)) {
                s_state = ST_TS_READ_H;
            }
            break;

        case ST_STOP_CLR:
            /* Clear a capture-available flag that may still be standing. The
               freshness check already guards against a stale timestamp, but
               leaving the status bit set would keep signalling an event that has
               been consumed. */
            if (!s_reg_busy) {
                (void)ptp_reg_write(OA_STATUS0, OA_STATUS0_TTSCAA);
            } else if (s_reg_done) {
                s_reg_busy = false;
                if (!s_reg_ok) {
                    s_cnt_reg_err++;
                }
                s_state = ST_STOP_TS;
            }
            break;

        case ST_STOP_TS:
            if (!s_reg_busy) {
                (void)ptp_reg_rmw(OA_CONFIG0, 0x00000000u, OA_CONFIG0_FTS_MASK);
            } else if (s_reg_done) {
                s_reg_busy = false;
                if (!s_reg_ok) {
                    s_cnt_reg_err++;
                }
                s_state = ST_OFF;
            }
            break;

        default:
            s_state = ST_OFF;
            break;
    }
}

/* --------------------------------------------------------------------------- */
/* Console                                                                     */
/* --------------------------------------------------------------------------- */

static void ptp_print_status(void)
{
    uint32_t repr = 0u;
    int8_t logival = ptp_log_interval(s_interval_ms, &repr);

    SYS_CONSOLE_PRINT("[PTP] sending: %s   interval: %u ms   seq: %u\r\n",
                      s_run_requested ? "on" : "off",
                      (unsigned)s_interval_ms, (unsigned)s_seq);
    SYS_CONSOLE_PRINT("[PTP] logMessageInterval: %d (%u ms)%s\r\n",
                      (int)logival, (unsigned)repr,
                      (repr == s_interval_ms) ? "" : "  <- field and interval differ, by design");
    SYS_CONSOLE_PRINT("[PTP] tx sync: %u   follow_up: %u   ts timeouts: %u   send fails: %u   reg errors: %u\r\n",
                      (unsigned)s_cnt_sync, (unsigned)s_cnt_fup,
                      (unsigned)s_cnt_ts_timeout, (unsigned)s_cnt_send_fail,
                      (unsigned)s_cnt_reg_err);
    if (s_ts_valid) {
        SYS_CONSOLE_PRINT("[PTP] last capture: %u s %u ns\r\n",
                          (unsigned)(s_ts_last >> 32u),
                          (unsigned)((uint32_t)s_ts_last & TS_NS_MASK));
    } else {
        SYS_CONSOLE_PRINT("[PTP] last capture: none yet\r\n");
    }
    SYS_CONSOLE_PRINT("[PTP] autostart (env ptp_auto): %s\r\n", s_auto_enabled ? "on" : "off");
}

/* ptp start | stop | interval [ms] | status */
static void cmd_ptp(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv)
{
    (void)pCmdIO;

    if (argc < 2) {
        ptp_print_status();
        SYS_CONSOLE_PRINT("usage: ptp start | stop | interval [ms] | status\r\n");
        return;
    }

    if (!strcmp(argv[1], "start")) {
        if (s_run_requested) {
            SYS_CONSOLE_PRINT("[PTP] already sending (interval %u ms)\r\n", (unsigned)s_interval_ms);
        } else if (PTP_GM_Start()) {
            SYS_CONSOLE_PRINT("[PTP] start: interval %u ms\r\n", (unsigned)s_interval_ms);
        } else {
            SYS_CONSOLE_PRINT("[PTP] start failed\r\n");
        }
        return;
    }
    if (!strcmp(argv[1], "stop")) {
        if (!s_run_requested) {
            SYS_CONSOLE_PRINT("[PTP] not sending\r\n");
        } else {
            PTP_GM_Stop();
            SYS_CONSOLE_PRINT("[PTP] stop\r\n");
        }
        return;
    }
    if (!strcmp(argv[1], "interval")) {
        if (argc < 3) {
            SYS_CONSOLE_PRINT("[PTP] interval %u ms\r\n", (unsigned)s_interval_ms);
        } else {
            uint32_t ms = (uint32_t)strtoul(argv[2], NULL, 10);
            if (!PTP_GM_SetInterval(ms)) {
                SYS_CONSOLE_PRINT("[PTP] interval must be %u..%u ms\r\n",
                                  (unsigned)PTP_GM_INTERVAL_MIN_MS,
                                  (unsigned)PTP_GM_INTERVAL_MAX_MS);
            } else {
                SYS_CONSOLE_PRINT("[PTP] interval %u ms (from the next cycle; sequence continues)\r\n",
                                  (unsigned)ms);
            }
        }
        return;
    }
    if (!strcmp(argv[1], "status")) {
        ptp_print_status();
        return;
    }
    SYS_CONSOLE_PRINT("usage: ptp start | stop | interval [ms] | status\r\n");
}

static const SYS_CMD_DESCRIPTOR ptp_cmd_tbl[] = {
    {"ptp", (SYS_CMD_FNC)cmd_ptp, ": PTP grandmaster (ptp start | stop | interval [ms] | status)"},
};

void PTP_GM_Initialize(void)
{
    if (!SYS_CMD_ADDGRP(ptp_cmd_tbl, (int)(sizeof ptp_cmd_tbl / sizeof *ptp_cmd_tbl),
                        "ptp", ": PTP grandmaster on the T1S segment")) {
        SYS_CONSOLE_PRINT("PTP: SYS_CMD_ADDGRP failed\r\n");
    }
}
