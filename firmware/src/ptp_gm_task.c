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
#include "config/default/driver/lan865x/drv_lan865x.h"
#include "config/default/system/console/sys_console.h"
#include "config/default/library/tcpip/tcpip.h"

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
static volatile bool    gm_tx_busy          = false;
static uint32_t         gm_sync_interval_ms = PTP_GM_SYNC_PERIOD_MS;
static uint8_t          gm_src_mac[6]       = {0u};

/* Frame buffers */
static uint8_t gm_sync_buf[58];       /* 14 (eth) + 44 (syncMsg_t) */
static uint8_t gm_followup_buf[90];   /* 14 (eth) + 76 (followUpMsg_t) */

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Generic read/write callback — sets the done flag and stores the read value */
static void gm_op_cb(void *r1, bool ok, uint32_t addr,
                     uint32_t value, void *tag, void *r2)
{
    (void)r1; (void)ok; (void)addr; (void)tag; (void)r2;
    gm_op_val  = value;
    gm_op_done = true;
}

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
    DRV_LAN865X_WriteRegister(0u, addr, value, true, NULL, NULL);
}

/* Build PTP multicast Ethernet header at dst (14 bytes) */
static void build_eth_header(uint8_t *dst)
{
    /* Destination: PTP Layer-2 multicast 01:80:C2:00:00:0E */
    dst[0] = 0x01u; dst[1] = 0x80u; dst[2] = 0xC2u;
    dst[3] = 0x00u; dst[4] = 0x00u; dst[5] = 0x0Eu;
    /* Source MAC */
    dst[6]  = gm_src_mac[0]; dst[7]  = gm_src_mac[1];
    dst[8]  = gm_src_mac[2]; dst[9]  = gm_src_mac[3];
    dst[10] = gm_src_mac[4]; dst[11] = gm_src_mac[5];
    /* EtherType 0x88F7 */
    dst[12] = 0x88u; dst[13] = 0xF7u;
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
    msg->header.tsmt             = 0x10u;          /* messageType = Sync */
    msg->header.version          = 0x02u;
    msg->header.messageLength    = htons(0x002Cu); /* 44 bytes */
    msg->header.flags[0]         = 0x02u;          /* twoStepFlag */
    msg->header.flags[1]         = 0x08u;
    msg->header.logMessageInterval = 0xFDu;        /* -3 → 125 ms */
    msg->header.controlField     = 0x02u;
    fill_clock_identity((uint8_t *)msg->header.sourcePortIdentity.clockIdentity);
    msg->header.sourcePortIdentity.portNumber = htons(1u);
    msg->header.sequenceID       = htons(gm_seq_id);
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

    /* --- TX Match registers: arm EtherType 0x88F7 detection --- */
    gm_write(GM_TXMLOC,    30u);         /* byte offset of EtherType in frame */
    gm_write(GM_TXMPATH,   0x88u);       /* pattern high byte */
    gm_write(GM_TXMPATL,   0xF710u);     /* pattern low byte + next byte */
    gm_write(GM_TXMMSKH,   0x00u);       /* no masking */
    gm_write(GM_TXMMSKL,   0x00u);
    gm_write(GM_TXMCTL,    0x02u);       /* arm TX match */

    /* MAC time increment: 40 ns per clock period */
    gm_write(MAC_TI, 40u);

    /* OA_CONFIG0: enable TX+RX cut-through (bits 7:6) */
    DRV_LAN865X_ReadModifyWriteRegister(0u, GM_OA_CONFIG0, 0xC0u, 0xC0u, true, NULL, NULL);

    /* PADCTRL: pad timing adjustment (bit 9 := 1, bit 8 := 0) */
    DRV_LAN865X_ReadModifyWriteRegister(0u, PADCTRL, 0x100u, 0x300u, true, NULL, NULL);

    /* PPS output enable (optional, makes timing visible on an oscilloscope) */
    gm_write(PPSCTL, 0x0000007Du);

    /* Reset state machine */
    gm_op_done          = false;
    gm_tx_busy          = false;
    gm_state            = GM_STATE_WAIT_PERIOD;
    gm_seq_id           = 0u;
    gm_sync_cnt         = 0u;
    gm_retry_cnt        = 0u;
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
                gm_state        = GM_STATE_SEND_SYNC;
            }
            break;

        /* ---- Build and send Sync with TSC=1 ---- */
        case GM_STATE_SEND_SYNC:
            if (gm_tx_busy) {
                break;   /* previous TX still running; retry next tick */
            }
            build_sync();
            gm_tx_busy   = true;
            gm_retry_cnt = 0u;
            if (!DRV_LAN865X_SendRawEthFrame(0u, gm_sync_buf, sizeof(gm_sync_buf),
                                              0x01u, gm_tx_cb, NULL)) {
                gm_tx_busy = false;   /* send failed; retry next tick */
                break;
            }
            /* Immediately kick off TXMCTL polling */
            gm_op_done = false;
            DRV_LAN865X_ReadRegister(0u, GM_TXMCTL, true, gm_op_cb, NULL);
            gm_state = GM_STATE_WAIT_TXMCTL;
            break;

        /* ---- Sent READ(TXMCTL); start in next state ---- */
        case GM_STATE_READ_TXMCTL:
            gm_op_done = false;
            DRV_LAN865X_ReadRegister(0u, GM_TXMCTL, true, gm_op_cb, NULL);
            gm_state = GM_STATE_WAIT_TXMCTL;
            break;

        /* ---- Wait for TXMCTL read; check TXPMDET ---- */
        case GM_STATE_WAIT_TXMCTL:
            if (!gm_op_done) break;
            if (gm_op_val & GM_TXMCTL_TXPMDET) {
                /* Pattern detected: read OA_STATUS0 */
                gm_op_done = false;
                DRV_LAN865X_ReadRegister(0u, GM_OA_STATUS0, false, gm_op_cb, NULL);
                gm_state = GM_STATE_WAIT_STATUS0;
            } else {
                /* Not detected yet: retry up to MAX_RETRIES */
                gm_retry_cnt++;
                if (gm_retry_cnt >= PTP_GM_MAX_RETRIES) {
                    SYS_CONSOLE_PRINT("[PTP-GM] TXPMDET timeout after Sync #%u\r\n",
                                       (unsigned)gm_seq_id);
                    gm_state = GM_STATE_WAIT_PERIOD;
                } else {
                    gm_state = GM_STATE_READ_TXMCTL;
                }
            }
            break;

        /* ---- READ_STATUS0 is issued inside WAIT_TXMCTL; land here on re-issue ---- */
        case GM_STATE_READ_STATUS0:
            gm_op_done = false;
            DRV_LAN865X_ReadRegister(0u, GM_OA_STATUS0, false, gm_op_cb, NULL);
            gm_state = GM_STATE_WAIT_STATUS0;
            break;

        /* ---- Wait for OA_STATUS0; check timestamp capture available ---- */
        case GM_STATE_WAIT_STATUS0:
            if (!gm_op_done) break;
            gm_status0 = gm_op_val;
            if (gm_status0 & (GM_STS0_TTSCAA | GM_STS0_TTSCAB | GM_STS0_TTSCAC)) {
                /* At least one capture slot has data: read seconds register */
                gm_state = GM_STATE_READ_TTSCA_H;
            } else {
                gm_retry_cnt++;
                if (gm_retry_cnt >= PTP_GM_MAX_RETRIES) {
                    SYS_CONSOLE_PRINT("[PTP-GM] TTSCA not set after Sync #%u\r\n",
                                       (unsigned)gm_seq_id);
                    gm_state = GM_STATE_WAIT_PERIOD;
                } else {
                    gm_state = GM_STATE_READ_STATUS0;
                }
            }
            break;

        /* ---- Read seconds register of available capture slot ---- */
        case GM_STATE_READ_TTSCA_H:
        {
            uint32_t secReg = GM_OA_TTSCAH;
            if      (gm_status0 & GM_STS0_TTSCAB) { secReg = GM_OA_TTSCBH; }
            else if (gm_status0 & GM_STS0_TTSCAC) { secReg = GM_OA_TTSCCH; }
            gm_op_done = false;
            DRV_LAN865X_ReadRegister(0u, secReg, false, gm_op_cb, NULL);
            gm_state = GM_STATE_WAIT_TTSCA_H;
            break;
        }

        /* ---- Store seconds, issue nanoseconds read ---- */
        case GM_STATE_WAIT_TTSCA_H:
            if (!gm_op_done) break;
            gm_ts_sec  = gm_op_val;
            gm_state   = GM_STATE_READ_TTSCA_L;
            break;

        /* ---- Read nanoseconds register of same capture slot ---- */
        case GM_STATE_READ_TTSCA_L:
        {
            uint32_t nsecReg = GM_OA_TTSCAL;
            if      (gm_status0 & GM_STS0_TTSCAB) { nsecReg = GM_OA_TTSCBL; }
            else if (gm_status0 & GM_STS0_TTSCAC) { nsecReg = GM_OA_TTSCCL; }
            gm_op_done = false;
            DRV_LAN865X_ReadRegister(0u, nsecReg, false, gm_op_cb, NULL);
            gm_state = GM_STATE_WAIT_TTSCA_L;
            break;
        }

        /* ---- Store nanoseconds, issue W1C clear of OA_STATUS0 ---- */
        case GM_STATE_WAIT_TTSCA_L:
            if (!gm_op_done) break;
            gm_ts_nsec = gm_op_val;
            gm_state   = GM_STATE_WRITE_CLEAR;
            break;

        /* ---- Write back OA_STATUS0 to clear capture flags (W1C) ---- */
        case GM_STATE_WRITE_CLEAR:
            gm_op_done = false;
            DRV_LAN865X_WriteRegister(0u, GM_OA_STATUS0, gm_status0, false, gm_op_cb, NULL);
            gm_state = GM_STATE_WAIT_CLEAR;
            break;

        /* ---- Wait for write ACK, then build and send FollowUp ---- */
        case GM_STATE_WAIT_CLEAR:
            if (!gm_op_done) break;
            gm_state = GM_STATE_SEND_FOLLOWUP;
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
            (void)DRV_LAN865X_SendRawEthFrame(0u, gm_followup_buf,
                                               sizeof(gm_followup_buf),
                                               0x00u, NULL, NULL);
            gm_seq_id++;
            gm_sync_cnt++;
            gm_state = GM_STATE_WAIT_PERIOD;
            break;
        }

        default:
            gm_state = GM_STATE_WAIT_PERIOD;
            break;
    }
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
