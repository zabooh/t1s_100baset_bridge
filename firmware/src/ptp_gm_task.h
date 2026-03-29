//DOM-IGNORE-BEGIN
/*
Copyright (C) 2025, Microchip Technology Inc., and its subsidiaries. All rights reserved.
PTP Grandmaster task — header for T1S 100BaseT Bridge (ATSAME54P20A / LAN865x).
Ported from noIP-SAM-E54-Curiosity-PTP-Grandmaster/firmware/src/ptp.h.
*/
//DOM-IGNORE-END

#ifndef PTP_GM_TASK_H
#define PTP_GM_TASK_H

#include <stdint.h>
#include <stdbool.h>

/* ---- TX Match registers (GM only) ---- */
#define GM_TXMCTL       (0x00040040u)
#define GM_TXMPATH      (0x00040041u)
#define GM_TXMPATL      (0x00040042u)
#define GM_TXMMSKH      (0x00040043u)
#define GM_TXMMSKL      (0x00040044u)
#define GM_TXMLOC       (0x00040045u)

/* ---- OA registers used by GM ---- */
#define GM_OA_CONFIG0   (0x00000004u)
#define GM_OA_STATUS0   (0x00000008u)

/* ---- TX Timestamp Capture A/B/C ---- */
#define GM_OA_TTSCAH    (0x00000010u)   /* Capture A: seconds   */
#define GM_OA_TTSCAL    (0x00000011u)   /* Capture A: nanosecs  */
#define GM_OA_TTSCBH    (0x00000012u)   /* Capture B: seconds   */
#define GM_OA_TTSCBL    (0x00000013u)   /* Capture B: nanosecs  */
#define GM_OA_TTSCCH    (0x00000014u)   /* Capture C: seconds   */
#define GM_OA_TTSCCL    (0x00000015u)   /* Capture C: nanosecs  */

/* ---- Status / control bit masks ---- */
#define GM_TXMCTL_TXPMDET   (0x0080u)   /* TX Packet Match Detected */
#define GM_STS0_TTSCAA      (0x0100u)   /* Timestamp Capture Available A */
#define GM_STS0_TTSCAB      (0x0200u)   /* Timestamp Capture Available B */
#define GM_STS0_TTSCAC      (0x0400u)   /* Timestamp Capture Available C */

/* ---- Timing constants ---- */
#define PTP_GM_SYNC_PERIOD_MS   125u
#define PTP_GM_STATIC_OFFSET    7650u
#define PTP_GM_MAX_TN_VAL       0x3B9ACA00u   /* 1 000 000 000 ns */
#define PTP_GM_MAX_RETRIES      5u

/* ---- Public API ---- */

/** Initialise hardware (TX Match, MAC time increment, PADCTRL) and reset state.
 *  Must be called after the TCPIP stack is up and the LAN865x driver is ready.
 */
void PTP_GM_Init(void);

/** Service the GM state machine.  Must be called every 1 ms (from a timer callback). */
void PTP_GM_Service(void);

/** Read-only status query. */
void PTP_GM_GetStatus(uint32_t *pSyncCount, uint32_t *pState);

/** Change the Sync burst interval (default 125 ms).  Valid range: 10 – 10000 ms. */
void PTP_GM_SetSyncInterval(uint32_t intervalMs);

#endif /* PTP_GM_TASK_H */
