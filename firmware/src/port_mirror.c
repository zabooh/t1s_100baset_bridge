/*******************************************************************************
  eth0 (10BASE-T1S) -> eth1 (100BASE-T) port mirror / SPAN

  File Name:
    port_mirror.c

  Summary:
    Implementation of the port mirror described in port_mirror.h.

  Description:
    Both mirror directions funnel into mirror_ethpkt_to_eth1(), which allocates a
    stack packet, copies the complete Ethernet frame into it and hands it to the
    GMAC. The own-MAC filters live in the two entry points, because the two
    directions look at opposite ends of the frame: the RX path compares the
    DESTINATION MAC (frames addressed to the bridge), the TX path the SOURCE MAC
    (frames the bridge originated).

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
#include "config/default/library/tcpip/src/link_list.h"      /* PROTECTED_SINGLE_LIST for the mirror pool */
#include "config/default/driver/gmac/drv_gmac.h"
#include "system/command/sys_command.h"
#include "tcpip_manager_control.h"                           /* TCPIP_NET_IF */
#include "port_mirror.h"
#include "env.h"                                             /* env_mirror(): persisted start state */

/* Interface indices: source of the mirrored traffic, and where the copies go. */
#define MIRROR_SRC_IF   0u    /* eth0, the 10BASE-T1S MAC-PHY */
#define MIRROR_DST_IF   1u    /* eth1, the 100BASE-T GMAC     */

#define MIRROR_MAX_FRAME  1518u

/* Own small, fixed-size packet pool instead of TCPIP_PKT_PacketAlloc()'ing
 * straight from the general TCPIP heap on every frame. Root-caused
 * empirically (FALLSTRICKE.md, 2026-08-27): at iperf-rate T1S traffic
 * (~500 fps) the shared heap gets exhausted by fragmentation long before its
 * nominal ~65 KB capacity is used up - many differently-sized, differently-
 * lived allocations (TCP, DHCP, ARP, ... and now mirror copies) compete for
 * the same heap, and TCPIP_PKT_PacketAlloc() returns NULL well before that
 * capacity is reached. A pool of same-sized, pre-allocated buffers cannot
 * fragment: freeing one always makes room for exactly one more allocation.
 * Same pattern already proven in this codebase for exactly this reason -
 * TCPIP_MAC_BRIDGE_PACKET_POOL_SIZE (8) for the bridge's own forwarding path,
 * and DRV_LAN865X_SetMacCtrlInfo()'s rxFreePackets for eth0 RX buffers. Sized
 * the same (8) as the bridge's own pool, which already handles this traffic
 * rate on the forwarding path today.
 *
 * TCPIP_MAC_PKT_FLAG_STATIC does NOT protect a packet from
 * TCPIP_PKT_PacketFree() - _TCPIP_PKT_PacketAllocInt() strips that flag
 * unconditionally at allocation time (tcpip_packet.c). The only actual
 * protection is discipline: never call TCPIP_PKT_PacketFree() on a pool
 * packet, only ever recycle it through mirror_pkt_ack() back onto
 * s_mirror_free_pkts - the same contract DRV_LAN865X_SetMacCtrlInfo()'s
 * _RxPacketAck() already relies on for its own static RX packets. */
#define MIRROR_POOL_SIZE  8u

static bool s_mirror_on = false;
static bool s_sniffer_on = false;
static PROTECTED_SINGLE_LIST s_mirror_free_pkts;

/* Debug counters - temporary, to find exactly where frames are lost between
 * "eth0 received it" and "eth1 transmitted it". Printed by 'mirror'/'sniffer'
 * with no argument. */
static uint32_t s_dbg_rx_hook_calls = 0u;    /* MIRROR_Eth0Rx() entered with mirror/sniffer on */
static uint32_t s_dbg_rx_passed_filter = 0u; /* survived the dest-MAC filter (or sniffer skipped it) */
static uint32_t s_dbg_pool_empty = 0u;       /* s_mirror_free_pkts was empty */
static uint32_t s_dbg_no_eth1 = 0u;          /* TCPIP_STACK_IndexToNet(MIRROR_DST_IF) was NULL */
static uint32_t s_dbg_tx_submitted = 0u;     /* handed to DRV_GMAC_PacketTx */

bool MIRROR_IsEnabled(void)   { return s_mirror_on; }
void MIRROR_Set(bool enable)  { s_mirror_on = enable; }

bool SNIFFER_IsEnabled(void)  { return s_sniffer_on; }
void SNIFFER_Set(bool enable) { s_sniffer_on = enable; }

/* Recycle a sent (or never-submitted) pool packet - never TCPIP_PKT_PacketFree()
 * it, see the pool comment above. */
static void mirror_pkt_ack(TCPIP_MAC_PACKET *pkt, const void *param)
{
    (void)param;
    TCPIP_Helper_ProtectedSingleListTailAdd(&s_mirror_free_pkts, (SGL_LIST_NODE*) pkt);
}

/* Pre-allocate the pool once, from the TCPIP heap while it still has room to
 * spare (called from MIRROR_Initialize(), well after TCPIP_STACK_Init() has
 * set the heap up - see initialization.c). A short pool at startup is not
 * fatal - mirror_ethpkt_to_eth1() already treats "pool empty" as a normal
 * drop, same as it always has. */
static void mirror_pool_init(void)
{
    uint8_t i;
    TCPIP_MAC_PACKET *pkt;

    (void)TCPIP_Helper_ProtectedSingleListInitialize(&s_mirror_free_pkts);
    for (i = 0u; i < MIRROR_POOL_SIZE; i++) {
        pkt = TCPIP_PKT_PacketAlloc(sizeof(TCPIP_MAC_PACKET), MIRROR_MAX_FRAME, TCPIP_MAC_PKT_FLAG_STATIC);
        if (pkt == NULL) {
            SYS_CONSOLE_PRINT("MIRROR: pool init got only %u/%u buffers\n\r", (unsigned)i, (unsigned)MIRROR_POOL_SIZE);
            break;
        }
        pkt->ackFunc = mirror_pkt_ack;
        pkt->ackParam = NULL;
        TCPIP_Helper_ProtectedSingleListTailAdd(&s_mirror_free_pkts, (SGL_LIST_NODE*) pkt);
    }
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
    pTx = (TCPIP_MAC_PACKET*) TCPIP_Helper_ProtectedSingleListHeadRemove(&s_mirror_free_pkts);
    if (pTx == NULL) { s_dbg_pool_empty++; return; }  /* pool empty right now: drop the mirror copy */

    pTx->pMacLayer = pTx->pDSeg->segLoad;
    memcpy(pTx->pMacLayer, frame, flen);             /* full Ethernet frame (header + payload) */
    pTx->pDSeg->segLen = flen;
    pTx->pNetLayer = pTx->pMacLayer + sizeof(TCPIP_MAC_ETHERNET_HEADER);
    pTx->ackFunc   = mirror_pkt_ack;                 /* recycled back onto the pool after TX, never freed */
    pTx->ackParam  = NULL;

    eth1 = TCPIP_STACK_IndexToNet(MIRROR_DST_IF);
    if (eth1 != NULL) {
        s_dbg_tx_submitted++;
        (void)DRV_GMAC_PacketTx(((TCPIP_NET_IF*)eth1)->hIfMac, pTx);
    } else {
        /* No eth1 right now - DRV_GMAC_PacketTx() never got it, so its ackFunc
         * never fires on its own; recycle it back to the pool ourselves. */
        s_dbg_no_eth1++;
        mirror_pkt_ack(pTx, NULL);
    }
}

/* RX mirror: a frame just arrived on eth0. With sniffer off (the original
 * behaviour), mirror it only if it is addressed to the bridge itself (dst MAC
 * == eth0 MAC) - PC-bound unicast and broadcast/multicast are forwarded to
 * eth1 by the MAC bridge already, so mirroring them would duplicate them at
 * the PC. With sniffer on, that destination filter is skipped entirely: every
 * frame eth0 receives is mirrored, including traffic between two OTHER nodes
 * that never involves the bridge - see the sniffer description in
 * port_mirror.h for why that traffic reaches this hook at all. */
void MIRROR_Eth0Rx(struct _tag_TCPIP_MAC_PACKET *rxPkt)
{
    const uint8_t *frame;
    const uint8_t *mac;

    if (!s_mirror_on && !s_sniffer_on) return;
    s_dbg_rx_hook_calls++;
    if (rxPkt == NULL || rxPkt->pDSeg == NULL) return;
    frame = rxPkt->pMacLayer;
    if (frame == NULL) return;
    if (!s_sniffer_on) {
        mac = eth0_own_mac();
        if (mac == NULL) return;
        if (memcmp(frame, mac, 6) != 0) return;      /* dst MAC != eth0 -> not for us, skip */
    }
    s_dbg_rx_passed_filter++;
    mirror_ethpkt_to_eth1(frame, rxPkt->pDSeg->segLen);
}

/* TX mirror: called from DRV_LAN865X_PacketTx (the single eth0 egress point) for
 * every frame about to leave on eth0. Mirror it only if the bridge ITSELF
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

/* Temporary debug helper - see the counter declarations above. */
static void mirror_print_dbg_counters(void) {
    SYS_CONSOLE_PRINT("  dbg: rx_hook=%lu passed_filter=%lu pool_empty=%lu no_eth1=%lu tx_submitted=%lu\n\r",
                       (unsigned long)s_dbg_rx_hook_calls, (unsigned long)s_dbg_rx_passed_filter,
                       (unsigned long)s_dbg_pool_empty, (unsigned long)s_dbg_no_eth1,
                       (unsigned long)s_dbg_tx_submitted);
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
    mirror_print_dbg_counters();
}

/* sniffer [0|1] - like 'mirror', but without the destination-MAC filter on RX:
 * every frame eth0 receives is copied to eth1, including traffic between two
 * OTHER nodes on the bus that never involves this bridge. No argument shows
 * the current state. */
static void cmd_sniffer(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    if (argc >= 2) {
        s_sniffer_on = (strtoul(argv[1], NULL, 0) != 0u);
    }
    SYS_CONSOLE_PRINT("eth0(T1S)->eth1 sniffer: %s\n\r", s_sniffer_on ? "ON" : "OFF");
    if (s_sniffer_on) {
        SYS_CONSOLE_PRINT("  Capture on the PC (eth1) in Wireshark to see ALL T1S bus traffic,\n\r");
        SYS_CONSOLE_PRINT("  including frames between other nodes that do not involve this bridge.\n\r");
    }
    mirror_print_dbg_counters();
}

static const SYS_CMD_DESCRIPTOR mirror_cmd_tbl[] = {
    {"mirror", (SYS_CMD_FNC) cmd_mirror, ": mirror eth0(T1S) RX+TX to eth1 for Wireshark (mirror [0|1])"},
    {"sniffer", (SYS_CMD_FNC) cmd_sniffer, ": mirror ALL eth0(T1S) RX to eth1, not just this bridge's own traffic (sniffer [0|1])"},
};

void MIRROR_Initialize(void) {
    mirror_pool_init();

    if (!SYS_CMD_ADDGRP(mirror_cmd_tbl, (int)(sizeof mirror_cmd_tbl / sizeof *mirror_cmd_tbl),
                        "span", ": eth0->eth1 port mirror for Wireshark")) {
        SYS_CONSOLE_PRINT("MIRROR: SYS_CMD_ADDGRP failed\n\r");
    }

    /* Persisted start state ('setenv mirror 1' + 'saveenv'). Safe to set this early:
     * both hooks bail out while the stack is not up, because eth0_own_mac() returns
     * NULL until the interface exists. ENV_Init() runs in SYS_Initialize, well before
     * APP_Initialize gets here, so the value is loaded by now. */
    s_mirror_on = env_mirror();
    if (s_mirror_on) {
        SYS_CONSOLE_PRINT("MIRROR: eth0(T1S)->eth1 mirror enabled from env\n\r");
    }
}
