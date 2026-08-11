/*******************************************************************************
  eth0 (10BASE-T1S) -> eth1 (100BASE-T) port mirror / SPAN

  File Name:
    port_mirror.c

  Summary:
    Implementation of the port mirror described in port_mirror.h.

  Description:
    All three entry points funnel into mirror_ethpkt_to_eth1(), which allocates a
    stack packet, copies the complete Ethernet frame into it and hands it to the
    GMAC. The own-MAC filters live in the entry points, because the two
    stack-borne directions look at opposite ends of the frame: the RX path
    compares the DESTINATION MAC (frames addressed to the bridge), the TX path
    the SOURCE MAC (frames the bridge originated). MIRROR_RawTx() needs no filter
    at all - its caller built the frame.

    Mirror copies are best-effort: if the packet pool is busy the copy is dropped
    rather than blocking or retrying. A dropped mirror frame costs a gap in the
    capture, never a stalled data path.
 *******************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>                                          /* strtoul() */
#include <string.h>                                          /* memcpy/memcmp */

#include "definitions.h"
#include "config/default/system/console/sys_console.h"
#include "config/default/library/tcpip/tcpip.h"
#define TCPIP_THIS_MODULE_ID    TCPIP_MODULE_MANAGER
#include "config/default/library/tcpip/src/tcpip_packet.h"
#include "config/default/driver/gmac/drv_gmac.h"
#include "system/command/sys_command.h"
#include "tcpip_manager_control.h"                           /* TCPIP_NET_IF */
#include "port_mirror.h"

/* Interface indices: source of the mirrored traffic, and where the copies go. */
#define MIRROR_SRC_IF   0u    /* eth0, the 10BASE-T1S MAC-PHY */
#define MIRROR_DST_IF   1u    /* eth1, the 100BASE-T GMAC     */

#define MIRROR_MAX_FRAME  1518u

static bool s_mirror_on = false;

bool MIRROR_IsEnabled(void)   { return s_mirror_on; }
void MIRROR_Set(bool enable)  { s_mirror_on = enable; }

static void mirror_pkt_ack(TCPIP_MAC_PACKET *pkt, const void *param)
{
    (void)param;
    TCPIP_PKT_PacketFree(pkt);
}

/* eth0 (T1S) interface MAC - the filter reference for both mirror directions. */
static const uint8_t *eth0_own_mac(void)
{
    TCPIP_NET_HANDLE eth0 = TCPIP_STACK_IndexToNet(MIRROR_SRC_IF);
    return (eth0 != NULL) ? TCPIP_STACK_NetAddressMac(eth0) : NULL;
}

/* Clone a complete Ethernet frame onto eth1 for the PC-side capture. The caller
 * has already applied the own-MAC filter. Single-segment copy (bridge/stack
 * frames are single-segment); empty/oversize frames are dropped. */
static void mirror_ethpkt_to_eth1(const uint8_t *frame, uint16_t flen)
{
    TCPIP_MAC_PACKET *pTx;
    TCPIP_NET_HANDLE  eth1;

    if (frame == NULL || flen == 0u || flen > MIRROR_MAX_FRAME) return;
    pTx = TCPIP_PKT_PacketAlloc(sizeof(TCPIP_MAC_PACKET), flen, 0);   /* flags=0: same as the MAC bridge's own fwd alloc */
    if (pTx == NULL) return;                         /* packet pool busy: drop the mirror copy */

    pTx->pMacLayer = pTx->pDSeg->segLoad;
    memcpy(pTx->pMacLayer, frame, flen);             /* full Ethernet frame (header + payload) */
    pTx->pDSeg->segLen = flen;
    pTx->pNetLayer = pTx->pMacLayer + sizeof(TCPIP_MAC_ETHERNET_HEADER);
    pTx->ackFunc   = mirror_pkt_ack;                 /* freed by the MAC driver after TX */
    pTx->ackParam  = NULL;

    eth1 = TCPIP_STACK_IndexToNet(MIRROR_DST_IF);
    if (eth1 != NULL) {
        (void)DRV_GMAC_PacketTx(((TCPIP_NET_IF*)eth1)->hIfMac, pTx);
    } else {
        TCPIP_PKT_PacketFree(pTx);
    }
}

/* RX mirror: a frame just arrived on eth0. Mirror it only if it is addressed to
 * the bridge itself (dst MAC == eth0 MAC). PC-bound unicast and broadcast/
 * multicast are forwarded to eth1 by the MAC bridge already - mirroring them
 * would duplicate them at the PC. */
void MIRROR_Eth0Rx(struct _tag_TCPIP_MAC_PACKET *rxPkt)
{
    const uint8_t *frame;
    const uint8_t *mac;

    if (!s_mirror_on) return;
    if (rxPkt == NULL || rxPkt->pDSeg == NULL) return;
    frame = rxPkt->pMacLayer;
    mac   = eth0_own_mac();
    if (mac == NULL || frame == NULL) return;
    if (memcmp(frame, mac, 6) != 0) return;          /* dst MAC != eth0 -> not for us, skip */
    mirror_ethpkt_to_eth1(frame, rxPkt->pDSeg->segLen);
}

/* RAW TX mirror: a module built this frame itself and sent it through
 * DRV_LAN865X_SendRawEthFrame(), which bypasses DRV_LAN865X_PacketTx() and thus
 * the hook below. No own-MAC filter: the caller originated the frame. See the
 * long comment in port_mirror.h for why this second entry point is needed. */
void MIRROR_RawTx(const uint8_t *frame, uint16_t flen)
{
    if (!s_mirror_on) return;
    mirror_ethpkt_to_eth1(frame, flen);
}

/* TX mirror: called from DRV_LAN865X_PacketTx for every frame about to leave on
 * eth0 THROUGH THE STACK. Not every eth0 egress passes here - raw sends go
 * around it, see MIRROR_RawTx() above. Mirror it only if the bridge ITSELF
 * originated it (src MAC == eth0 MAC) - the firmware's own ping/ARP.
 * Frames forwarded from eth1 keep their original (PC) src MAC and are skipped;
 * the PC already has them. The driver transmits from pDSeg->segLoad.
 *
 * Name fixed by the driver patch - see the comment in port_mirror.h. */
void mirror_eth0_tx_hook(struct _tag_TCPIP_MAC_PACKET *txPkt)
{
    const uint8_t *frame;
    const uint8_t *mac;
    if (!s_mirror_on) return;
    if (txPkt == NULL || txPkt->pDSeg == NULL) return;
    frame = txPkt->pDSeg->segLoad;
    if (frame == NULL) return;
    mac = eth0_own_mac();
    if (mac == NULL) return;
    if (memcmp(frame + 6, mac, 6) != 0) return;      /* src MAC != eth0 -> forwarded, skip mirror */
    mirror_ethpkt_to_eth1(frame, txPkt->pDSeg->segLen);
}

/* mirror [0|1] - copy every eth0 (T1S) RX frame out eth1 so a PC on eth1 can
 * sniff the T1S bus with Wireshark (e.g. the endpoint's replies to a firmware
 * CLI command). No argument shows the current state. */
static void cmd_mirror(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    if (argc >= 2) {
        s_mirror_on = (strtoul(argv[1], NULL, 0) != 0u);
    }
    SYS_CONSOLE_PRINT("eth0(T1S)->eth1 mirror: %s\n\r", s_mirror_on ? "ON" : "OFF");
    if (s_mirror_on) {
        SYS_CONSOLE_PRINT("  Capture on the PC (eth1) in Wireshark to see the T1S bus traffic:\n\r");
        SYS_CONSOLE_PRINT("  RX (endpoint -> bridge: replies/ARP) AND the bridge's own TX.\n\r");
    }
}

static const SYS_CMD_DESCRIPTOR mirror_cmd_tbl[] = {
    {"mirror", (SYS_CMD_FNC) cmd_mirror, ": mirror eth0(T1S) RX+TX to eth1 for Wireshark (mirror [0|1])"},
};

void MIRROR_Initialize(void) {
    if (!SYS_CMD_ADDGRP(mirror_cmd_tbl, (int)(sizeof mirror_cmd_tbl / sizeof *mirror_cmd_tbl),
                        "span", ": eth0->eth1 port mirror for Wireshark")) {
        SYS_CONSOLE_PRINT("MIRROR: SYS_CMD_ADDGRP failed\n\r");
    }
}
