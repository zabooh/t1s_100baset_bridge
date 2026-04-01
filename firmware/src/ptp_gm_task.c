//DOM-IGNORE-BEGIN
/*
Copyright (C) 2025, Microchip Technology Inc., and its subsidiaries. All rights reserved.
PTP Grandmaster task — non-blocking state machine for T1S 100BaseT Bridge (ATSAME54P20A / LAN865x).
Ported from noIP-SAM-E54-Curiosity-PTP-Grandmaster/firmware/src/main.c.

Key differences vs. the noIP reference:
  - TC6 blocking register access replaced by DRV_LAN865X_ReadRegister() + callback flag.
  - TC6NoIP_SendEthernetPacket_TimestampA() replaced by DRV_LAN865X_SendRawEthFrame(tsc=1).
  - systick timer replaced by 1 ms service calls from a Harmony SYS_TIME periodic callback.
  - Source MAC read dynamically via TCPIP_STACK_NetAddressMac().
  - State machine split across PTP_GM_Service() calls (non-blocking).
*/
//DOM-IGNORE-END

#include <string.h>
#include <stdint.h>
#include "ptp_gm_task.h"
#include "ptp_bridge_task.h"
#if PTP_GM_USE_DRV_LAN865X_WRITEREGISTER || \
    PTP_GM_USE_DRV_LAN865X_READREGISTER || \
    PTP_GM_USE_DRV_LAN865X_SENDRAWETHFRAME || \
    PTP_GM_USE_DRV_LAN865X_GETANDCLEARTSCAPTURE
#include "config/default/driver/lan865x/drv_lan865x.h"
#endif
#include "config/default/system/console/sys_console.h"
#include "config/default/library/tcpip/tcpip.h"

#define GM_NOIP_ETHERTYPE 0x88B5u
#define GM_PTP_ETHERTYPE  ((uint16_t)(PTP_GM_PTP_ETHERTYPE & 0xFFFFu))

/* -------------------------------------------------------------------------
 * State machine
 * ---------------------------------------------------------------------- */

typedef enum {
    GM_STATE_IDLE = 0,
    GM_STATE_WAIT_PERIOD,
    GM_STATE_SEND_SYNC,
    GM_STATE_READ_TXMCTL,
    GM_STATE_WAIT_TXMCTL,
    GM_STATE_READ_STATUS0,
    GM_STATE_WAIT_STATUS0,
    GM_STATE_READ_TTSCA_H,
    GM_STATE_WAIT_TTSCA_H,
    GM_STATE_READ_TTSCA_L,
    GM_STATE_WAIT_TTSCA_L,
    GM_STATE_WRITE_CLEAR,
    GM_STATE_WAIT_CLEAR,
    GM_STATE_SEND_FOLLOWUP
} gmState_t;

/* -------------------------------------------------------------------------
 * Module-level state
 * ---------------------------------------------------------------------- */

static gmState_t        gm_state            = GM_STATE_IDLE;
static volatile bool    gm_op_done          = false;   /* set by read/write callback */
static volatile uint32_t gm_op_val          = 0u;      /* value from last read callback */
static uint32_t         gm_status0          = 0u;      /* OA_STATUS0 snapshot */
static uint32_t         gm_ts_sec           = 0u;      /* TX timestamp: seconds */
static uint32_t         gm_ts_nsec          = 0u;      /* TX timestamp: nanoseconds */
static uint32_t         gm_tick_ms          = 0u;      /* ms counter, incremented per Service() call */
static uint32_t         gm_period_start     = 0u;      /* tick at start of current period */
static uint16_t         gm_seq_id           = 0u;
static uint32_t         gm_sync_cnt         = 0u;
static uint8_t          gm_retry_cnt        = 0u;
static uint32_t         gm_wait_ticks       = 0u;      /* ticks spent waiting for a callback */
static volatile bool    gm_tx_busy          = false;
static uint32_t         gm_sync_interval_ms = PTP_GM_SYNC_PERIOD_MS;
static uint8_t          gm_src_mac[6]       = {0u};
static ptp_gm_dst_mode_t gm_dst_mode        = PTP_GM_DST_MULTICAST;

/* Frame buffers */
static uint8_t gm_sync_buf[60];       /* 14 (eth) + 44 (syncMsg_t) + 2 pad bytes */
static uint8_t gm_followup_buf[90];   /* 14 (eth) + 76 (followUpMsg_t) */
#if (PTP_GM_SYNC_TX_MODE == 1)
static uint8_t gm_noip_buf[60];       /* matches Test noip_send frame size */
static uint32_t gm_noip_seq = 0u;
#endif

typedef char gm_sync_msg_size_must_be_44[(sizeof(syncMsg_t) == 44u) ? 1 : -1];

static const char *gm_state_to_str(gmState_t state)
{
    switch (state) {
        case GM_STATE_IDLE:          return "GM_STATE_IDLE";
        case GM_STATE_WAIT_PERIOD:   return "GM_STATE_WAIT_PERIOD";
        case GM_STATE_SEND_SYNC:     return "GM_STATE_SEND_SYNC";
        case GM_STATE_READ_TXMCTL:   return "GM_STATE_READ_TXMCTL";
        case GM_STATE_WAIT_TXMCTL:   return "GM_STATE_WAIT_TXMCTL";
        case GM_STATE_READ_STATUS0:  return "GM_STATE_READ_STATUS0";
        case GM_STATE_WAIT_STATUS0:  return "GM_STATE_WAIT_STATUS0";
        case GM_STATE_READ_TTSCA_H:  return "GM_STATE_READ_TTSCA_H";
        case GM_STATE_WAIT_TTSCA_H:  return "GM_STATE_WAIT_TTSCA_H";
        case GM_STATE_READ_TTSCA_L:  return "GM_STATE_READ_TTSCA_L";
        case GM_STATE_WAIT_TTSCA_L:  return "GM_STATE_WAIT_TTSCA_L";
        case GM_STATE_WRITE_CLEAR:   return "GM_STATE_WRITE_CLEAR";
        case GM_STATE_WAIT_CLEAR:    return "GM_STATE_WAIT_CLEAR";
        case GM_STATE_SEND_FOLLOWUP: return "GM_STATE_SEND_FOLLOWUP";
        default:                     return "GM_STATE_UNKNOWN";
    }
}

static void gm_set_state(gmState_t nextState, uint32_t line)
{
    if (gm_state != nextState) {
        gm_state = nextState;
        SYS_CONSOLE_PRINT("[PTP-GM][STATE] %s @L%lu\r\n",
                          gm_state_to_str(nextState),
                          (unsigned long)line);
    }
}

#define GM_SET_STATE(_nextState) gm_set_state((_nextState), (uint32_t)__LINE__)

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Generic read/write callback — sets the done flag and stores the read value */
#if PTP_GM_USE_DRV_LAN865X_WRITEREGISTER || PTP_GM_USE_DRV_LAN865X_READREGISTER
static void gm_op_cb(void *r1, bool ok, uint32_t addr,
                     uint32_t value, void *tag, void *r2)
{
    (void)r1; (void)ok; (void)addr; (void)tag; (void)r2;
    gm_op_val  = value;
    gm_op_done = true;
}
#endif

/* TX-done callback for raw Sync / FollowUp frames */
static void gm_tx_cb(void *pInst, const uint8_t *pTx,
                     uint16_t len, void *pTag, void *pGlobalTag)
{
    (void)pInst; (void)pTx; (void)len; (void)pTag; (void)pGlobalTag;
    gm_tx_busy = false;
}

/* Fire-and-forget register write (no callback needed) */
static void gm_write(uint32_t addr, uint32_t value)
{
#if PTP_GM_USE_DRV_LAN865X_WRITEREGISTER
    DRV_LAN865X_WriteRegister(0u, addr, value, true, NULL, NULL);
#else
    (void)addr;
    (void)value;
#endif
}

static bool gm_read_register(uint32_t addr, bool useCallbackProtectedMode)
{
#if PTP_GM_USE_DRV_LAN865X_READREGISTER
    return DRV_LAN865X_ReadRegister(0u, addr, useCallbackProtectedMode, gm_op_cb, NULL);
#else
    (void)addr;
    (void)useCallbackProtectedMode;
    return false;
#endif
}

static bool gm_write_register(uint32_t addr, uint32_t value, bool useCallbackProtectedMode)
{
#if PTP_GM_USE_DRV_LAN865X_WRITEREGISTER
    return DRV_LAN865X_WriteRegister(0u, addr, value, useCallbackProtectedMode, gm_op_cb, NULL);
#else
    (void)addr;
    (void)value;
    (void)useCallbackProtectedMode;
    return false;
#endif
}

static bool gm_send_raw_eth_frame(const uint8_t *frame, uint16_t length,
                                  uint8_t tsc,
                                  void (*callback)(void *, const uint8_t *, uint16_t, void *, void *),
                                  void *tag)
{
#if PTP_GM_USE_DRV_LAN865X_SENDRAWETHFRAME
    return DRV_LAN865X_SendRawEthFrame(0u, frame, length, tsc, callback, tag);
#else
    (void)frame;
    (void)length;
    (void)tsc;
    (void)callback;
    (void)tag;
    return false;
#endif
}

static uint32_t gm_get_and_clear_ts_capture(void)
{
#if PTP_GM_USE_DRV_LAN865X_GETANDCLEARTSCAPTURE
    return DRV_LAN865X_GetAndClearTsCapture(0u);
#else
    return 0u;
#endif
}

/* Build PTP multicast Ethernet header at dst (14 bytes) */
static void build_eth_header(uint8_t *dst)
{
    if (gm_dst_mode == PTP_GM_DST_BROADCAST) {
        /* Destination: Layer-2 broadcast */
        dst[0] = 0xFFu; dst[1] = 0xFFu; dst[2] = 0xFFu;
        dst[3] = 0xFFu; dst[4] = 0xFFu; dst[5] = 0xFFu;
    } else {
        /* Destination: PTP Layer-2 multicast 01:80:C2:00:00:0E */
        dst[0] = 0x01u; dst[1] = 0x80u; dst[2] = 0xC2u;
        dst[3] = 0x00u; dst[4] = 0x00u; dst[5] = 0x0Eu;
    }
    /* Source MAC */
    dst[6]  = gm_src_mac[0]; dst[7]  = gm_src_mac[1];
    dst[8]  = gm_src_mac[2]; dst[9]  = gm_src_mac[3];
    dst[10] = gm_src_mac[4]; dst[11] = gm_src_mac[5];
    /* Configurable EtherType for PTP-payload diagnostics (default 0x88F7). */
    dst[12] = (uint8_t)((GM_PTP_ETHERTYPE >> 8u) & 0xFFu);
    dst[13] = (uint8_t)( GM_PTP_ETHERTYPE        & 0xFFu);
}

/* Fill clockIdentity from MAC (EUI-64: MAC[0..2]+FF+FE+MAC[3..5]) */
static void fill_clock_identity(uint8_t *ci)
{
    ci[0] = gm_src_mac[0]; ci[1] = gm_src_mac[1]; ci[2] = gm_src_mac[2];
    ci[3] = 0xFFu;          ci[4] = 0xFEu;
    ci[5] = gm_src_mac[3]; ci[6] = gm_src_mac[4]; ci[7] = gm_src_mac[5];
}

static void build_sync(void)
{
    memset(gm_sync_buf, 0, sizeof(gm_sync_buf));
    build_eth_header(gm_sync_buf);

    syncMsg_t *msg = (syncMsg_t *)(&gm_sync_buf[14]);
    msg->header.tsmt             = 0x00u;          /* transportSpecific=0, messageType=Sync */
    msg->header.version          = 0x02u;          /* PTPv2 */
    msg->header.messageLength    = htons(0x002Cu); /* 44 bytes */
    fill_clock_identity((uint8_t *)msg->header.sourcePortIdentity.clockIdentity);
    msg->header.sourcePortIdentity.portNumber = htons(1u);
    msg->header.sequenceID       = htons(gm_seq_id);

#if (PTP_GM_SYNC_BUILD_LEVEL >= 2)
    msg->header.controlField       = 0x00u;
    msg->header.logMessageInterval = 0x7Fu;
#endif

#if (PTP_GM_SYNC_BUILD_LEVEL >= 3)
    /* Legacy profile from earlier runs. */
    msg->header.tsmt               = 0x10u;
    msg->header.flags[0]           = 0x02u;
    msg->header.flags[1]           = 0x08u;
    msg->header.logMessageInterval = 0xFDu;
    msg->header.controlField       = 0x02u;
#endif
}

static void build_followup(uint32_t sec, uint32_t nsec)
{
    memset(gm_followup_buf, 0, sizeof(gm_followup_buf));
    build_eth_header(gm_followup_buf);

    followUpMsg_t *msg = (followUpMsg_t *)(&gm_followup_buf[14]);
    msg->header.tsmt             = 0x18u;          /* messageType = Follow_Up */
    msg->header.version          = 0x02u;
    msg->header.messageLength    = htons(0x004Cu); /* 76 bytes */
    msg->header.flags[1]         = 0x08u;
    msg->header.logMessageInterval = 0xFDu;
    msg->header.controlField     = 0x02u;
    fill_clock_identity((uint8_t *)msg->header.sourcePortIdentity.clockIdentity);
    msg->header.sourcePortIdentity.portNumber = htons(1u);
    msg->header.sequenceID       = htons(gm_seq_id);

    /* preciseOriginTimestamp — big-endian (htons/htonl swap bytes) */
    msg->preciseOriginTimestamp.secondsMsb = 0u;
    msg->preciseOriginTimestamp.secondsLsb = htonl(sec);
    msg->preciseOriginTimestamp.nanoseconds = htonl(nsec);

    /* Organization-Specific TLV (type 0x0003) */
    msg->tlv.tlvType            = htons(0x0003u);
    msg->tlv.lengthField        = htons(28u);
    msg->tlv.organizationId[0]  = 0x00u;
    msg->tlv.organizationId[1]  = 0x80u;
    msg->tlv.organizationId[2]  = 0xC2u;
    msg->tlv.organizationSubType[2] = 0x01u;  /* cumulativeRateRatio TLV */
}

#if (PTP_GM_SYNC_TX_MODE == 1)
static void build_noip_test_frame(void)
{
    /* Match cmd_noip_send in app.c: broadcast + EtherType 0x88B5 + 0xAA payload */
    gm_noip_buf[0] = 0xFFu; gm_noip_buf[1] = 0xFFu; gm_noip_buf[2] = 0xFFu;
    gm_noip_buf[3] = 0xFFu; gm_noip_buf[4] = 0xFFu; gm_noip_buf[5] = 0xFFu;

    gm_noip_buf[6]  = gm_src_mac[0]; gm_noip_buf[7]  = gm_src_mac[1];
    gm_noip_buf[8]  = gm_src_mac[2]; gm_noip_buf[9]  = gm_src_mac[3];
    gm_noip_buf[10] = gm_src_mac[4]; gm_noip_buf[11] = gm_src_mac[5];

    gm_noip_buf[12] = (uint8_t)((GM_NOIP_ETHERTYPE >> 8u) & 0xFFu);
    gm_noip_buf[13] = (uint8_t)( GM_NOIP_ETHERTYPE        & 0xFFu);

    memset(&gm_noip_buf[14], 0xAAu, 46u);

    gm_noip_seq++;
    gm_noip_buf[14] = (uint8_t)((gm_noip_seq >> 24u) & 0xFFu);
    gm_noip_buf[15] = (uint8_t)((gm_noip_seq >> 16u) & 0xFFu);
    gm_noip_buf[16] = (uint8_t)((gm_noip_seq >>  8u) & 0xFFu);
    gm_noip_buf[17] = (uint8_t)( gm_noip_seq          & 0xFFu);
}
#endif

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void PTP_GM_Init(void)
{
    /* Read board MAC address from eth0 (LAN865x interface is index 0) */
    TCPIP_NET_HANDLE netH = TCPIP_STACK_IndexToNet(0);
    if (netH != NULL) {
        const uint8_t *pMac = TCPIP_STACK_NetAddressMac(netH);
        if (pMac != NULL) {
            memcpy(gm_src_mac, pMac, 6);
        }
    }

    /* --- TX Match registers: arm configured EtherType detection --- */
    gm_write(GM_TXMLOC,    30u);         /* byte offset of EtherType in frame */
    gm_write(GM_TXMPATH,   (GM_PTP_ETHERTYPE >> 8u) & 0xFFu); /* pattern high byte */
    gm_write(GM_TXMPATL,   (((uint32_t)(GM_PTP_ETHERTYPE & 0xFFu)) << 8u) | 0x10u); /* low byte + tsmt */
    gm_write(GM_TXMMSKH,   0x00u);       /* no masking */
    gm_write(GM_TXMMSKL,   0x00u);
    /* NOTE: GM_TXMCTL is NOT armed here — it is re-armed before every Sync TX
     * in GM_STATE_SEND_SYNC, so PLCA resets cannot clear the arm permanently. */

    /* MAC time increment: 40 ns per clock period */
    gm_write(MAC_TI, 40u);

    /* NOTE: OA_CONFIG0 (cut-through) and PADCTRL writes intentionally omitted.
     * Writing these registers while PLCA is running causes TC6Error_SyncLost. */

    /* PPS output enable (optional, makes timing visible on an oscilloscope) */
    gm_write(PPSCTL, 0x0000007Du);

    /* Reset state machine */
    gm_op_done          = false;
    gm_tx_busy          = false;
    GM_SET_STATE(GM_STATE_WAIT_PERIOD);
    gm_seq_id           = 0u;
    gm_sync_cnt         = 0u;
    gm_retry_cnt        = 0u;
    gm_wait_ticks       = 0u;
    gm_tick_ms          = 0u;
    gm_period_start     = 0u;

    SYS_CONSOLE_PRINT("[PTP-GM] Init complete (MAC %02X:%02X:%02X:%02X:%02X:%02X)\r\n",
                       gm_src_mac[0], gm_src_mac[1], gm_src_mac[2],
                       gm_src_mac[3], gm_src_mac[4], gm_src_mac[5]);
}

void PTP_GM_Service(void)
{
    gm_tick_ms++;

    switch (gm_state) {

        /* ---- IDLE: never re-entered after Init ---- */
        case GM_STATE_IDLE:
            break;

        /* ---- Wait for the next 125 ms period ---- */
        case GM_STATE_WAIT_PERIOD:
            if ((gm_tick_ms - gm_period_start) >= gm_sync_interval_ms) {
                gm_period_start = gm_tick_ms;
                GM_SET_STATE(GM_STATE_SEND_SYNC);
            }
            break;

        /* ---- Build and send Sync-class TX frame ---- */
        case GM_STATE_SEND_SYNC:
            if (gm_tx_busy) {
                break;   /* previous TX still running; retry next tick */
            }
#if (PTP_GM_SYNC_TX_MODE == 1)
            build_noip_test_frame();
#else
            build_sync();
#endif
            gm_tx_busy   = true;
            gm_retry_cnt = 0u;
#if (PTP_GM_SYNC_TX_MODE == 1)
            if (!gm_send_raw_eth_frame(gm_noip_buf, sizeof(gm_noip_buf),
                                       0x00u, gm_tx_cb, NULL)) {
#else
            if (!gm_send_raw_eth_frame(gm_sync_buf, sizeof(gm_sync_buf),
                                       0x00u, gm_tx_cb, NULL)) {
#endif
                gm_tx_busy = false;
                GM_SET_STATE(GM_STATE_WAIT_PERIOD);
                break;
            }
            gm_sync_cnt++;
            GM_SET_STATE(GM_STATE_WAIT_PERIOD);
            break;

        /* ---- Sent READ(TXMCTL); start in next state ---- */
        case GM_STATE_READ_TXMCTL:
            gm_op_done = false;
            if (!gm_read_register(GM_TXMCTL, true)) {
                GM_SET_STATE(GM_STATE_WAIT_PERIOD);
                break;
            }
            GM_SET_STATE(GM_STATE_WAIT_TXMCTL);
            break;

        /* ---- Wait for TXMCTL read; check TXPMDET ---- */
        case GM_STATE_WAIT_TXMCTL:
            if (!gm_op_done) {
                if (++gm_wait_ticks >= 200u) {
                    SYS_CONSOLE_PRINT("[PTP-GM] WAIT_TXMCTL cb timeout, retry\r\n");
                    gm_wait_ticks = 0u;
                    GM_SET_STATE(GM_STATE_WAIT_PERIOD);
                }
                break;
            }
            gm_wait_ticks = 0u;
            if (gm_op_val & GM_TXMCTL_TXPMDET) {
                /* Pattern detected: poll captured STATUS0 bits saved by _OnStatus0.
                 * Do NOT issue a ReadRegister(STATUS0) here — the driver's interrupt
                 * handler clears STATUS0 (W1C) before our read could complete. */
                gm_retry_cnt  = 0u;
                gm_wait_ticks = 0u;
                GM_SET_STATE(GM_STATE_WAIT_STATUS0);
            } else {
                /* Not detected yet: retry up to MAX_RETRIES */
                gm_retry_cnt++;
                if (gm_retry_cnt >= PTP_GM_MAX_RETRIES) {
                    SYS_CONSOLE_PRINT("[PTP-GM] TXPMDET timeout after Sync #%u\r\n",
                                       (unsigned)gm_seq_id);
                    GM_SET_STATE(GM_STATE_WAIT_PERIOD);
                } else {
                    GM_SET_STATE(GM_STATE_READ_TXMCTL);
                }
            }
            break;

        /* ---- READ_STATUS0 (legacy, falls through to WAIT_STATUS0 poll) ---- */
        case GM_STATE_READ_STATUS0:
            GM_SET_STATE(GM_STATE_WAIT_STATUS0);
            break;

        /* ---- Poll TTSCAA/B/C bits captured by the driver's _OnStatus0 handler ---- */
        case GM_STATE_WAIT_STATUS0:
        {
            /* The driver's interrupt handler (_OnStatus0) reads STATUS0 and clears
             * it via W1C before any application-level ReadRegister could complete.
             * We therefore retrieve the saved bits from DRV_LAN865X_GetAndClearTsCapture
             * instead of issuing our own register read. */
            uint32_t tsCapture = gm_get_and_clear_ts_capture();
            if (0u != (tsCapture & (GM_STS0_TTSCAA | GM_STS0_TTSCAB | GM_STS0_TTSCAC))) {
                gm_status0    = tsCapture;
                gm_wait_ticks = 0u;
                GM_SET_STATE(GM_STATE_READ_TTSCA_H);
            } else {
                if (++gm_wait_ticks >= 500u) {  /* 500 ms max wait */
                    SYS_CONSOLE_PRINT("[PTP-GM] TTSCA not set after Sync #%u\r\n",
                                       (unsigned)gm_seq_id);
                    gm_wait_ticks = 0u;
                    GM_SET_STATE(GM_STATE_WAIT_PERIOD);
                }
            }
            break;
        }

        /* ---- Read seconds register of available capture slot ---- */
        case GM_STATE_READ_TTSCA_H:
        {
            uint32_t secReg = GM_OA_TTSCAH;
            if      (gm_status0 & GM_STS0_TTSCAB) { secReg = GM_OA_TTSCBH; }
            else if (gm_status0 & GM_STS0_TTSCAC) { secReg = GM_OA_TTSCCH; }
            gm_op_done = false;
            if (!gm_read_register(secReg, false)) {
                GM_SET_STATE(GM_STATE_WAIT_PERIOD);
                break;
            }
            GM_SET_STATE(GM_STATE_WAIT_TTSCA_H);
            break;
        }

        /* ---- Store seconds, issue nanoseconds read ---- */
        case GM_STATE_WAIT_TTSCA_H:
            if (!gm_op_done) {
                if (++gm_wait_ticks >= 200u) {
                    SYS_CONSOLE_PRINT("[PTP-GM] WAIT_TTSCA_H cb timeout, retry\r\n");
                    gm_wait_ticks = 0u;
                    GM_SET_STATE(GM_STATE_WAIT_PERIOD);
                }
                break;
            }
            gm_wait_ticks = 0u;
            gm_ts_sec  = gm_op_val;
            GM_SET_STATE(GM_STATE_READ_TTSCA_L);
            break;

        /* ---- Read nanoseconds register of same capture slot ---- */
        case GM_STATE_READ_TTSCA_L:
        {
            uint32_t nsecReg = GM_OA_TTSCAL;
            if      (gm_status0 & GM_STS0_TTSCAB) { nsecReg = GM_OA_TTSCBL; }
            else if (gm_status0 & GM_STS0_TTSCAC) { nsecReg = GM_OA_TTSCCL; }
            gm_op_done = false;
            if (!gm_read_register(nsecReg, false)) {
                GM_SET_STATE(GM_STATE_WAIT_PERIOD);
                break;
            }
            GM_SET_STATE(GM_STATE_WAIT_TTSCA_L);
            break;
        }

        /* ---- Store nanoseconds, issue W1C clear of OA_STATUS0 ---- */
        case GM_STATE_WAIT_TTSCA_L:
            if (!gm_op_done) {
                if (++gm_wait_ticks >= 200u) {
                    SYS_CONSOLE_PRINT("[PTP-GM] WAIT_TTSCA_L cb timeout, retry\r\n");
                    gm_wait_ticks = 0u;
                    GM_SET_STATE(GM_STATE_WAIT_PERIOD);
                }
                break;
            }
            gm_wait_ticks = 0u;
            gm_ts_nsec = gm_op_val;
            GM_SET_STATE(GM_STATE_WRITE_CLEAR);
            break;

        /* ---- Write back OA_STATUS0 to clear capture flags (W1C) ---- */
        case GM_STATE_WRITE_CLEAR:
            gm_op_done = false;
            if (!gm_write_register(GM_OA_STATUS0, gm_status0, false)) {
                GM_SET_STATE(GM_STATE_WAIT_PERIOD);
                break;
            }
            GM_SET_STATE(GM_STATE_WAIT_CLEAR);
            break;

        /* ---- Wait for write ACK, then build and send FollowUp ---- */
        case GM_STATE_WAIT_CLEAR:
            if (!gm_op_done) {
                if (++gm_wait_ticks >= 200u) {
                    SYS_CONSOLE_PRINT("[PTP-GM] WAIT_CLEAR cb timeout, retry\r\n");
                    gm_wait_ticks = 0u;
                    GM_SET_STATE(GM_STATE_WAIT_PERIOD);
                }
                break;
            }
            gm_wait_ticks = 0u;
            GM_SET_STATE(GM_STATE_SEND_FOLLOWUP);
            break;

        /* ---- Apply static offset, build FollowUp, send ---- */
        case GM_STATE_SEND_FOLLOWUP:
        {
            /* Apply static TX path compensation */
            uint32_t nsec = gm_ts_nsec + PTP_GM_STATIC_OFFSET;
            uint32_t sec  = gm_ts_sec;
            if (nsec > PTP_GM_MAX_TN_VAL) {
                nsec -= PTP_GM_MAX_TN_VAL;
                sec++;
            }
            build_followup(sec, nsec);
            /* Send FollowUp without TSC (tsc=0) */
            (void)gm_send_raw_eth_frame(gm_followup_buf,
                                        sizeof(gm_followup_buf),
                                        0x00u, NULL, NULL);
            gm_seq_id++;
            gm_sync_cnt++;
            GM_SET_STATE(GM_STATE_WAIT_PERIOD);
            break;
        }

        default:
            GM_SET_STATE(GM_STATE_WAIT_PERIOD);
            break;
    }
}

void PTP_GM_Deinit(void)
{
    /* Best-effort disarm of TX-Match logic; safe no-op when register writes are disabled. */
    gm_write(GM_TXMCTL,  0u);
    gm_write(GM_TXMPATH, 0u);
    gm_write(GM_TXMPATL, 0u);
    gm_write(GM_TXMMSKH, 0u);
    gm_write(GM_TXMMSKL, 0u);
    gm_write(GM_TXMLOC,  0u);   /* clear TX-Match location (set during Init) */

    /* Stop MAC TSU free-running timer — prevents residual PTP timestamp activity
     * from interfering with normal (NoIP) Ethernet traffic after ptp_mode off. */
    gm_write(MAC_TI,  0u);

    /* Disable PPS output pulse that was started during Init. */
    gm_write(PPSCTL,  0u);

    gm_tx_busy      = false;
    gm_op_done      = false;
    gm_op_val       = 0u;
    gm_status0      = 0u;
    gm_ts_sec       = 0u;
    gm_ts_nsec      = 0u;
    gm_retry_cnt    = 0u;
    gm_wait_ticks   = 0u;
    gm_period_start = gm_tick_ms;
    GM_SET_STATE(GM_STATE_IDLE);

    SYS_CONSOLE_PRINT("[PTP-GM] deinit: TX-Match/TSU/PPS disarmed, state reset\r\n");
}

void PTP_GM_GetStatus(uint32_t *pSyncCount, uint32_t *pState)
{
    if (pSyncCount != NULL) { *pSyncCount = gm_sync_cnt; }
    if (pState     != NULL) { *pState     = (uint32_t)gm_state; }
}

void PTP_GM_SetSyncInterval(uint32_t intervalMs)
{
    if (intervalMs >= 10u && intervalMs <= 10000u) {
        gm_sync_interval_ms = intervalMs;
    } else {
        gm_sync_interval_ms = PTP_GM_SYNC_PERIOD_MS;
    }
}

void PTP_GM_SetDstMode(ptp_gm_dst_mode_t mode)
{
    if (mode == PTP_GM_DST_BROADCAST) {
        gm_dst_mode = PTP_GM_DST_BROADCAST;
    } else {
        gm_dst_mode = PTP_GM_DST_MULTICAST;
    }
}

ptp_gm_dst_mode_t PTP_GM_GetDstMode(void)
{
    return gm_dst_mode;
}
