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

    Both directions are mirrored, each filtered against the bridge's OWN eth0
    MAC so the capture stays duplicate-free:

      - RX (bus -> bridge): only frames addressed TO the bridge.
      - TX (bridge -> bus): only frames the bridge ITSELF originated.

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

#ifdef __cplusplus
}
#endif

#endif /* PORT_MIRROR_H */
