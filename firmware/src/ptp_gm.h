/*******************************************************************************
  PTP grandmaster on the 10BASE-T1S segment

  File Name:
    ptp_gm.h

  Summary:
    Sends IEEE 1588 Sync + Follow_Up on eth0 with a hardware transmit timestamp
    taken by the LAN8651 at the end of the SFD, so followers on the multidrop bus
    can lock their wall clock to this device.

  Description:
    One-way only: this device sends, followers listen and send nothing. There is
    no Delay_Req/Delay_Resp and no path-delay measurement - the delay is treated
    as a constant. The reasoning, and what that costs, is in
    LAN8651_TIME_SYNC.md section 11.4; the staged plan is
    PTP_IMPLEMENTATION_PLAN.md.

    Two-step operation, because it cannot be anything else: the egress timestamp
    only exists after the frame has left, so `Sync` announces it (twoStepFlag)
    and `Follow_Up` carries it. One cycle is therefore:

      1. send Sync with TSC = 1, which asks the MAC-PHY to capture the egress
         time into the Transmit Timestamp Capture A register pair
      2. read TTSCAH/TTSCAL until the value differs from the previous cycle
      3. send Follow_Up with that value as preciseOriginTimestamp

    Sending is OFF until someone turns it on. It is a tool, not a service: the
    console starts and stops it, the interval is settable, and it only starts by
    itself when the persistent configuration says so ("setenv ptp_auto 1"). The
    default matters - the existing test scripts count frames on the bus and use
    the endpoint's own 1 Hz traffic as their oracle, which a grandmaster sending
    unasked would falsify.

  Freshness instead of a status bit:
    The natural handshake would be TTSCAA in OA_STATUS0, but the driver reads
    that register on every extended-status event and clears it (see
    _OnStatus0() in drv_lan865x_api.c), so a consumer here would race it. This
    module instead compares the 64-bit capture against the previous cycle's
    value: a capture that has not happened yet still reads the old timestamp.
    That shadow deliberately survives stop/start, which is what keeps a stale
    timestamp from being credited to the first Sync of a new run.

  Dependencies:
    - the Harmony LAN865x driver (raw send with TSC, register access)
    - SYS_CMD / SYS_CONSOLE / SYS_TIME
    - the TCP/IP stack, for this interface's own MAC address only
    - port_mirror.h: MIRROR_RawTx() is called after each successful send, which
      is what makes these frames visible to Wireshark on eth1 while "mirror 1"
      is on. The raw driver path bypasses the mirror's normal TX hook, so
      without that call a capture stays empty and the grandmaster looks dead.
      Dropping those two calls is all that is needed to decouple this module.

    It does NOT touch generated code, and it does not configure the transmit
    pattern matcher: the driver's own init already leaves it enabled and
    matching every frame at the SFD, which is exactly what is wanted here.
 *******************************************************************************/

#ifndef PTP_GM_H
#define PTP_GM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PTP over Ethernet, IEEE 1588 Annex F. */
#define PTP_GM_ETHERTYPE            0x88F7u

/* Interval bounds and the value a fresh board starts with. 1000 ms is also the
   one interval that logMessageInterval can express exactly (2^0 s). */
#define PTP_GM_INTERVAL_MIN_MS      50u
#define PTP_GM_INTERVAL_MAX_MS      10000u
#define PTP_GM_INTERVAL_DEFAULT_MS  1000u

/* Added to the captured egress timestamp before it goes into
   preciseOriginTimestamp. Zero in phase 1: what is sent is exactly what the
   hardware captured, which keeps the verification honest. Phase 3 decides
   whether the path-delay constant (~3788 ns on the reference hardware,
   LAN8651_TIME_SYNC.md section 11.4) is pre-compensated here or added by the
   follower. */
#define PTP_GM_STATIC_OFFSET_NS     0

/* Register the console command group ("ptp", plus "ptphelp"). Call once, after
   SYS_CMD is up. */
void PTP_GM_Initialize(void);

/* Drive the send cycle and the timestamp read-back. Call from the main loop, in
   a state where LAN865x register access is serviced - register operations issued
   earlier than that are not answered, and the first cycle would look like a
   register fault. */
void PTP_GM_Tasks(void);

/* Start / stop sending. Start enables frame timestamping (FTSE + FTSS in
   OA_CONFIG0) and returns false if that register operation cannot be issued.
   Stop finishes nothing by halves: a cycle waiting for its timestamp is
   abandoned rather than completed with a Follow_Up that has no Sync. */
bool PTP_GM_Start(void);
void PTP_GM_Stop(void);

bool PTP_GM_IsRunning(void);

/* Send interval in milliseconds, PTP_GM_INTERVAL_MIN_MS..PTP_GM_INTERVAL_MAX_MS.
   A change takes effect with the next cycle and does NOT reset sequenceId, so a
   follower watching the sequence sees no gap. Returns false if out of range. */
bool PTP_GM_SetInterval(uint32_t ms);
uint32_t PTP_GM_Interval(void);

/* Hand over the persisted configuration. Called from env_apply(); only the
   FIRST call can arm the automatic start, so re-applying the environment at
   runtime (saveenv/readenv) never starts sending behind the operator's back.
   The interval is adopted whenever sending is currently off. */
void PTP_GM_ConfigureAutoStart(bool enable, uint32_t interval_ms);

#ifdef __cplusplus
}
#endif

#endif /* PTP_GM_H */
