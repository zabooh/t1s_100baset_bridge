/*******************************************************************************
  PTP follower on the 10BASE-T1S segment: receive and measure

  File Name:
    ptp_follower.c

  Summary:
    Implementation of the receive-and-measure stage described in ptp_follower.h.

  Description:
    Two contexts, deliberately kept apart:

    Driver context (ptp_follower_rx_hook) does the minimum that can only be done
    there: filter on EtherType, remember a Sync's arrival timestamp under its
    sequenceId, and when the matching Follow_Up turns up, push the finished pair
    into a ring buffer. No printing, no register access, no arithmetic beyond
    byte extraction.

    Task context (PTP_FOL_Tasks) drains that ring and does the statistics. The
    pending-Sync table is four deep, which is enough: a Follow_Up follows its Sync
    within a fraction of a millisecond (0.1-0.3 ms measured on the bridge, the SPI
    read-back of the egress timestamp), while the interval between cycles is tens
    to hundreds of milliseconds.
 *******************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>                                          /* strtoul() */
#include <string.h>

#include "definitions.h"
#include "config/default/system/console/sys_console.h"
#include "config/default/library/tcpip/tcpip.h"
#include "config/default/system/time/sys_time.h"
#include "config/default/driver/lan865x/drv_lan865x.h"
#include "system/command/sys_command.h"
#include "ptp_follower.h"

#define PTP_FOL_IF              0u          /* eth0, the 10BASE-T1S MAC-PHY */

/* --- registers, MMS in the upper 16 bits ------------------------------------ */
#define OA_CONFIG0              0x00000004u
#define OA_CONFIG0_FTS_MASK     0x000000C0u  /* FTSE (bit 7) + FTSS (bit 6): both, always */

/* --- frame layout (see ptp_gm.c on the bridge for the sending side) --------- */
#define ETH_HDR_LEN             14u
#define PTP_HDR_LEN             34u
#define PTP_TS_LEN              10u
#define PTP_MIN_LEN             (ETH_HDR_LEN + PTP_HDR_LEN + PTP_TS_LEN)   /* 58 */
#define PTP_MSGTYPE_SYNC        0x0u
#define PTP_MSGTYPE_FOLLOW_UP   0x8u
#define TS_NS_MASK              0x3FFFFFFFu

#define PENDING_SLOTS           4u
#define SAMPLE_RING             8u
#define REG_TIMEOUT_MS          200u

typedef struct {
    bool     used;
    uint16_t seq;
    uint64_t t2;                  /* arrival of the Sync, master-independent */
} pending_t;

typedef struct {
    uint16_t seq;
    uint64_t t1;                  /* master egress, from Follow_Up   */
    uint64_t t2;                  /* our arrival, from the hardware  */
} sample_t;

typedef enum {
    ST_OFF = 0,
    ST_TS_ENABLE,
    ST_RUN,
    ST_TS_DISABLE
} fol_state_t;

static fol_state_t s_state = ST_OFF;
static bool s_run_requested = false;
static bool s_verbose = false;

/* driver context <-> task context */
static volatile pending_t s_pending[PENDING_SLOTS];
static volatile sample_t  s_ring[SAMPLE_RING];
static volatile uint32_t  s_ring_head = 0u;   /* written by the hook  */
static volatile uint32_t  s_ring_tail = 0u;   /* read by the task     */

/* counters, all reported by "ptpf status" */
static volatile uint32_t s_cnt_sync = 0u;
static volatile uint32_t s_cnt_sync_nots = 0u;   /* Sync without a timestamp */
static volatile uint32_t s_cnt_fup = 0u;
static volatile uint32_t s_cnt_unmatched = 0u;   /* Follow_Up with no pending Sync */
static volatile uint32_t s_cnt_overflow = 0u;    /* task did not keep up */

/* statistics, task context only */
static uint32_t s_samples = 0u;
static int64_t  s_offset_last = 0;
static int64_t  s_offset_prev = 0;
static int64_t  s_offset_delta = 0;              /* change per cycle = frequency error */
static int64_t  s_offset_min = 0;
static int64_t  s_offset_max = 0;
static uint64_t s_t1_last = 0u;
static uint64_t s_t2_last = 0u;
static uint64_t s_t1_prev = 0u;
static uint32_t s_seq_last = 0u;

/* register operation in flight */
static volatile bool     s_reg_done = false;
static volatile bool     s_reg_ok = false;
static bool     s_reg_busy = false;
static uint64_t s_reg_deadline = 0u;
static uint64_t s_ticks_per_ms = 0u;

/* --------------------------------------------------------------------------- */
/* helpers                                                                     */
/* --------------------------------------------------------------------------- */

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8u) | (uint16_t)p[1]);
}

/* PTP timestamp on the wire: 48-bit seconds, 32-bit nanoseconds, big endian.
   Returned as a plain nanosecond count - 32-bit seconds times 1e9 stays far
   inside int64. */
static uint64_t ts_from_wire(const uint8_t *p)
{
    uint64_t sec = ((uint64_t)p[0] << 40u) | ((uint64_t)p[1] << 32u)
                 | ((uint64_t)p[2] << 24u) | ((uint64_t)p[3] << 16u)
                 | ((uint64_t)p[4] <<  8u) |  (uint64_t)p[5];
    uint64_t ns  = ((uint64_t)p[6] << 24u) | ((uint64_t)p[7] << 16u)
                 | ((uint64_t)p[8] <<  8u) |  (uint64_t)p[9];
    return sec * 1000000000ULL + ns;
}

/* Hardware timestamp: 64-bit, seconds in the upper 32 bits, nanoseconds in bits
   29:0 of the lower half (datasheet figure 5-4). */
static uint64_t ts_from_hw(uint64_t raw)
{
    uint64_t sec = raw >> 32u;
    uint64_t ns  = (uint64_t)((uint32_t)raw & TS_NS_MASK);
    return sec * 1000000000ULL + ns;
}

/* --------------------------------------------------------------------------- */
/* driver context: the receive hook                                            */
/* --------------------------------------------------------------------------- */

void ptp_follower_rx_hook(const uint8_t *frame, uint16_t len, const uint64_t *rxTimestamp)
{
    uint8_t msgtype;
    uint16_t seq;
    uint32_t i;

    if (!s_run_requested || frame == NULL || len < PTP_MIN_LEN) {
        return;
    }
    if (be16(&frame[12]) != (uint16_t)PTP_FOL_ETHERTYPE) {
        return;
    }

    msgtype = (uint8_t)(frame[ETH_HDR_LEN] & 0x0Fu);
    seq = be16(&frame[ETH_HDR_LEN + 30u]);

    if (msgtype == PTP_MSGTYPE_SYNC) {
        s_cnt_sync++;
        if (rxTimestamp == NULL) {
            /* Frame timestamping off, or the driver patch is gone after an MCC
               regeneration. Counted separately because it is the one failure that
               otherwise looks like "no PTP traffic". */
            s_cnt_sync_nots++;
            return;
        }
        /* Remember this Sync until its Follow_Up shows up. Oldest slot wins if
           all are taken - a stale pending Sync is worthless anyway. */
        for (i = 0u; i < PENDING_SLOTS; i++) {
            if (!s_pending[i].used) {
                break;
            }
        }
        if (i == PENDING_SLOTS) {
            i = 0u;
        }
        s_pending[i].seq = seq;
        s_pending[i].t2 = ts_from_hw(*rxTimestamp);
        s_pending[i].used = true;
        return;
    }

    if (msgtype == PTP_MSGTYPE_FOLLOW_UP) {
        s_cnt_fup++;
        for (i = 0u; i < PENDING_SLOTS; i++) {
            if (s_pending[i].used && s_pending[i].seq == seq) {
                uint32_t head = s_ring_head;
                uint32_t next = (head + 1u) % SAMPLE_RING;
                if (next == s_ring_tail) {
                    s_cnt_overflow++;
                } else {
                    s_ring[head].seq = seq;
                    s_ring[head].t1 = ts_from_wire(&frame[ETH_HDR_LEN + PTP_HDR_LEN]);
                    s_ring[head].t2 = s_pending[i].t2;
                    s_ring_head = next;
                }
                s_pending[i].used = false;
                return;
            }
        }
        s_cnt_unmatched++;
    }
}

/* --------------------------------------------------------------------------- */
/* register access                                                             */
/* --------------------------------------------------------------------------- */

static void fol_reg_cb(void *r1, bool success, uint32_t addr, uint32_t value, void *tag, void *r2)
{
    (void)r1; (void)addr; (void)value; (void)tag; (void)r2;
    s_reg_ok = success;
    s_reg_done = true;
}

static bool fol_reg_rmw(uint32_t addr, uint32_t value, uint32_t mask)
{
    s_reg_done = false;
    s_reg_ok = false;
    if (DRV_LAN865X_ReadModifyWriteRegister(PTP_FOL_IF, addr, value, mask, true,
                                            fol_reg_cb, NULL) != TCPIP_MAC_RES_OK) {
        return false;
    }
    s_reg_busy = true;
    s_reg_deadline = SYS_TIME_Counter64Get() + (uint64_t)REG_TIMEOUT_MS * s_ticks_per_ms;
    return true;
}

/* --------------------------------------------------------------------------- */
/* public API                                                                  */
/* --------------------------------------------------------------------------- */

bool PTP_FOL_IsRunning(void) { return s_run_requested; }

bool PTP_FOL_Start(void)
{
    uint32_t i;
    if (s_run_requested) {
        return true;
    }
    for (i = 0u; i < PENDING_SLOTS; i++) {
        s_pending[i].used = false;
    }
    s_ring_head = s_ring_tail = 0u;
    s_run_requested = true;
    s_state = ST_TS_ENABLE;
    s_reg_busy = false;
    return true;
}

void PTP_FOL_Stop(void)
{
    if (!s_run_requested) {
        return;
    }
    s_run_requested = false;
    s_state = ST_TS_DISABLE;
    s_reg_busy = false;
}

/* --------------------------------------------------------------------------- */
/* task context                                                                */
/* --------------------------------------------------------------------------- */

static void fol_consume(const sample_t *s)
{
    int64_t offset = (int64_t)s->t2 - (int64_t)s->t1 - (int64_t)PTP_FOL_D_CONST_NS;

    s_offset_prev = s_offset_last;
    s_offset_last = offset;
    s_t1_prev = s_t1_last;
    s_t1_last = s->t1;
    s_t2_last = s->t2;
    s_seq_last = s->seq;

    if (s_samples == 0u) {
        s_offset_min = s_offset_max = offset;
        s_offset_delta = 0;
    } else {
        if (offset < s_offset_min) { s_offset_min = offset; }
        if (offset > s_offset_max) { s_offset_max = offset; }
        s_offset_delta = offset - s_offset_prev;
    }
    s_samples++;

    if (s_verbose) {
        /* One line per cycle, for bring-up. The delta is the interesting column:
           it is the frequency error between the two crystals, per cycle. */
        SYS_CONSOLE_PRINT("[PTPF] seq=%u  offset=%lld ns  delta=%lld ns  t1=%llu  t2=%llu\r\n",
                          (unsigned)s->seq, (long long)offset, (long long)s_offset_delta,
                          (unsigned long long)s->t1, (unsigned long long)s->t2);
    }
}

void PTP_FOL_Tasks(void)
{
    if (s_ticks_per_ms == 0u) {
        s_ticks_per_ms = (uint64_t)SYS_TIME_FrequencyGet() / 1000ULL;
        if (s_ticks_per_ms == 0u) {
            return;
        }
    }

    if (s_reg_busy && !s_reg_done
        && ((int64_t)(SYS_TIME_Counter64Get() - s_reg_deadline) >= 0)) {
        s_reg_busy = false;
        SYS_CONSOLE_PRINT("[PTPF] register operation timed out\r\n");
        s_state = (s_state == ST_TS_ENABLE) ? ST_OFF : ST_OFF;
        s_run_requested = false;
        return;
    }

    switch (s_state) {
        case ST_OFF:
            break;

        case ST_TS_ENABLE:
            if (!s_reg_busy) {
                (void)fol_reg_rmw(OA_CONFIG0, OA_CONFIG0_FTS_MASK, OA_CONFIG0_FTS_MASK);
            } else if (s_reg_done) {
                s_reg_busy = false;
                if (!s_reg_ok) {
                    SYS_CONSOLE_PRINT("[PTPF] enabling frame timestamps failed\r\n");
                    s_run_requested = false;
                    s_state = ST_OFF;
                } else {
                    s_state = ST_RUN;
                }
            }
            break;

        case ST_RUN:
            /* Drain whatever the hook has paired. Two per call keeps the loop
               responsive; at 4 to 20 cycles per second there is never a backlog. */
            {
                uint32_t budget = 2u;
                while (budget-- > 0u && s_ring_tail != s_ring_head) {
                    sample_t s = *(const sample_t *)&s_ring[s_ring_tail];
                    s_ring_tail = (s_ring_tail + 1u) % SAMPLE_RING;
                    fol_consume(&s);
                }
            }
            break;

        case ST_TS_DISABLE:
            if (!s_reg_busy) {
                (void)fol_reg_rmw(OA_CONFIG0, 0x00000000u, OA_CONFIG0_FTS_MASK);
            } else if (s_reg_done) {
                s_reg_busy = false;
                s_state = ST_OFF;
            }
            break;

        default:
            s_state = ST_OFF;
            break;
    }
}

/* --------------------------------------------------------------------------- */
/* console                                                                     */
/* --------------------------------------------------------------------------- */

static void fol_print_status(void)
{
    SYS_CONSOLE_PRINT("[PTPF] listening: %s   samples: %u   last seq: %u\r\n",
                      s_run_requested ? "on" : "off",
                      (unsigned)s_samples, (unsigned)s_seq_last);
    SYS_CONSOLE_PRINT("[PTPF] rx sync: %u   follow_up: %u   sync without timestamp: %u\r\n",
                      (unsigned)s_cnt_sync, (unsigned)s_cnt_fup, (unsigned)s_cnt_sync_nots);
    SYS_CONSOLE_PRINT("[PTPF] unmatched follow_up: %u   ring overflows: %u\r\n",
                      (unsigned)s_cnt_unmatched, (unsigned)s_cnt_overflow);
    if (s_samples == 0u) {
        SYS_CONSOLE_PRINT("[PTPF] no sample yet\r\n");
        return;
    }
    SYS_CONSOLE_PRINT("[PTPF] offset: %lld ns   change per cycle: %lld ns\r\n",
                      (long long)s_offset_last, (long long)s_offset_delta);
    SYS_CONSOLE_PRINT("[PTPF] offset min/max: %lld / %lld ns   span: %lld ns\r\n",
                      (long long)s_offset_min, (long long)s_offset_max,
                      (long long)(s_offset_max - s_offset_min));
    SYS_CONSOLE_PRINT("[PTPF] t1 (master): %llu ns   t2 (ours): %llu ns\r\n",
                      (unsigned long long)s_t1_last, (unsigned long long)s_t2_last);
    if (s_t1_prev != 0u) {
        SYS_CONSOLE_PRINT("[PTPF] master cycle: %llu ns   (D_const assumed: %d ns)\r\n",
                          (unsigned long long)(s_t1_last - s_t1_prev), PTP_FOL_D_CONST_NS);
    }
    SYS_CONSOLE_PRINT("[PTPF] the clock is NOT being adjusted - measuring only\r\n");
}

/* ptpf on | off | status | log [0|1] | reset */
static void cmd_ptpf(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv)
{
    (void)pCmdIO;

    if (argc < 2) {
        fol_print_status();
        SYS_CONSOLE_PRINT("usage: ptpf on | off | status | log [on|off] | reset\r\n");
        return;
    }
    if (!strcmp(argv[1], "on")) {
        if (s_run_requested) {
            SYS_CONSOLE_PRINT("[PTPF] already listening\r\n");
        } else if (PTP_FOL_Start()) {
            SYS_CONSOLE_PRINT("[PTPF] listening (FTSE+FTSS on)\r\n");
        }
        return;
    }
    if (!strcmp(argv[1], "off")) {
        PTP_FOL_Stop();
        SYS_CONSOLE_PRINT("[PTPF] stopped\r\n");
        return;
    }
    if (!strcmp(argv[1], "status")) {
        fol_print_status();
        return;
    }
    if (!strcmp(argv[1], "log")) {
        if (argc >= 3) {
            s_verbose = (!strcmp(argv[2], "on") || !strcmp(argv[2], "1"));
        }
        SYS_CONSOLE_PRINT("[PTPF] per-cycle log: %s\r\n", s_verbose ? "on" : "off");
        return;
    }
    if (!strcmp(argv[1], "reset")) {
        s_samples = 0u;
        s_cnt_sync = s_cnt_sync_nots = s_cnt_fup = 0u;
        s_cnt_unmatched = s_cnt_overflow = 0u;
        s_t1_prev = s_t1_last = s_t2_last = 0u;
        SYS_CONSOLE_PRINT("[PTPF] counters cleared\r\n");
        return;
    }
    SYS_CONSOLE_PRINT("usage: ptpf on | off | status | log [on|off] | reset\r\n");
}

static const SYS_CMD_DESCRIPTOR ptpf_cmd_tbl[] = {
    {"ptpf", (SYS_CMD_FNC)cmd_ptpf, ": PTP follower (ptpf on | off | status | log [on|off] | reset)"},
};

void PTP_FOL_Initialize(void)
{
    uint32_t i;
    for (i = 0u; i < PENDING_SLOTS; i++) {
        s_pending[i].used = false;
    }
    if (!SYS_CMD_ADDGRP(ptpf_cmd_tbl, (int)(sizeof ptpf_cmd_tbl / sizeof *ptpf_cmd_tbl),
                        "ptpf", ": PTP follower on the T1S segment")) {
        SYS_CONSOLE_PRINT("PTPF: SYS_CMD_ADDGRP failed\r\n");
    }
}
