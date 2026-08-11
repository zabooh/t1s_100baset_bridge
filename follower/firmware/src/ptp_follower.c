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
#define MAC_TISUBN              0x0001006Fu  /* 24-bit sub-ns increment per clock cycle  */
#define MAC_TSL                 0x00010074u  /* seconds [31:0]                           */
#define MAC_TN                  0x00010075u  /* nanoseconds [29:0]                       */
#define MAC_TA                  0x00010076u  /* one-shot adjust: bit31 = subtract, 29:0 ns */
#define MAC_TI                  0x00010077u  /* whole ns per clock cycle, nominal 0x28   */

#define MAC_TA_SUBTRACT         0x80000000u
#define MAC_TI_NOMINAL          40u          /* 25 MHz -> 40 ns per cycle                */

/* The increment is a fixed-point number: MAC_TI whole nanoseconds plus a 24-bit
   fraction in MAC_TISUBN. Working in units of 2^-24 ns keeps both in one integer,
   and one LSB is 1/(40 * 2^24) = 1.5 ppb - far finer than the servo needs. */
#define INC_FRAC_BITS           24
#define INC_NOMINAL             ((uint64_t)MAC_TI_NOMINAL << INC_FRAC_BITS)

/* --- servo thresholds, from LAN8651_TIME_SYNC.md section 7 ------------------- */
#define SERVO_WARMUP_SAMPLES    16           /* estimate the rate before touching anything */
#define SERVO_RESET_NS          1070000000LL /* beyond this, start over                  */
#define SERVO_HARDSYNC_NS       100000000LL  /* below this, step with MAC_TA             */
#define SERVO_COARSE_NS         300LL        /* below this: coarse                        */
#define SERVO_FINE_NS           150LL        /* at or below this: fine                    */
#define SERVO_STEP_CAP_NS       16000000LL   /* largest single MAC_TA step                */
#define SERVO_RATE_OUTLIER_PPB  5000000LL    /* +/-5000 ppm: reject the sample            */
#define SERVO_RATE_CLAMP_PPB    200000LL     /* +/-200 ppm of total correction            */
#define SERVO_RATE_EVERY        32u          /* rate update cadence, in samples           */
/* The cadence has to relate to the filter: with N = 128 the estimate needs about
   that many samples to reflect a change, and updating every 8 samples with a
   quarter gain still hunted - the correction swung between 5115 and 1897 ppb for
   an error that was really about 5100. Updating every 32 samples keeps the loop
   slower than the filter that feeds it. */
#define SERVO_RATE_GAIN_SHIFT   2            /* apply a quarter of the residual per update */
#define SERVO_PHASE_GAIN_SHIFT  1            /* step by half the filtered offset           */
#define SERVO_PHASE_DEADBAND_NS 40LL         /* one clock tick: below this, do not dither  */
#define SERVO_IIR_SHIFT         7            /* N = 128, about 11 s at 8 samples/s        */

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
    uint64_t t1;                  /* master egress, from Follow_Up          */
    uint64_t t2;                  /* our arrival, from the hardware         */
    uint64_t host;                /* SYS_TIME tick when the pair completed  */
} sample_t;

typedef enum {
    ST_OFF = 0,
    ST_TS_ENABLE,
    ST_RUN,
    ST_REGQ,                  /* working through a queued burst of register writes */
    ST_TS_DISABLE
} fol_state_t;

/* Servo states, five of them. The split of "unlocked" into a frequency-matching
   step and a hard-set step is the part that is easy to get wrong: correcting the
   rate first means the hard set does not immediately walk away again. */
typedef enum {
    SV_UNINIT = 0,   /* nominal increment written, collecting samples          */
    SV_MATCHFREQ,    /* first rate correction applied, about to set the clock  */
    SV_HARDSYNC,     /* large one-shot MAC_TA steps                            */
    SV_COARSE,       /* |offset| < 300 ns, filtered steps                      */
    SV_FINE          /* |offset| <= 150 ns                                     */
} servo_state_t;

static const char *servo_name(servo_state_t s)
{
    switch (s) {
        case SV_UNINIT:    return "UNINIT";
        case SV_MATCHFREQ: return "MATCHFREQ";
        case SV_HARDSYNC:  return "HARDSYNC";
        case SV_COARSE:    return "COARSE";
        case SV_FINE:      return "FINE";
        default:           return "?";
    }
}

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

/* A short burst of writes (set the clock, change the increment) is queued and
   worked through one at a time, so the servo logic stays free of a state per
   register. */
#define REGQ_MAX 4
static uint32_t s_q_addr[REGQ_MAX];
static uint32_t s_q_val[REGQ_MAX];
static uint8_t  s_q_len = 0u;
static uint8_t  s_q_pos = 0u;
static fol_state_t s_q_return = ST_RUN;

/* servo state, task context only */
static bool          s_servo_on = true;      /* "ptpf servo off" measures only  */
static servo_state_t s_servo = SV_UNINIT;
static uint32_t s_servo_entered = 0u;        /* sample count when the state began */
static int64_t  s_rate_acc = 0;              /* IIR accumulator, ppb << SHIFT     */
static int64_t  s_rate_est_ppb = 0;          /* filtered residual rate error      */
static int64_t  s_rate_corr_ppb = 0;         /* total correction currently applied */
static uint64_t s_inc_units = INC_NOMINAL;   /* what was last written             */
static uint32_t s_cnt_rate_outlier = 0u;
static uint32_t s_cnt_steps = 0u;            /* MAC_TA writes                     */
static uint32_t s_cnt_rate_writes = 0u;
static bool     s_skip_rate_once = false;    /* after a hard set                  */
static int64_t  s_step_applied_ns = 0;       /* signed sum of MAC_TA since the last sample */
static bool     s_rate_primed = false;       /* IIR seeded with the first estimate */
static uint32_t s_rate_next_at = 0u;         /* sample count of the next rate update */
static int64_t  s_fir[3] = {0, 0, 0};        /* FIR3 over the offset              */
static uint32_t s_fir_n = 0u;

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
                    s_ring[head].host = SYS_TIME_Counter64Get();
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

static bool fol_reg_write(uint32_t addr, uint32_t value)
{
    s_reg_done = false;
    s_reg_ok = false;
    if (DRV_LAN865X_WriteRegister(PTP_FOL_IF, addr, value, true,
                                  fol_reg_cb, NULL) != TCPIP_MAC_RES_OK) {
        return false;
    }
    s_reg_busy = true;
    s_reg_deadline = SYS_TIME_Counter64Get() + (uint64_t)REG_TIMEOUT_MS * s_ticks_per_ms;
    return true;
}

/* Queue a burst of writes; the task drains it. Returns false when a burst is
   still in flight, in which case the caller simply tries again next sample. */
static bool fol_queue(const uint32_t *addr, const uint32_t *val, uint8_t n)
{
    uint8_t i;
    if (s_q_len != 0u || n > REGQ_MAX) {
        return false;
    }
    for (i = 0u; i < n; i++) {
        s_q_addr[i] = addr[i];
        s_q_val[i] = val[i];
    }
    s_q_len = n;
    s_q_pos = 0u;
    s_q_return = ST_RUN;
    return true;
}

/* --------------------------------------------------------------------------- */
/* servo                                                                       */
/* --------------------------------------------------------------------------- */

/* Split the fixed-point increment into the two registers. The 24-bit fraction is
   stored as MSBTIR[15:0] in bits 15:0 and LSBTIR[7:0] in bits 31:24 - not one
   contiguous field, which is easy to get wrong and would show up as a rate error
   of a few hundred ppm. */
static uint32_t inc_to_tisubn(uint64_t units)
{
    uint32_t frac = (uint32_t)(units & 0x00FFFFFFu);
    return ((frac & 0xFFu) << 24u) | ((frac >> 8u) & 0xFFFFu);
}

static uint32_t inc_to_ti(uint64_t units)
{
    return (uint32_t)(units >> INC_FRAC_BITS);
}

/* Write the increment for a total correction of `ppb` relative to nominal. */
static bool servo_write_rate(int64_t ppb)
{
    uint32_t addr[2];
    uint32_t val[2];
    int64_t units;

    if (ppb > SERVO_RATE_CLAMP_PPB)  { ppb = SERVO_RATE_CLAMP_PPB; }
    if (ppb < -SERVO_RATE_CLAMP_PPB) { ppb = -SERVO_RATE_CLAMP_PPB; }

    /* units = nominal * (1 + ppb/1e9), computed so the rounding stays in integers */
    units = (int64_t)INC_NOMINAL + (((int64_t)INC_NOMINAL * ppb) / 1000000000LL);
    if (units < 1) {
        units = 1;
    }

    /* Fraction first, whole nanoseconds second: MAC_TI is what the hardware reads
       per cycle, so writing it last makes the pair take effect together. */
    addr[0] = MAC_TISUBN; val[0] = inc_to_tisubn((uint64_t)units);
    addr[1] = MAC_TI;     val[1] = inc_to_ti((uint64_t)units);
    if (!fol_queue(addr, val, 2u)) {
        return false;
    }
    s_inc_units = (uint64_t)units;
    s_rate_corr_ppb = ppb;
    s_cnt_rate_writes++;
    return true;
}

/* One-shot offset correction. Positive offset means our clock is ahead, so the
   adjustment subtracts. */
static bool servo_step(int64_t offset_ns)
{
    uint32_t addr[1];
    uint32_t val[1];
    int64_t mag = (offset_ns < 0) ? -offset_ns : offset_ns;

    if (mag > SERVO_STEP_CAP_NS) {
        mag = SERVO_STEP_CAP_NS;
    }
    if (mag == 0) {
        return true;
    }
    addr[0] = MAC_TA;
    val[0] = (uint32_t)(mag & 0x3FFFFFFFLL) | ((offset_ns > 0) ? MAC_TA_SUBTRACT : 0u);
    if (!fol_queue(addr, val, 1u)) {
        return false;
    }
    s_cnt_steps++;
    /* Do not blind the rate estimator - correct it. The next interval measured on
       our clock is longer or shorter by exactly this step, and the amount is known,
       so it can be subtracted. Skipping the estimate instead (the first attempt)
       froze it for as long as the phase loop kept stepping, which in HARDSYNC is
       every cycle: the rate error then stays uncorrected while the steps hide it,
       and the servo never leaves HARDSYNC. */
    s_step_applied_ns += (offset_ns > 0) ? -mag : mag;
    return true;
}

/* Hard-set the clock to the master's time. Seconds first, nanoseconds second, so
   a wrap between the two writes cannot land a second in the past. The residual
   (transport plus these two SPI writes) is what HARDSYNC then removes. */
static bool servo_hard_set(uint64_t master_ns, uint64_t host_at_sample)
{
    uint32_t addr[2];
    uint32_t val[2];
    /* The sample is up to one interval old by now, and setting the clock to a time
       that is already in the past would leave HARDSYNC an offset larger than its
       own threshold - which made it hard-set again, forever. SYS_TIME says how long
       ago the pair completed; its own drift over one interval is well under a
       microsecond and does not matter here. */
    if (host_at_sample != 0u && s_ticks_per_ms != 0u) {
        uint64_t elapsed_ticks = SYS_TIME_Counter64Get() - host_at_sample;
        master_ns += (elapsed_ticks * 1000000ULL) / s_ticks_per_ms;
    }
    addr[0] = MAC_TSL; val[0] = (uint32_t)(master_ns / 1000000000ULL);
    addr[1] = MAC_TN;  val[1] = (uint32_t)(master_ns % 1000000000ULL);
    if (!fol_queue(addr, val, 2u)) {
        return false;
    }
    s_skip_rate_once = true;
    return true;
}

static void servo_enter(servo_state_t next)
{
    if (next != s_servo) {
        SYS_CONSOLE_PRINT("[PTPF] servo %s -> %s  (offset %lld ns, rate %lld ppb)\r\n",
                          servo_name(s_servo), servo_name(next),
                          (long long)s_offset_last, (long long)s_rate_corr_ppb);
        s_servo = next;
        s_servo_entered = s_samples;
    }
}

/* Called once per paired sample, in task context. */
static void servo_update(int64_t offset, uint64_t t1, uint64_t t2, uint64_t t1_prev,
                         uint64_t t2_prev, uint64_t host)
{
    int64_t mag = (offset < 0) ? -offset : offset;

    /* --- frequency estimate: how much longer our clock says the same interval was */
    if (t1_prev != 0u && !s_skip_rate_once) {
        int64_t d1 = (int64_t)(t1 - t1_prev);
        int64_t d2 = (int64_t)(t2 - t2_prev) - s_step_applied_ns;
        if (d1 > 0) {
            int64_t ppb = ((d2 - d1) * 1000000000LL) / d1;
            if (ppb > SERVO_RATE_OUTLIER_PPB || ppb < -SERVO_RATE_OUTLIER_PPB) {
                /* nIRQ delivery jitter produces these every few tens of seconds */
                s_cnt_rate_outlier++;
            } else if (!s_rate_primed) {
                /* Seed the filter with the first estimate instead of letting it
                   crawl up from zero. An N=128 IIR started at zero reaches only
                   12 % of the true value after 16 samples, so the first rate
                   correction came out eight times too small - which is exactly the
                   error that kept the servo in HARDSYNC. */
                s_rate_acc = ppb << SERVO_IIR_SHIFT;
                s_rate_est_ppb = ppb;
                s_rate_primed = true;
            } else {
                s_rate_acc += ppb - (s_rate_acc >> SERVO_IIR_SHIFT);
                s_rate_est_ppb = s_rate_acc >> SERVO_IIR_SHIFT;
            }
        }
    }
    s_skip_rate_once = false;
    s_step_applied_ns = 0;

    /* --- FIR3 over the offset, used by the two locked states */
    s_fir[2] = s_fir[1];
    s_fir[1] = s_fir[0];
    s_fir[0] = offset;
    if (s_fir_n < 3u) {
        s_fir_n++;
    }

    if (!s_servo_on) {
        return;
    }

    /* A wild offset means the assumptions are gone (master restarted, cable
       replugged): drop everything and start over rather than stepping in circles.
       Only from the locked states, though: before the clock has ever been set the
       offset is the difference in uptime between the two boards and is meant to be
       huge. Checking it earlier put the servo in a UNINIT <-> MATCHFREQ loop that
       never got as far as setting the clock. */
    if ((s_servo == SV_COARSE || s_servo == SV_FINE) && mag > SERVO_RESET_NS) {
        servo_enter(SV_UNINIT);
        s_servo_entered = s_samples;
        return;
    }

    switch (s_servo) {
        case SV_UNINIT:
            /* Collect samples with the nominal increment, then correct the rate
               BEFORE setting the clock - a clock set on a wrong rate walks away
               again immediately. */
            if ((s_samples - s_servo_entered) >= SERVO_WARMUP_SAMPLES) {
                if (servo_write_rate(-s_rate_est_ppb)) {
                    servo_enter(SV_MATCHFREQ);
                }
            }
            break;

        case SV_MATCHFREQ:
            if (servo_hard_set(t1 + (uint64_t)PTP_FOL_D_CONST_NS, host)) {
                servo_enter(SV_HARDSYNC);
            }
            break;

        case SV_HARDSYNC:
            if (mag <= SERVO_COARSE_NS) {
                servo_enter(SV_COARSE);
            } else if (mag < SERVO_HARDSYNC_NS) {
                /* Phase and rate both: stepping alone would hold the offset at
                   one cycle's worth of drift forever. The rate update runs on its
                   own schedule, not on state entry - tying it to the state made it
                   fire on every COARSE/FINE flap and wind the correction up. */
                if (s_samples >= s_rate_next_at && s_rate_est_ppb != 0) {
                    s_rate_next_at = s_samples + SERVO_RATE_EVERY;
                    (void)servo_write_rate(s_rate_corr_ppb
                                           - (s_rate_est_ppb >> SERVO_RATE_GAIN_SHIFT));
                } else {
                    (void)servo_step(offset);
                }
            } else {
                /* Still far out after the hard set: set it again rather than
                   creeping there in 16 ms steps. */
                (void)servo_hard_set(t1 + (uint64_t)PTP_FOL_D_CONST_NS, host);
            }
            break;

        case SV_COARSE:
        case SV_FINE: {
            int64_t filt = (s_fir_n >= 3u) ? ((s_fir[0] + s_fir[1] + s_fir[2]) / 3) : offset;
            int64_t fmag = (filt < 0) ? -filt : filt;

            if (mag > SERVO_HARDSYNC_NS) {
                servo_enter(SV_HARDSYNC);
                break;
            }
            /* Rate trim on its own schedule with a quarter of the residual: the
               offset steps take out the phase, the increment takes out the reason
               the phase keeps moving. Full gain on a lagging estimate overshoots -
               measured as the correction swinging to 19784 ppb for a true error of
               5117 ppb. */
            if (s_samples >= s_rate_next_at && s_rate_est_ppb != 0) {
                s_rate_next_at = s_samples + SERVO_RATE_EVERY;
                (void)servo_write_rate(s_rate_corr_ppb
                                       - (s_rate_est_ppb >> SERVO_RATE_GAIN_SHIFT));
            } else if (fmag > SERVO_PHASE_DEADBAND_NS) {
                /* Half the filtered offset: the other half is left to the next
                   sample, which is what keeps a noisy measurement from being
                   amplified into a step. Below one clock tick, do nothing at all. */
                (void)servo_step(filt >> SERVO_PHASE_GAIN_SHIFT);
            }
            if (fmag <= SERVO_FINE_NS) {
                servo_enter(SV_FINE);
            } else if (fmag > SERVO_COARSE_NS) {
                servo_enter(SV_COARSE);
            }
            break;
        }

        default:
            servo_enter(SV_UNINIT);
            break;
    }
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
    /* Start from the nominal increment and an empty rate estimate: a correction
       left over from the previous run would be measured again and applied twice. */
    s_servo = SV_UNINIT;
    s_servo_entered = s_samples;
    s_rate_acc = 0;
    s_rate_est_ppb = 0;
    s_rate_corr_ppb = 0;
    s_inc_units = INC_NOMINAL;
    s_fir_n = 0u;
    s_skip_rate_once = false;
    s_step_applied_ns = 0;
    s_rate_primed = false;
    s_rate_next_at = s_samples + SERVO_WARMUP_SAMPLES;
    s_q_len = 0u;
    s_q_pos = 0u;
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
    uint64_t t1_prev = s_t1_last;
    uint64_t t2_prev = s_t2_last;

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
        SYS_CONSOLE_PRINT("[PTPF] seq=%u  offset=%lld ns  delta=%lld ns  %s  rate=%lld ppb\r\n",
                          (unsigned)s->seq, (long long)offset, (long long)s_offset_delta,
                          servo_name(s_servo), (long long)s_rate_est_ppb);
    }

    servo_update(offset, s->t1, s->t2, t1_prev, t2_prev, s->host);
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
               responsive; at 4 to 20 cycles per second there is never a backlog.
               One sample per call while a write burst is queued, because the servo
               would otherwise decide twice on the same not-yet-applied state. */
            {
                uint32_t budget = 2u;
                while (budget-- > 0u && s_ring_tail != s_ring_head && s_q_len == 0u) {
                    sample_t s = *(const sample_t *)&s_ring[s_ring_tail];
                    s_ring_tail = (s_ring_tail + 1u) % SAMPLE_RING;
                    fol_consume(&s);
                }
                if (s_q_len != 0u) {
                    s_state = ST_REGQ;
                }
            }
            break;

        case ST_REGQ:
            /* Work through the queued writes, one register operation at a time. */
            if (!s_reg_busy) {
                if (s_q_pos >= s_q_len) {
                    s_q_len = 0u;
                    s_q_pos = 0u;
                    s_state = s_q_return;
                } else {
                    (void)fol_reg_write(s_q_addr[s_q_pos], s_q_val[s_q_pos]);
                }
            } else if (s_reg_done) {
                s_reg_busy = false;
                if (!s_reg_ok) {
                    SYS_CONSOLE_PRINT("[PTPF] write to 0x%08X failed\r\n",
                                      (unsigned int)s_q_addr[s_q_pos]);
                }
                s_q_pos++;
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
    if (!s_servo_on) {
        SYS_CONSOLE_PRINT("[PTPF] servo: OFF - measuring only, the clock is not touched\r\n");
    } else {
        SYS_CONSOLE_PRINT("[PTPF] servo: %s for %u samples   rate error: %lld ppb   correction: %lld ppb\r\n",
                          servo_name(s_servo), (unsigned)(s_samples - s_servo_entered),
                          (long long)s_rate_est_ppb, (long long)s_rate_corr_ppb);
        SYS_CONSOLE_PRINT("[PTPF] increment: MAC_TI %u ns + %u/2^24 ns   steps: %u   rate writes: %u   outliers: %u\r\n",
                          (unsigned)inc_to_ti(s_inc_units),
                          (unsigned)(s_inc_units & 0x00FFFFFFu),
                          (unsigned)s_cnt_steps, (unsigned)s_cnt_rate_writes,
                          (unsigned)s_cnt_rate_outlier);
    }
}

/* ptpf on | off | status | log [0|1] | reset */
static void cmd_ptpf(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv)
{
    (void)pCmdIO;

    if (argc < 2) {
        fol_print_status();
        SYS_CONSOLE_PRINT("usage: ptpf on | off | status | servo [on|off] | log [on|off] | reset\r\n");
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
    if (!strcmp(argv[1], "servo")) {
        if (argc >= 3) {
            bool want = (!strcmp(argv[2], "on") || !strcmp(argv[2], "1"));
            if (want != s_servo_on) {
                s_servo_on = want;
                if (want) {
                    /* Re-entering the loop: forget the old estimate, keep the
                       increment that is actually in the hardware. */
                    s_servo = SV_UNINIT;
                    s_servo_entered = s_samples;
                    s_rate_acc = 0;
                    s_rate_est_ppb = 0;
                }
            }
        }
        SYS_CONSOLE_PRINT("[PTPF] servo: %s\r\n", s_servo_on ? "on" : "off (measuring only)");
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
        s_cnt_steps = s_cnt_rate_writes = s_cnt_rate_outlier = 0u;
        /* The sample counter carries the servo's schedule, so it has to be
           rebased here as well - otherwise clearing the statistics silently stops
           the rate updates until the counter catches up again. */
        s_servo_entered = 0u;
        s_rate_next_at = SERVO_RATE_EVERY;
        SYS_CONSOLE_PRINT("[PTPF] counters cleared\r\n");
        return;
    }
    SYS_CONSOLE_PRINT("usage: ptpf on | off | status | servo [on|off] | log [on|off] | reset\r\n");
}

static const SYS_CMD_DESCRIPTOR ptpf_cmd_tbl[] = {
    {"ptpf", (SYS_CMD_FNC)cmd_ptpf, ": PTP follower + servo (ptpf on | off | status | servo [on|off] | log | reset)"},
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
