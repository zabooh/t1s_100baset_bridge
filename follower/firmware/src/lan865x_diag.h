/*******************************************************************************
  LAN865x register access, IEEE transmitter test modes, PLCA control and SQI

  File Name:
    lan865x_diag.h

  Summary:
    Self-contained diagnostics layer on top of the Harmony LAN865x MAC-PHY
    driver: generic register read/write/read-modify-write, the IEEE 802.3-2022
    section 147.5.2 transmitter test modes, PLCA node configuration, and
    continuous Signal Quality Indicator monitoring - all reachable both
    programmatically and from the serial console.

  Description:
    This module is deliberately free of application dependencies so it can be
    dropped into another LAN865x/LAN8651 project unchanged. It needs only:

      - the Harmony LAN865x driver (DRV_LAN865X_*)
      - SYS_CMD    (console command group registration)
      - SYS_TIME   (64-bit counter for operation timeouts)
      - SYS_CONSOLE(print)

    It does NOT depend on the TCP/IP stack, on any persistent-configuration
    layer, or on the host application's state machine or data structures.

    To use it in a new project:

      1. Add lan865x_diag.c/.h to the project.
      2. Call LAN865X_DIAG_Initialize() once, after SYS_CMD is up.
      3. Call LAN865X_DIAG_Tasks() from the main loop / an idle state.

    That is the whole integration. The console then offers lan_read, lan_write,
    lan_rmw, testmode, plca_node and sqi.

    All register operations are asynchronous and there is exactly ONE slot: a
    request is rejected (not queued) while another is in flight, and results are
    printed from LAN865X_DIAG_Tasks() when the driver callback arrives.
 *******************************************************************************/

#ifndef LAN865X_DIAG_H
#define LAN865X_DIAG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// *****************************************************************************
// Section: Register addresses
//
// The address is 32 bits wide: the upper 16 bits select the memory map (MMS),
// the lower 16 bits are the register offset inside that bank. MMS 3 is the PHY
// PMA/PMD bank, MMS 4 the vendor-specific bank.
// *****************************************************************************

#define LAN865X_T1STSTCTL       0x000308FBu   /* test mode control, mode in bits 15:13   */
#define LAN865X_T1STSTCTL_MASK  0x0000E000u   /* only those three bits read back         */
#define LAN865X_T1SPMACTL       0x000308F9u   /* PMA control (RST/TXD/LPE/MDE/LBE)       */
#define LAN865X_T1SPMASTS       0x000308FAu   /* PMA status, read-only                   */
#define LAN865X_PLCA_CTRL1      0x0004CA02u   /* NODE_CNT in 15:8, NODE_ID in 7:0        */

/* Signal Quality Indicator (datasheet DS60001734F section 7.5 / 11.5.52-55). */
#define LAN865X_SQICTL          0x000400A0u   /* SQIRST bit15 (self-clear), SQIEN bit14  */
#define LAN865X_SQISTS0         0x000400A1u   /* SQIERR/SQIVLD(RC)/SQIVAL/SQIERRC, RO    */
#define LAN865X_SQICFG0         0x000400AAu   /* TOID in bits 11:4                       */
#define LAN865X_SQICFG2         0x000400ACu   /* SQIINTTHR in bits 12:8                  */

#define LAN865X_TESTMODE_MAX    4u            /* highest valid transmitter test mode     */

/* T1SPMACTL bit masks, for use with LAN865X_DIAG_Rmw(). */
#define LAN865X_PMACTL_RST      0x00008000u   /* PMA reset, self-clearing                */
#define LAN865X_PMACTL_TXD      0x00004000u   /* transmit disable                        */
#define LAN865X_PMACTL_LPE      0x00000800u   /* low power enable                        */
#define LAN865X_PMACTL_MDE      0x00000400u   /* multidrop enable                        */
#define LAN865X_PMACTL_LBE      0x00000001u   /* PMA loopback enable                     */

/* SQICTL bit mask, for use with LAN865X_DIAG_Rmw(). */
#define LAN865X_SQICTL_SQIEN    0x00004000u

/* SQICFG0/SQICFG2 fields, for use with LAN865X_DIAG_Rmw() (the datasheet requires
 * read-modify-write here to avoid disturbing reserved bits). */
#define LAN865X_SQICFG0_TOID_MASK        0x00000FF0u
#define LAN865X_SQICFG0_TOID_SHIFT       4u
#define LAN865X_SQICFG2_SQIINTTHR_MASK   0x00001F00u
#define LAN865X_SQICFG2_SQIINTTHR_POLL   0x00001F00u   /* 0x1F = interrupt disabled, polling mode */

/* SQISTS0 fields. */
#define LAN865X_SQISTS0_SQIERR      0x00000080u
#define LAN865X_SQISTS0_SQIVLD      0x00000040u   /* read-clear; re-arms automatically  */
#define LAN865X_SQISTS0_SQIVAL_MASK 0x00000038u
#define LAN865X_SQISTS0_SQIVAL_SHIFT 3u
#define LAN865X_SQISTS0_SQIERRC_MASK 0x00000007u

#define LAN865X_SQI_TOID_ALL    0xFFu         /* weighted average over all PLCA nodes    */

// *****************************************************************************
// Section: Life cycle
// *****************************************************************************

/* Register the console command group. Call once, after SYS_CMD is available. */
void LAN865X_DIAG_Initialize(void);

/* Service the register state machine: start queued operations, detect timeouts,
   print results and run the test-mode auto-revert. Call from the main loop. */
void LAN865X_DIAG_Tasks(void);

/* true while a register operation is in flight. Requests are rejected, not
   queued, so callers that must not fail should check this first. */
bool LAN865X_DIAG_Busy(void);

// *****************************************************************************
// Section: Register access
//
// Each of these queues one operation and returns false if the single slot is
// busy. The result is printed by LAN865X_DIAG_Tasks() when the callback lands.
// *****************************************************************************

bool LAN865X_DIAG_Read(uint32_t addr);
bool LAN865X_DIAG_Write(uint32_t addr, uint32_t value);

/* Read-modify-write. Driver semantics: new = (old & ~mask) | value. Note that
   the driver does NOT mask `value`, so bits outside `mask` are written anyway.
   The masked field is verified by a follow-up read; self-clearing bits such as
   LAN865X_PMACTL_RST will therefore legitimately report a failed verify. */
bool LAN865X_DIAG_Rmw(uint32_t addr, uint32_t mask, uint32_t value);

// *****************************************************************************
// Section: Transmitter test modes
// *****************************************************************************

/* Select a transmitter test mode (0 = normal operation, 1..4 per IEEE
   802.3-2022 section 147.5.2) and verify it by reading T1STSTCTL back.

   seconds > 0 arms an automatic return to normal operation after that many
   seconds, which keeps a forgotten test mode from later presenting itself as a
   link that will not come up. seconds == 0 leaves the mode active indefinitely.

   Modes 1..4 stop normal traffic by design: the T1S link goes down. Returns
   false if a register operation is already in progress or the mode is invalid. */
bool LAN865X_DIAG_TestMode(uint32_t mode, uint32_t seconds);

/* Human-readable name of a test mode, for logging. */
const char *LAN865X_DIAG_TestModeName(uint32_t mode);

// *****************************************************************************
// Section: PLCA
// *****************************************************************************

/* Apply PLCA node id and node count to the LAN865x: update the driver's node id
   (so a LOFE re-init reuses it) and queue the PLCA_CTRL1 write. Both values are
   remembered, so a later node-id-only change keeps the same node count.

   Node id 0 makes this device the PLCA coordinator.

   Prints "[PLCA] LAN busy - apply skipped" and does nothing if a register
   operation is in progress - in that case the caller's value is NOT applied. */
void LAN865X_DIAG_ApplyPlca(uint8_t node_id, uint8_t node_cnt);

/* Last applied values. Note these reflect what was requested, which for the
   node id is set before the register write completes - read LAN865X_PLCA_CTRL1
   if you need to know what the PHY actually holds. */
uint8_t LAN865X_DIAG_PlcaNodeId(void);
uint8_t LAN865X_DIAG_PlcaNodeCnt(void);

// *****************************************************************************
// Section: SQI (Signal Quality Indicator)
//
// SQI is not a one-shot register: the PHY accumulates it from live received
// traffic over a duration that depends on how much traffic is flowing (the
// datasheet recommends polling roughly once per second), and the result is
// exposed through a read-clear valid bit that re-arms itself automatically -
// reading SQISTS0 both consumes the current value and starts the next
// accumulation. Modeled here as a background poll that shares the module's
// single register slot with lan_read/write/rmw/testmode/plca_node.
// *****************************************************************************

/* Start (or restart) continuous SQI monitoring of one PLCA transmit
   opportunity (0..254), or LAN865X_SQI_TOID_ALL for a traffic-weighted
   average over all nodes. Resets the accumulated min/max/sample/error
   counters. Configures TOID and polling mode, then enables SQIEN - each step
   is read-modify-write plus verify, per the datasheet's own recommendation. */
void LAN865X_DIAG_SqiStart(uint8_t toid);

/* Stop monitoring (clears SQIEN). Safe to call even if not active. */
void LAN865X_DIAG_SqiStop(void);

/* true once LAN865X_DIAG_SqiStart() has been called and not yet stopped -
   includes the setup phase, before the first valid sample. */
bool LAN865X_DIAG_SqiActive(void);

#ifdef __cplusplus
}
#endif

#endif /* LAN865X_DIAG_H */
