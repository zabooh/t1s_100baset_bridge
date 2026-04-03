//DOM-IGNORE-BEGIN
/*
Copyright (C) 2025, Microchip Technology Inc., and its subsidiaries. All rights reserved.
Adapted from noIP-SAM-E54-Curiosity-PTP-Follower/ptp_task.c for use with the
Harmony TCP/IP stack (T1S 100BaseT Bridge project).

Key differences vs. the noIP version:
  - TC6_WriteRegister() replaced by DRV_LAN865X_WriteRegister() (fire-and-forget).
  - TC6_Service() blocking loops removed (the Harmony driver handles service internally).
  - get_macPhy_inst() / TC6_t* macPhy removed (use driver index 0 instead).
  - printf replaced with SYS_CONSOLE_PRINT.
  - ptpTask() renamed to PTP_FOL_Init().
  - PTP_FOL_OnFrame() added as the entry point from pktEth0Handler().
*/
//DOM-IGNORE-END

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "PTP_FOL_task.h"
#include "filters.h"
#include "config/default/driver/lan865x/drv_lan865x.h"
#include "config/default/system/console/sys_console.h"

#define PTP_LOG SYS_CONSOLE_PRINT

/* -------------------------------------------------------------------------
 * Globals (mirror of ptp_task.c)
 * ---------------------------------------------------------------------- */

ptpSync_ct      TS_SYNC;

static ptpMode_t ptpMode          = PTP_DISABLED;
static int32_t   ptp_sync_sequenceId = -1;
static uint8_t   syncReceived     = 0;
static bool      wallClockSet     = false;

volatile double rateRatio         = 1.0;
volatile double rateRatioIIR      = 1.0;
volatile double rateRatioFIR      = 1.0;
volatile double offsetIIR         = 0;
volatile double offsetFIR         = 0;

static uint8_t  ptpSynced         = 0;
static uint8_t  syncStatus        = UNINIT;
static uint32_t runs              = 0;
static uint64_t diffLocal         = 0;
static uint64_t diffRemote        = 0;

volatile int64_t  offset          = 0;
volatile uint64_t offset_abs      = 0;
volatile int      hardResync      = 0;

static double   rateRatioValue[FIR_FILER_SIZE]         = {0};
static lpfStateF rateRatiolpfState;

static int32_t  offsetValue[FIR_FILER_SIZE_FINE]       = {0};
static lpfState offsetState;

static int32_t  offsetCoarseValue[FIR_FILER_SIZE_FINE] = {0};
static lpfState offsetCoarseState;

long double continiousratio = 1.0;
static int32_t  diff        = 0;
static int32_t  filteredDiff= 0;
long double corrNs          = 0.0;
long double corrNsFlt       = 0.0;

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Full 64-bit byte swap using GCC builtin (XC32 / ARM Cortex-M4 supported) */
static uint64_t BSWAP64(uint64_t rawValue)
{
    uint32_t high = (uint32_t)((rawValue >> 32u) & 0xFFFFFFFFu);
    uint32_t low  = (uint32_t)((rawValue >>  0u) & 0xFFFFFFFFu);
    return (((uint64_t)__builtin_bswap32(low)) << 32u) |
            ((uint64_t)__builtin_bswap32(high));
}

static int64_t getCorrectionField(ptpHeader_t *hdr)
{
    return (int64_t)(BSWAP64((uint64_t)hdr->correctionField) >> 16);
}

uint64_t tsToInternal(const timeStamp_t *ts)
{
    uint64_t seconds = ((uint64_t)ts->secondsMsb << 32u) | ts->secondsLsb;
    return (seconds * SEC_IN_NS) + ts->nanoseconds;
}

/* -------------------------------------------------------------------------
 * Slave-node reset
 * ---------------------------------------------------------------------- */

static void resetSlaveNode(void)
{
    PTP_LOG("GM_RESET -> Slave node reset initiated due to sequence ID mismatch\r\n");
    ptp_sync_sequenceId = -1;
    syncReceived        = 0;
    wallClockSet        = false;
    ptpSynced           = 0;
    syncStatus          = UNINIT;
    runs                = 0;

    memset(&TS_SYNC, 0, sizeof(ptpSync_ct));

    for (uint32_t x = 0; x < FIR_FILER_SIZE_FINE; x++) {
        firLowPassFilter(0, &offsetCoarseState);
        firLowPassFilter(0, &offsetState);
    }
    for (uint32_t x = 0; x < FIR_FILER_SIZE; x++) {
        firLowPassFilterF(1.0, &rateRatiolpfState);
    }

    PTP_FOL_Init();
}

/* -------------------------------------------------------------------------
 * PTP message processors
 * ---------------------------------------------------------------------- */

static void processSync(syncMsg_t *ptpPkt)
{
    uint16_t seqId = htons(ptpPkt->header.sequenceID);

    if (ptp_sync_sequenceId < 0) {
        ptp_sync_sequenceId = seqId;
        syncReceived        = 0;
    } else {
        int sequenceDifference = abs((int)seqId - (int)ptp_sync_sequenceId);
        if (sequenceDifference > 10) {
            PTP_LOG("Large sequence mismatch: %u vs %d. Resetting...\r\n",
                    (unsigned int)seqId, (int)ptp_sync_sequenceId);
            resetSlaveNode();
        } else if ((int)ptp_sync_sequenceId == (int)seqId) {
            syncReceived = 1;
        } else {
            syncReceived = 0;
            PTP_LOG("Sync seqId mismatch. Is: %u - %d\r\n",
                    (unsigned int)seqId, (int)ptp_sync_sequenceId);
            ptp_sync_sequenceId = -1;
        }
    }
}

static void processFollowUp(followUpMsg_t *ptpPkt)
{
    uint16_t seqId = htons(ptpPkt->header.sequenceID);

    if (ptp_sync_sequenceId >= 0 && syncReceived) {
        if (ptp_sync_sequenceId == (int)seqId) {
            ptp_sync_sequenceId = (ptp_sync_sequenceId + 1) % (int)UINT16_MAX;
            syncReceived = 0;
        } else {
            PTP_LOG("FollowUp seqId mismatch. Is: %u - %d\r\n",
                    (unsigned int)seqId, (int)ptp_sync_sequenceId);
            ptp_sync_sequenceId = -1;
            memset(&TS_SYNC.receipt,      0, sizeof(ptpTimeStamp_t));
            memset(&TS_SYNC.receipt_prev, 0, sizeof(ptpTimeStamp_t));
            return;
        }
    } else {
        ptp_sync_sequenceId = ((int)seqId + 1) % (int)UINT16_MAX;
        PTP_LOG("FollowUp seqId out of sync. Is: %u - %d\r\n",
                (unsigned int)seqId, (int)ptp_sync_sequenceId);
        return;
    }

    /* Extract t1 from PTP frame */
    TS_SYNC.origin.secondsMsb  = htons(ptpPkt->preciseOriginTimestamp.secondsMsb);
    TS_SYNC.origin.secondsLsb  = htonl(ptpPkt->preciseOriginTimestamp.secondsLsb);
    TS_SYNC.origin.nanoseconds = htonl(ptpPkt->preciseOriginTimestamp.nanoseconds);
    TS_SYNC.origin.correctionField = (uint64_t)getCorrectionField(&ptpPkt->header);

    /* Hard sync: set the local wall clock directly to the GM timestamp */
    if (hardResync) {
        DRV_LAN865X_WriteRegister(0u, MAC_TSL, TS_SYNC.origin.secondsLsb, true, NULL, NULL);
        DRV_LAN865X_WriteRegister(0u, MAC_TN,  TS_SYNC.origin.nanoseconds, true, NULL, NULL);
        PTP_LOG("Large offset, doing hard sync\r\n");
        hardResync = 0;
    }

    /* Enable 1PPS output once the clock is synced */
    if (ptpSynced && !wallClockSet) {
        DRV_LAN865X_WriteRegister(0u, PPSCTL, 0x000007Du, true, NULL, NULL);
        wallClockSet = true;
    }

    /* Convert to internal ns representation */
    uint64_t t1 = tsToInternal(&TS_SYNC.origin);
    uint64_t t2 = tsToInternal(&TS_SYNC.receipt);

    if (TS_SYNC.receipt_prev.secondsLsb != 0u) {
        uint64_t curr = t2;
        uint64_t prev = tsToInternal(&TS_SYNC.receipt_prev);
        diffLocal = curr - prev;
    }

    if (TS_SYNC.origin_prev.secondsLsb != 0u) {
        uint64_t curr = t1;
        uint64_t prev = tsToInternal(&TS_SYNC.origin_prev);
        diffRemote = curr - prev;
    }

    TS_SYNC.receipt_prev = TS_SYNC.receipt;
    TS_SYNC.origin_prev  = TS_SYNC.origin;

    /* Rate-ratio estimation */
    if (diffLocal && diffRemote) {
        if (syncStatus == UNINIT || syncStatus > HARDSYNC) {
            rateRatio = (double)diffRemote / (double)diffLocal;
            if (rateRatio > 0.998 && rateRatio < 1.002) {
                rateRatioIIR = lowPassExponential((double)diffRemote / (double)diffLocal,
                                                  rateRatio, 0.5);
                rateRatioFIR = firLowPassFilterF((double)diffRemote / (double)diffLocal,
                                                 &rateRatiolpfState);
            } else {
                PTP_LOG("Filtered rateRatio outlier\r\n");
            }
        }
        runs++;
    } else {
        PTP_LOG("!");
        return;
    }

    offset     = (int64_t)t2 - (int64_t)t1;
    uint8_t neg = (offset < 0) ? 0u : 1u;
    offset_abs  = (uint64_t)llabs(offset);

    /* ---- Clock servo state machine ---- */
    if (syncStatus == UNINIT) {
        if (runs >= (FIR_FILER_SIZE * 1u)) {
            double calcInc = CLOCK_CYCLE_NS * rateRatioFIR;
            uint8_t mac_ti = (uint8_t)calcInc;
            double calcSubInc = calcInc - (double)mac_ti;
            calcSubInc *= 16777216.0;
            uint32_t calcSubInc_uint = (uint32_t)calcSubInc;
            calcSubInc_uint = ((calcSubInc_uint >> 8) & 0xFFFFu)
                            | ((calcSubInc_uint & 0xFFu) << 24);

            DRV_LAN865X_WriteRegister(0u, MAC_TISUBN, calcSubInc_uint, true, NULL, NULL);
            DRV_LAN865X_WriteRegister(0u, MAC_TI, (uint32_t)mac_ti, true, NULL, NULL);
            PTP_LOG("PTP UNINIT->MATCHFREQ  MAC_TI=%u TISUBN=0x%08X\r\n",
                    (unsigned int)mac_ti, (unsigned int)calcSubInc_uint);

            syncStatus = MATCHFREQ;
            ptpSynced  = 1;
            runs       = 0;
        }

    } else if (syncStatus == MATCHFREQ) {
        if (offset_abs > MATCHFREQ_RESET_THRESHOLD) {
            hardResync = 1;
        } else {
            syncStatus = HARDSYNC;
        }

    } else if (syncStatus >= HARDSYNC) {
        if (offset_abs > HARDSYNC_RESET_THRESHOLD) {
            syncStatus = UNINIT;
            for (uint32_t x = 0; x < FIR_FILER_SIZE_FINE; x++) {
                (void)firLowPassFilter(0, &offsetCoarseState);
                (void)firLowPassFilter(0, &offsetState);
            }
            for (uint32_t x = 0; x < FIR_FILER_SIZE; x++) {
                (void)firLowPassFilterF(1.0, &rateRatiolpfState);
            }
            runs = 0;

        } else if (offset_abs > HARDSYNC_THRESHOLD) {
            offset_abs = HARDSYNC_THRESHOLD;
            DRV_LAN865X_WriteRegister(0u, MAC_TA,
                ((neg & 1u) << 31) | (uint32_t)offset_abs, true, NULL, NULL);
            syncStatus = HARDSYNC;

        } else if (offset_abs > HARDSYNC_COARSE_THRESHOLD) {
            for (uint32_t x = 0; x < FIR_FILER_SIZE_FINE; x++) {
                (void)firLowPassFilter(0, &offsetCoarseState);
                (void)firLowPassFilter(0, &offsetState);
                offsetCoarseState.filled = 0;
                offsetState.filled       = 0;
            }
            DRV_LAN865X_WriteRegister(0u, MAC_TA,
                ((neg & 1u) << 31) | (uint32_t)offset_abs, true, NULL, NULL);
            syncStatus = HARDSYNC;

        } else if (offset_abs > HARDSYNC_FINE_THRESHOLD) {
            for (uint32_t x = 0; x < FIR_FILER_SIZE_FINE; x++) {
                (void)firLowPassFilter(0, &offsetState);
                offsetState.filled = 0;
            }
            offsetFIR = firLowPassFilter((int32_t)offset, &offsetCoarseState);
            neg = (offsetFIR < 0) ? 0u : 1u;
            int32_t write_val = (int32_t)offsetFIR;
            if (!neg) write_val = write_val * (-1);

            DRV_LAN865X_WriteRegister(0u, MAC_TA,
                ((neg & 1u) << 31) | (uint32_t)write_val, true, NULL, NULL);
            syncStatus = COARSE;
            PTP_LOG("PTP COARSE  offset=%d val=%d\r\n",
                    (int)offset, (int)write_val);

        } else {
            offsetFIR = firLowPassFilter((int32_t)offset, &offsetState);
            neg = (offsetFIR < 0) ? 0u : 1u;
            int32_t write_val = (int32_t)offsetFIR;
            if (!neg) write_val = write_val * (-1);

            DRV_LAN865X_WriteRegister(0u, MAC_TA,
                ((neg & 1u) << 31) | (uint32_t)write_val, true, NULL, NULL);
            syncStatus = FINE;
            PTP_LOG("PTP FINE    offset=%d val=%d\r\n",
                    (int)offset, (int)write_val);
        }
    }
}

/* -------------------------------------------------------------------------
 * PTP frame dispatcher  (called internally & exposed via header)
 * ---------------------------------------------------------------------- */

void handlePtp(uint8_t *pData, uint32_t size, uint32_t sec, uint32_t nsec)
{
    (void)size;
    ptpHeader_t *ptpPkt = (ptpHeader_t *)(pData + sizeof(ethHeader_t));
    uint8_t messageType = ptpPkt->tsmt & 0x0Fu;

    if (messageType == (uint8_t)MSG_FOLLOW_UP) {
        processFollowUp((followUpMsg_t *)ptpPkt);
    } else if (messageType == (uint8_t)MSG_SYNC) {
        processSync((syncMsg_t *)ptpPkt);
        if (syncReceived) {
            TS_SYNC.receipt_prev        = TS_SYNC.receipt;
            TS_SYNC.receipt.secondsLsb  = sec;
            TS_SYNC.receipt.nanoseconds = nsec;
        }
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void PTP_FOL_Init(void)
{
    /* Set up PPS output (stopped; PPSCTL=0x02 = pulse-width / period preset) */
    DRV_LAN865X_WriteRegister(0u, PPSCTL,   0x00000002u,       true, NULL, NULL);
    DRV_LAN865X_WriteRegister(0u, SEVINTEN, SEVINTEN_PPSDONE_Msk, true, NULL, NULL);

    memset(&TS_SYNC, 0, sizeof(TS_SYNC));

    rateRatiolpfState.buffer     = &rateRatioValue[0];
    rateRatiolpfState.filterSize = sizeof(rateRatioValue) / sizeof(rateRatioValue[0]);

    offsetState.buffer           = &offsetValue[0];
    offsetState.filterSize       = sizeof(offsetValue) / sizeof(offsetValue[0]);

    offsetCoarseState.buffer     = &offsetCoarseValue[0];
    offsetCoarseState.filterSize = sizeof(offsetCoarseValue) / sizeof(offsetCoarseValue[0]);

    PTP_LOG("PTP_FOL_Init: HW init done, PTP mode=%d (not activated)\r\n", (int)ptpMode);
}

ptpMode_t PTP_FOL_GetMode(void)
{
    return ptpMode;
}

void PTP_FOL_SetMode(ptpMode_t mode)
{
    ptpMode = mode;
    if (mode == PTP_SLAVE) {
        resetSlaveNode();
    }
}

void PTP_FOL_GetOffset(int64_t *pOffset, uint64_t *pOffsetAbs)
{
    if (pOffset)    *pOffset    = offset;
    if (pOffsetAbs) *pOffsetAbs = offset_abs;
}

void PTP_FOL_Reset(void)
{
    resetSlaveNode();
}

void PTP_FOL_OnFrame(const uint8_t *pData, uint16_t len, uint64_t rxTimestamp)
{
    if (ptpMode != PTP_SLAVE) {
        return;
    }
    uint32_t sec  = (uint32_t)((rxTimestamp >> 32u) & 0xFFFFFFFFu);
    uint32_t nsec = (uint32_t)( rxTimestamp         & 0xFFFFFFFFu);
    handlePtp((uint8_t *)pData, (uint32_t)len, sec, nsec);
}
