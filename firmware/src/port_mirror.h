/*******************************************************************************
  eth0 (10BASE-T1S) -> eth1 (100BASE-T) port mirror / SPAN

  File Name:
    port_mirror.h

  Summary:
    Clones the bridge's own T1S conversation onto eth1 so a PC running Wireshark
    on the 100BASE-T side can capture the two-wire bus.

  Description:
    A 10BASE-T1S segment cannot be tapped with an ordinary NIC, and the MAC
    bridge only carries frames that belong on the other port. That leaves the
    interesting part invisible: the traffic between the bridge firmware itself
    and the nodes on the bus (its own ARP/ICMP and the replies to it). This
    module copies exactly that traffic onto eth1.

    Three entry points feed the mirror. The two stack-borne ones filter against
    the bridge's OWN eth0 MAC so the capture stays duplicate-free:

      - RX (bus -> bridge): only frames addressed TO the bridge.
      - TX (bridge -> bus): only frames the bridge ITSELF originated.
      - RAW TX: frames a module built and sent itself, bypassing the stack
        (MIRROR_RawTx, see below) - no filter, they are ours by construction.

    Frames the MAC bridge merely forwards between the PC and the bus keep their
    original MAC addresses and already reach eth1 natively; mirroring them would
    show them twice at the PC.

  Dependencies:
    Unlike lan865x_diag.c, this module is NOT free-standing. It needs:

      - the Harmony TCP/IP stack (packet allocation, TCPIP_NET_IF internals via
        tcpip_manager_control.h)
      - DRV_GMAC_PacketTx, i.e. specifically a GMAC as the mirror destination
      - the LAN865x driver patched to call mirror_eth0_tx_hook() from its TX path
      - a call to MIRROR_Eth0Rx() from the eth0 RX packet handler
      - a call to MIRROR_RawTx() from every module that sends on eth0 with
        DRV_LAN865X_SendRawEthFrame() (noip_test.c today, the PTP grandmaster
        next); frames sent that way reach eth1 no other way

    It is reusable in another Harmony two-port bridge, not in an arbitrary
    project. Adapting it means replacing the two interface indices (0 = source,
    1 = mirror destination) and the destination MAC driver.
 *******************************************************************************/

#ifndef PORT_MIRROR_H
#define PORT_MIRROR_H

#include <stdbool.h>
#include <stdint.h>

#include "config/default/library/tcpip/tcpip.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the console command group ('mirror'). Call once, after SYS_CMD is up. */
void MIRROR_Initialize(void);

/* Current state. */
bool MIRROR_IsEnabled(void);

/* Turn mirroring on or off without going through the console. */
void MIRROR_Set(bool enable);

/* RX path: call for every frame received on eth0, from the interface's packet
   handler. Does nothing when mirroring is off, and applies the own-MAC filter
   itself - the caller does not need to check anything. */
void MIRROR_Eth0Rx(struct _tag_TCPIP_MAC_PACKET *rxPkt);

/* TX path: called by the LAN865x driver for every frame about to leave on eth0.
 *
 * THE NAME OF THIS FUNCTION IS NOT FREE. The generated driver declares it with a
 * local `extern void mirror_eth0_tx_hook(TCPIP_MAC_PACKET *txPkt);` and calls it
 * directly - see DRV_LAN865X_PacketTx() in
 * src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c. That call is a
 * hand-patch to MCC-generated code: renaming this function breaks the link, and
 * re-running MCC code generation removes the call site. If a build suddenly
 * mirrors nothing on the TX side, check that the patch is still in place. */
void mirror_eth0_tx_hook(struct _tag_TCPIP_MAC_PACKET *txPkt);

/* RAW TX path: send a copy of a self-built eth0 frame out eth1 as well, so a PC
 * on the 100BASE-T side can capture it in Wireshark.
 *
 * WHY THIS EXISTS. There are TWO eth0 egress points, not one. Frames handed to
 * DRV_LAN865X_SendRawEthFrame() go straight into TC6_SendRawEthernetPacket()
 * and never pass DRV_LAN865X_PacketTx(), so mirror_eth0_tx_hook() above never
 * sees them - and the MAC bridge cannot help either, because it only forwards
 * frames it RECEIVES on a port, and a self-generated frame is never received.
 * Without this call such frames are invisible on eth1 even with the mirror on.
 * Raw senders need that path whenever they need the tsc flag (hardware TX
 * timestamp), so it is not something a caller can simply avoid.
 *
 * Gated by the SAME "mirror [on|off]" switch as the other two paths - deliberately
 * one switch for all of eth0, not a second per-sender flag. Two flags in series
 * would mean an empty capture with nothing actually broken, the expensive kind
 * of test failure.
 *
 * No MAC filter is applied, unlike the other two entry points: the caller built
 * the frame, so it is bridge-originated by definition. Pass the complete
 * Ethernet frame as handed to the driver, including the 14-byte header.
 * Best-effort like the other paths: a busy packet pool costs a gap in the
 * capture, never a stalled send. */
void MIRROR_RawTx(const uint8_t *frame, uint16_t flen);

#ifdef __cplusplus
}
#endif

#endif /* PORT_MIRROR_H */
