/*******************************************************************************
  Time-triggered actions - implementation

  See ptp_trigger.h.  Phase C of PTP_TIMEBASE_PLAN.md, software stage: the
  instant is computed on the shared timebase, the firing is a SYS_TIME one-shot
  callback.  Phase E replaces the firing with a hardware compare; nothing in the
  scheduling logic here has to change for that.
*******************************************************************************/

#include <stdlib.h>
#include <string.h>
#include "ptp_trigger.h"
#include "ptp_timebase.h"
#include "definitions.h"

/* --------------------------------------------------------------------------- */
/* state                                                                       */
/* --------------------------------------------------------------------------- */

typedef struct
{
    bool             used;
    bool             isr_ctx;
    uint16_t         id;
    PTP_TRIG_Handler fn;
    uintptr_t        ctx;
    uint32_t         last_seq;
    bool             seq_valid;
} action_t;

static action_t s_act[PTP_TRIG_ACTIONS];

static void trig_cb(uintptr_t context);      /* SYS_TIME callback, ISR context */

static volatile bool     s_armed;
static volatile uint16_t s_armed_id;
static volatile uint64_t s_target_ns;
static volatile uint64_t s_target_L;
static volatile uint64_t s_period_ns;
static volatile uint64_t s_phase_ns;

/* Set by the callback, consumed by PTP_TRIG_Tasks() for deferred handlers. */
static volatile bool     s_pending_defer;
static volatile uint64_t s_pending_sched_ns;
static volatile int32_t  s_pending_late;
static volatile uint16_t s_pending_id;

static SYS_TIME_HANDLE s_timer = SYS_TIME_HANDLE_INVALID;
static PTP_TRIG_MODE   s_mode = PTP_TRIG_MODE_STRICT;

static uint64_t s_ticks_per_us;
static uint32_t s_cnt_fired;
static uint32_t s_cnt_refused;
static uint32_t s_cnt_missed;
static int32_t  s_late_last;
static int32_t  s_late_max;
static int32_t  s_late_min;
static uint64_t s_late_sum;
static uint32_t s_late_n;

/* --------------------------------------------------------------------------- */
/* helpers                                                                     */
/* --------------------------------------------------------------------------- */

static action_t *act_find(uint16_t id)
{
    uint32_t i;
    for (i = 0u; i < PTP_TRIG_ACTIONS; i++)
    {
        if (s_act[i].used && s_act[i].id == id)
        {
            return &s_act[i];
        }
    }
    return NULL;
}

static void trig_disarm(void)
{
    if (s_timer != SYS_TIME_HANDLE_INVALID)
    {
        SYS_TIME_TimerDestroy(s_timer);
        s_timer = SYS_TIME_HANDLE_INVALID;
    }
    s_armed = false;
}

static void trig_note_late(int32_t late)
{
    s_late_last = late;
    if (s_late_n == 0u)
    {
        s_late_max = late;
        s_late_min = late;
    }
    else
    {
        if (late > s_late_max) { s_late_max = late; }
        if (late < s_late_min) { s_late_min = late; }
    }
    /* Mean over the magnitude is meaningless with a sign, so sum the raw value
       and let the reader divide - min/max are the interesting columns anyway. */
    s_late_sum += (uint64_t)((late < 0) ? -late : late);
    s_late_n++;
}

/* Arms the SYS_TIME one-shot for a target already known in local ticks.
   Returns false if the target is not far enough away to arm. */
static bool trig_arm_ticks(uint64_t target_L)
{
    uint64_t now = SYS_TIME_Counter64Get();
    uint64_t delay_ticks;
    uint32_t delay_us;

    if (target_L <= now)
    {
        return false;
    }
    delay_ticks = target_L - now;
    delay_us = (uint32_t)(delay_ticks / s_ticks_per_us);
    if (delay_us == 0u)
    {
        return false;
    }

    /* Truncating to whole microseconds means the callback can arrive a fraction
       EARLY - hence the signed lateness.  Rounding up instead would bias every
       trigger late, which is worse: early is correctable by the handler, late is
       not. */
    s_timer = SYS_TIME_CallbackRegisterUS(trig_cb, (uintptr_t)0, delay_us, SYS_TIME_SINGLE);
    return (s_timer != SYS_TIME_HANDLE_INVALID);
}

/* --------------------------------------------------------------------------- */
/* the callback - ISR context                                                  */
/* --------------------------------------------------------------------------- */

static void trig_cb(uintptr_t context)
{
    uint64_t now;
    int64_t  late_t;
    action_t *a;
    (void)context;

    /* Lateness first, before anything else can add to it. */
    now = SYS_TIME_Counter64Get();
    late_t = (int64_t)now - (int64_t)s_target_L;
    if (late_t > INT32_MAX) { late_t = INT32_MAX; }
    if (late_t < INT32_MIN) { late_t = INT32_MIN; }

    s_timer = SYS_TIME_HANDLE_INVALID;
    s_armed = false;
    trig_note_late((int32_t)late_t);
    s_cnt_fired++;

    a = act_find(s_armed_id);
    if (a != NULL)
    {
        if (a->isr_ctx)
        {
            a->fn(s_target_ns, (int32_t)late_t, a->ctx);
        }
        else
        {
            /* Deferred: only a flag here.  A handler that prints or sends would
               otherwise do it from the TC0 ISR (plan C.3). */
            s_pending_sched_ns = s_target_ns;
            s_pending_late = (int32_t)late_t;
            s_pending_id = s_armed_id;
            s_pending_defer = true;
        }
    }

    /* Periodic: re-arm here, in the same ISR, so the period does not wander with
       the main-loop load (plan C.9). */
    if (s_period_ns != 0u)
    {
        uint64_t next = s_target_ns + s_period_ns;
        uint64_t now_ns;

        /* If the next instant is already gone, SKIP whole periods rather than
           firing a burst to catch up. */
        if (PTP_TB_Convert(now, &now_ns))
        {
            while (next <= now_ns)
            {
                next += s_period_ns;
                s_cnt_missed++;
            }
        }

        if (PTP_TB_LocalFor(next, (uint64_t *)&s_target_L))
        {
            s_target_ns = next;
            if (trig_arm_ticks(s_target_L))
            {
                s_armed = true;
            }
        }
    }
}

/* --------------------------------------------------------------------------- */
/* public                                                                      */
/* --------------------------------------------------------------------------- */

void PTP_TRIG_Initialize(void)
{
    uint32_t hz = SYS_TIME_FrequencyGet();

    memset(s_act, 0, sizeof(s_act));
    trig_disarm();
    s_period_ns = 0u;
    s_phase_ns = 0u;
    s_pending_defer = false;
    s_mode = PTP_TRIG_MODE_STRICT;
    s_cnt_fired = 0u;
    s_cnt_refused = 0u;
    s_cnt_missed = 0u;
    s_late_last = 0;
    s_late_max = 0;
    s_late_min = 0;
    s_late_sum = 0u;
    s_late_n = 0u;

    if (hz == 0u)
    {
        hz = 60000000u;
    }
    s_ticks_per_us = (uint64_t)hz / 1000000u;
    if (s_ticks_per_us == 0u)
    {
        s_ticks_per_us = 1u;
    }
}

bool PTP_TRIG_Register(uint16_t action_id, PTP_TRIG_Handler h, uintptr_t ctx, bool isr_ctx)
{
    uint32_t i;
    action_t *a = act_find(action_id);

    if (h == NULL)
    {
        return false;
    }
    if (a == NULL)
    {
        for (i = 0u; i < PTP_TRIG_ACTIONS; i++)
        {
            if (!s_act[i].used)
            {
                a = &s_act[i];
                break;
            }
        }
        if (a == NULL)
        {
            return false;
        }
        a->used = true;
        a->id = action_id;
        a->seq_valid = false;
    }
    a->fn = h;
    a->ctx = ctx;
    a->isr_ctx = isr_ctx;
    return true;
}

/* Common validation for both schedule entry points. */
static PTP_TRIG_RESULT trig_check(uint16_t action_id, uint32_t cmd_seq, action_t **out)
{
    action_t *a = act_find(action_id);

    if (a == NULL)
    {
        return PTP_TRIG_ERR_NO_ACTION;
    }
    if (!PTP_TB_IsUsable() && s_mode == PTP_TRIG_MODE_STRICT)
    {
        return PTP_TRIG_ERR_NO_TIME;
    }
    /* Fire exactly once per (action, sequence).  A repeated command - the master
       sends one-way and repeats deliberately - must not schedule twice (plan
       C.7).  The SOME/IP Session ID is what carries this over the wire. */
    if (a->seq_valid && a->last_seq == cmd_seq)
    {
        return PTP_TRIG_ERR_DUPLICATE;
    }
    *out = a;
    return PTP_TRIG_OK;
}

PTP_TRIG_RESULT PTP_TRIG_ScheduleAt(uint16_t action_id, uint32_t cmd_seq, uint64_t tx_ns)
{
    action_t *a = NULL;
    PTP_TRIG_RESULT r = trig_check(action_id, cmd_seq, &a);
    uint64_t now_ns;
    uint64_t target_L;

    if (r != PTP_TRIG_OK)
    {
        s_cnt_refused++;
        return r;
    }
    if (s_armed)
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_BUSY;
    }
    if (!PTP_TB_Now(&now_ns))
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_NO_TIME;
    }
    if (tx_ns < now_ns + (uint64_t)PTP_TRIG_MIN_LEAD_MS * 1000000ULL)
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_PAST;
    }
    if (tx_ns > now_ns + (uint64_t)PTP_TRIG_MAX_LEAD_MS * 1000000ULL)
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_TOO_FAR;
    }
    if (!PTP_TB_LocalFor(tx_ns, &target_L))
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_NO_TIME;
    }

    s_target_ns = tx_ns;
    s_target_L = target_L;
    s_period_ns = 0u;
    s_armed_id = action_id;

    if (!trig_arm_ticks(target_L))
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_ARM;
    }
    s_armed = true;
    a->last_seq = cmd_seq;
    a->seq_valid = true;
    return PTP_TRIG_OK;
}

PTP_TRIG_RESULT PTP_TRIG_SchedulePeriodic(uint16_t action_id, uint32_t cmd_seq,
                                          uint64_t period_ns, uint64_t phase_ns)
{
    action_t *a = NULL;
    PTP_TRIG_RESULT r = trig_check(action_id, cmd_seq, &a);
    uint64_t now_ns;
    uint64_t n;
    uint64_t next;
    uint64_t target_L;

    if (r != PTP_TRIG_OK)
    {
        s_cnt_refused++;
        return r;
    }
    if (period_ns == 0u)
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_PAST;
    }
    if (!PTP_TB_Now(&now_ns))
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_NO_TIME;
    }

    trig_disarm();

    /* Absolute phase: the next instant on the global grid, not "now + period".
       This is the whole reason two nodes can align at all (plan G.1). */
    if (now_ns <= phase_ns)
    {
        next = phase_ns;
    }
    else
    {
        n = (now_ns - phase_ns) / period_ns + 1u;
        next = phase_ns + n * period_ns;
    }
    /* Respect the same minimum lead as a one-shot. */
    while (next < now_ns + (uint64_t)PTP_TRIG_MIN_LEAD_MS * 1000000ULL)
    {
        next += period_ns;
    }

    if (!PTP_TB_LocalFor(next, &target_L))
    {
        s_cnt_refused++;
        return PTP_TRIG_ERR_NO_TIME;
    }

    s_target_ns = next;
    s_target_L = target_L;
    s_period_ns = period_ns;
    s_phase_ns = phase_ns;
    s_armed_id = action_id;

    if (!trig_arm_ticks(target_L))
    {
        s_period_ns = 0u;
        s_cnt_refused++;
        return PTP_TRIG_ERR_ARM;
    }
    s_armed = true;
    a->last_seq = cmd_seq;
    a->seq_valid = true;
    return PTP_TRIG_OK;
}

void PTP_TRIG_Cancel(void)
{
    s_period_ns = 0u;
    trig_disarm();
}

void PTP_TRIG_ModeSet(PTP_TRIG_MODE mode)
{
    s_mode = mode;
}

void PTP_TRIG_Tasks(void)
{
    if (s_pending_defer)
    {
        action_t *a;
        uint64_t sched = s_pending_sched_ns;
        int32_t  late = s_pending_late;
        uint16_t id = s_pending_id;

        s_pending_defer = false;
        a = act_find(id);
        if (a != NULL && !a->isr_ctx)
        {
            a->fn(sched, late, a->ctx);
        }
    }
}

void PTP_TRIG_StatusGet(PTP_TRIG_STATUS *out)
{
    if (out == NULL)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->armed = s_armed;
    out->mode = s_mode;
    out->action_id = s_armed_id;
    out->target_ns = s_target_ns;
    out->period_ns = s_period_ns;
    out->fired = s_cnt_fired;
    out->refused = s_cnt_refused;
    out->missed = s_cnt_missed;
    out->late_last_ticks = s_late_last;
    out->late_max_ticks = s_late_max;
    out->late_min_ticks = s_late_min;
    out->late_sum_ticks = s_late_sum;
    out->late_n = s_late_n;
    out->ticks_per_us = (uint32_t)s_ticks_per_us;
}

const char *PTP_TRIG_ResultName(PTP_TRIG_RESULT r)
{
    switch (r)
    {
        case PTP_TRIG_OK:             return "ok";
        case PTP_TRIG_ERR_NO_ACTION:  return "no such action id";
        case PTP_TRIG_ERR_NO_TIME:    return "timebase not usable";
        case PTP_TRIG_ERR_PAST:       return "target in the past or too close";
        case PTP_TRIG_ERR_TOO_FAR:    return "target too far ahead";
        case PTP_TRIG_ERR_DUPLICATE:  return "duplicate action+sequence";
        case PTP_TRIG_ERR_BUSY:       return "a trigger is already armed";
        case PTP_TRIG_ERR_ARM:        return "could not arm the timer";
        default:                      return "?";
    }
}

/* --------------------------------------------------------------------------- */
/* built-in demo actions and CLI                                               */
/* --------------------------------------------------------------------------- */

/* Action 1, ISR context: the cheapest thing that still proves the trigger
   fired at the right instant.  No printing - that is what action 2 is for. */
static volatile uint32_t s_demo_isr_hits;
static volatile uint64_t s_demo_isr_L;

static void demo_isr(uint64_t scheduled_ns, int32_t late_ticks, uintptr_t ctx)
{
    (void)scheduled_ns; (void)late_ticks; (void)ctx;
    s_demo_isr_L = SYS_TIME_Counter64Get();
    s_demo_isr_hits++;
}

/* Action 2, task context: allowed to print, which action 1 is not. */
static void demo_task(uint64_t scheduled_ns, int32_t late_ticks, uintptr_t ctx)
{
    (void)ctx;
    SYS_CONSOLE_PRINT("[TRIG] action 2 fired: scheduled %llu ns, late %ld ticks (%ld ns)\r\n",
                      (unsigned long long)scheduled_ns, (long)late_ticks,
                      (long)((int64_t)late_ticks * 1000 / (int64_t)s_ticks_per_us));
}

bool PTP_TRIG_CliTry(int argc, char **argv)
{
    PTP_TRIG_STATUS st;
    PTP_TRIG_RESULT r;
    uint64_t now_ns;

    if (argc >= 3 && !strcmp(argv[1], "fire"))
    {
        uint32_t ahead_ms = (uint32_t)strtoul(argv[2], NULL, 0);
        uint16_t id = (argc >= 4) ? (uint16_t)strtoul(argv[3], NULL, 0) : 1u;
        static uint32_t seq = 0u;
        if (!PTP_TB_Now(&now_ns))
        {
            SYS_CONSOLE_PRINT("[TRIG] no timebase\r\n");
            return;
        }
        r = PTP_TRIG_ScheduleAt(id, ++seq, now_ns + (uint64_t)ahead_ms * 1000000ULL);
        SYS_CONSOLE_PRINT("[TRIG] schedule id=%u in %lu ms: %s\r\n",
                          (unsigned)id, (unsigned long)ahead_ms, PTP_TRIG_ResultName(r));
        return true;
    }

    if (argc >= 3 && !strcmp(argv[1], "per"))
    {
        uint32_t period_ms = (uint32_t)strtoul(argv[2], NULL, 0);
        uint16_t id = (argc >= 4) ? (uint16_t)strtoul(argv[3], NULL, 0) : 1u;
        static uint32_t pseq = 1000u;
        /* phase 0: the grid is anchored on grandmaster zero, so every node lands
           on the same instants no matter when it got the command (plan G.1). */
        r = PTP_TRIG_SchedulePeriodic(id, ++pseq, (uint64_t)period_ms * 1000000ULL, 0u);
        SYS_CONSOLE_PRINT("[TRIG] periodic id=%u every %lu ms, phase 0: %s\r\n",
                          (unsigned)id, (unsigned long)period_ms, PTP_TRIG_ResultName(r));
        return true;
    }

    if (argc >= 2 && !strcmp(argv[1], "cancel"))
    {
        PTP_TRIG_Cancel();
        SYS_CONSOLE_PRINT("[TRIG] cancelled\r\n");
        return true;
    }

    if (argc >= 3 && !strcmp(argv[1], "mode"))
    {
        bool free_mode = (!strcmp(argv[2], "free"));
        PTP_TRIG_ModeSet(free_mode ? PTP_TRIG_MODE_FREE : PTP_TRIG_MODE_STRICT);
        SYS_CONSOLE_PRINT("[TRIG] mode: %s%s\r\n", free_mode ? "FREE" : "STRICT",
                          free_mode ? "  <- fires without a usable timebase, NOT synchronized" : "");
        return true;
    }

    if (argc < 2 || strcmp(argv[1], "trig") != 0)
    {
        return false;              /* not ours - let tbase handle it */
    }

    PTP_TRIG_StatusGet(&st);
    SYS_CONSOLE_PRINT("[TRIG] armed: %s   mode: %s   action: %u   period: %llu ms\r\n",
                      st.armed ? "yes" : "no",
                      (st.mode == PTP_TRIG_MODE_FREE) ? "FREE" : "STRICT",
                      (unsigned)st.action_id, (unsigned long long)(st.period_ns / 1000000ULL));
    SYS_CONSOLE_PRINT("[TRIG] fired: %lu   refused: %lu   skipped periods: %lu\r\n",
                      (unsigned long)st.fired, (unsigned long)st.refused, (unsigned long)st.missed);
    if (st.late_n > 0u)
    {
        int32_t tpu = (int32_t)st.ticks_per_us;
        SYS_CONSOLE_PRINT("[TRIG] lateness over %lu fires, ticks (%ld ticks = 1 us):\r\n",
                          (unsigned long)st.late_n, (long)tpu);
        SYS_CONSOLE_PRINT("[TRIG]   last %ld (%ld ns)   min %ld (%ld ns)   max %ld (%ld ns)\r\n",
                          (long)st.late_last_ticks, (long)((int64_t)st.late_last_ticks * 1000 / tpu),
                          (long)st.late_min_ticks,  (long)((int64_t)st.late_min_ticks * 1000 / tpu),
                          (long)st.late_max_ticks,  (long)((int64_t)st.late_max_ticks * 1000 / tpu));
        SYS_CONSOLE_PRINT("[TRIG]   mean |late| %llu ticks (%llu ns)\r\n",
                          (unsigned long long)(st.late_sum_ticks / st.late_n),
                          (unsigned long long)(st.late_sum_ticks * 1000ULL / st.late_n / (uint64_t)tpu));
    }
    SYS_CONSOLE_PRINT("[TRIG] demo action 1 (isr) hits: %lu\r\n", (unsigned long)s_demo_isr_hits);
    return true;
}

void PTP_TRIG_CliInit(void)
{
    /* Two built-ins so both contexts are exercised without extra wiring:
       id 1 in ISR context, id 2 deferred to the main loop. */
    (void)PTP_TRIG_Register(1u, demo_isr, 0u, true);
    (void)PTP_TRIG_Register(2u, demo_task, 0u, false);
}
