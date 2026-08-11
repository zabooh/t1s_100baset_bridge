/*******************************************************************************
  Raw Ethernet frame test on eth0, bypassing the TCP/IP stack

  File Name:
    noip_test.c

  Summary:
    Implementation of the NoIP raw-frame test described in noip_test.h.

  Description:
    The transmit path is deliberately blunt: one static 60-byte buffer, filled
    once per command with a broadcast destination, this interface's own MAC as
    source, EtherType 0x88B5 and a 0xAA fill, then handed to
    DRV_LAN865X_SendRawEthFrame() once per frame with only the sequence number
    changing. 60 bytes is the minimum legal Ethernet frame, so nothing pads it
    behind our back and what the oscilloscope shows is what is written here.

    The gap between frames is a busy-wait on the SYS_TIME counter rather than a
    timer callback. For a diagnostic command issued from the console that is the
    honest trade: it blocks the caller for the requested time, which is exactly
    what "send 20 frames 50 ms apart" is asking for, and it keeps this module
    free of any scheduler state.
 *******************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>                                          /* strtoul() */
#include <string.h>                                          /* memcpy/memset */

#include "definitions.h"
#include "config/default/system/console/sys_console.h"
#include "config/default/library/tcpip/tcpip.h"
#include "config/default/system/time/sys_time.h"
#include "config/default/driver/lan865x/drv_lan865x.h"
#include "system/command/sys_command.h"
#include "noip_test.h"

/* Interface the frames go out of: 0 = eth0, the 10BASE-T1S MAC-PHY. */
#define NOIP_IF          0u

#define NOIP_FRAME_LEN   60u     /* minimum legal Ethernet frame, so nothing pads it */
#define NOIP_MAX_COUNT   100u
#define NOIP_MAX_GAP_MS  1000u

static uint32_t s_tx_cnt = 0u;
static uint32_t s_rx_cnt = 0u;
static uint8_t  s_frame[NOIP_FRAME_LEN];

uint32_t NOIP_TxCount(void) { return s_tx_cnt; }
uint32_t NOIP_RxCount(void) { return s_rx_cnt; }

bool NOIP_IsNoIpFrame(uint16_t frameType) {
    return (frameType == (uint16_t)NOIP_ETHERTYPE);
}

uint32_t NOIP_CountRx(void) {
    s_rx_cnt++;
    return s_rx_cnt;
}

uint32_t NOIP_SeqFromFrame(const uint8_t *frame) {
    if (frame == NULL) {
        return 0u;
    }
    /* Sequence number occupies the first four payload bytes, i.e. right after
     * the 14-byte Ethernet header. */
    return ((uint32_t)frame[14] << 24) | ((uint32_t)frame[15] << 16)
         | ((uint32_t)frame[16] <<  8) |  (uint32_t)frame[17];
}

void NOIP_PrintRxLine(uint32_t index, uint32_t seq, const uint8_t *mac_src,
                      uint16_t length, uint64_t ts_ms) {
    static const uint8_t zero_mac[6] = {0};
    const uint8_t *m = (mac_src != NULL) ? mac_src : zero_mac;
    SYS_CONSOLE_PRINT("[NoIP-RX] #%u seq=%u from %02X:%02X:%02X:%02X:%02X:%02X len=%d ts=%llu ms\r\n",
                      (unsigned)index, (unsigned)seq,
                      m[0], m[1], m[2], m[3], m[4], m[5],
                      (int)length, (unsigned long long)ts_ms);
}

/* Blocking millisecond wait on the SYS_TIME counter - see the file header for
 * why a busy-wait is the right shape here. */
static void noip_wait_ms(uint32_t ms)
{
    uint64_t start = SYS_TIME_Counter64Get();
    uint64_t ticks = ((uint64_t)SYS_TIME_FrequencyGet() * (uint64_t)ms) / 1000ULL;
    while ((SYS_TIME_Counter64Get() - start) < ticks) {
    }
}

/* noip_send <n> [gap_ms]  - send N raw Ethernet frames (EtherType 0x88B5) on eth0/T1S */
static void cmd_noip_send(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv)
{
    uint32_t count = 5u;
    uint32_t gap_ms = 0u;
    if (argc >= 2) { count = (uint32_t)strtoul(argv[1], NULL, 10); }
    if (argc >= 3) { gap_ms = (uint32_t)strtoul(argv[2], NULL, 10); }
    if (count == 0u || count > NOIP_MAX_COUNT) {
        SYS_CONSOLE_PRINT("[NoIP] count must be 1..%u\r\n", (unsigned)NOIP_MAX_COUNT);
        return;
    }
    if (gap_ms > NOIP_MAX_GAP_MS) {
        SYS_CONSOLE_PRINT("[NoIP] gap_ms must be 0..%u\r\n", (unsigned)NOIP_MAX_GAP_MS);
        return;
    }

    SYS_CONSOLE_PRINT("[NoIP-TX] start count=%u gap_ms=%u\r\n", (unsigned)count, (unsigned)gap_ms);

    /* Get our MAC from the T1S interface (index 0 = eth0) */
    TCPIP_NET_HANDLE netH = TCPIP_STACK_IndexToNet(NOIP_IF);
    const uint8_t  *pMac  = TCPIP_STACK_NetAddressMac(netH);

    /* DST: Layer-2 broadcast */
    memset(&s_frame[0], 0xFFu, 6u);
    /* SRC: our MAC */
    if (pMac != NULL) { memcpy(&s_frame[6], pMac, 6u); }
    else              { memset(&s_frame[6], 0u,   6u); }
    /* EtherType 0x88B5 */
    s_frame[12] = (uint8_t)((NOIP_ETHERTYPE >> 8u) & 0xFFu);
    s_frame[13] = (uint8_t)( NOIP_ETHERTYPE        & 0xFFu);
    /* Payload: 4-byte sequence + 42-byte fill to reach the 60-byte minimum frame */
    memset(&s_frame[14], 0xAAu, NOIP_FRAME_LEN - 14u);

    uint32_t i;
    for (i = 0u; i < count; i++) {
        s_tx_cnt++;
        s_frame[14] = (uint8_t)((s_tx_cnt >> 24u) & 0xFFu);
        s_frame[15] = (uint8_t)((s_tx_cnt >> 16u) & 0xFFu);
        s_frame[16] = (uint8_t)((s_tx_cnt >>  8u) & 0xFFu);
        s_frame[17] = (uint8_t)( s_tx_cnt         & 0xFFu);
        if (!DRV_LAN865X_SendRawEthFrame(NOIP_IF, s_frame, (uint16_t)sizeof(s_frame), 0x00u, NULL, NULL)) {
            SYS_CONSOLE_PRINT("[NoIP-TX] send failed at seq=%u\r\n", (unsigned)s_tx_cnt);
            s_tx_cnt--;
            break;
        }
        SYS_CONSOLE_PRINT("[NoIP-TX] sent seq=%u\r\n", (unsigned)s_tx_cnt);
        if (gap_ms > 0u) {
            noip_wait_ms(gap_ms);
        }
    }
}

static void cmd_noip_stat(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv)
{
    SYS_CONSOLE_PRINT("[NoIP] TX=%u  RX=%u\r\n", (unsigned)s_tx_cnt, (unsigned)s_rx_cnt);
}

static const SYS_CMD_DESCRIPTOR noip_cmd_tbl[] = {
    {"noip_send", (SYS_CMD_FNC) cmd_noip_send, ": send N raw Ethernet frames bypassing TCP stack (noip_send <n> [gap_ms])"},
    {"noip_stat", (SYS_CMD_FNC) cmd_noip_stat, ": show NoIP TX/RX counters"},
};

void NOIP_Initialize(void) {
    if (!SYS_CMD_ADDGRP(noip_cmd_tbl, (int)(sizeof noip_cmd_tbl / sizeof *noip_cmd_tbl),
                        "noip", ": raw Ethernet frame test (EtherType 0x88B5)")) {
        SYS_CONSOLE_PRINT("NOIP: SYS_CMD_ADDGRP failed\n\r");
    }
}
