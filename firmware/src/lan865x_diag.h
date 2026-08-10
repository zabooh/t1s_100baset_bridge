/*******************************************************************************
  LAN865x register access, IEEE transmitter test modes and PLCA control

  File Name:
    lan865x_diag.h

  Summary:
    Self-contained diagnostics layer on top of the Harmony LAN865x MAC-PHY
    driver: generic register read/write/read-modify-write, the IEEE 802.3-2022
    section 147.5.2 transmitter test modes, and PLCA node configuration - all
    reachable both programmatically and from the serial console.

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
    lan_rmw, testmode and plca_node.

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

#define LAN865X_TESTMODE_MAX    4u            /* highest valid transmitter test mode     */

/* T1SPMACTL bit masks, for use with LAN865X_DIAG_Rmw(). */
#define LAN865X_PMACTL_RST      0x00008000u   /* PMA reset, self-clearing                */
#define LAN865X_PMACTL_TXD      0x00004000u   /* transmit disable                        */
#define LAN865X_PMACTL_LPE      0x00000800u   /* low power enable                        */
#define LAN865X_PMACTL_MDE      0x00000400u   /* multidrop enable                        */
#define LAN865X_PMACTL_LBE      0x00000001u   /* PMA loopback enable                     */

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

#ifdef __cplusplus
}
#endif

#endif /* LAN865X_DIAG_H */
