/*******************************************************************************
  MPLAB Harmony Application Source File

  Company:
    Microchip Technology Inc.

  File Name:
    app.c

  Summary:
    This file contains the source code for the MPLAB Harmony application.

  Description:
    This file contains the source code for the MPLAB Harmony application.  It
    implements the logic of the application's state machine and it may call
    API routines of other MPLAB Harmony modules in the system, such as drivers,
    system services, and middleware.  However, it does not call any of the
    system interfaces (such as the "Initialize" and "Tasks" functions) of any of
    the modules in the system or make any assumptions about when those functions
    are called.  That is the responsibility of the configuration-specific system
    files.
 *******************************************************************************/

// *****************************************************************************
// *****************************************************************************
// Section: Included Files
// *****************************************************************************
// *****************************************************************************

#include "app.h"
#include <string.h>
#include "ptp_ts_ipc.h"
#include "PTP_FOL_task.h"
#include "ptp_gm_task.h"
#include "config/default/system/console/sys_console.h"
#include "config/default/library/tcpip/tcpip.h"
#define TCPIP_THIS_MODULE_ID    TCPIP_MODULE_MANAGER
#include "config/default/library/tcpip/src/tcpip_packet.h"
#include "config/default/library/tcpip/telnet.h"
#include "config/default/system/time/sys_time.h"
#include "config/default/driver/gmac/drv_gmac.h"
#include "config/default/driver/lan865x/drv_lan865x.h"
#include "system/command/sys_command.h"
#include "tcpip_manager_control.h"


// *****************************************************************************
// *****************************************************************************
// Section: Global Data Definitions
// *****************************************************************************
// *****************************************************************************

// *****************************************************************************
/* Application Data

  Summary:
    Holds application data

  Description:
    This structure holds the application's data.

  Remarks:
    This structure should be initialized by the APP_Initialize function.

    Application strings and buffers are be defined outside this structure.
 */

APP_DATA appData;

// *****************************************************************************
// *****************************************************************************
// Section: Application Callback Functions
// *****************************************************************************
// *****************************************************************************

/* TODO:  Add any necessary callback functions.
 */

// *****************************************************************************
// *****************************************************************************
// Section: Application Local Functions
// *****************************************************************************
// *****************************************************************************
bool pktEth0Handler(TCPIP_NET_HANDLE hNet, struct _tag_TCPIP_MAC_PACKET* rxPkt, uint16_t frameType, const void* hParam);
const void *MyEth0HandlerParam;

bool pktEth1Handler(TCPIP_NET_HANDLE hNet, struct _tag_TCPIP_MAC_PACKET* rxPkt, uint16_t frameType, const void* hParam);
const void *MyEth1HandlerParam;

void DumpMem(uint32_t addr, uint32_t count);
bool Command_Init(void);

uint32_t ipdump_mode = 0;
uint32_t fwd_mode = 0;
uint32_t my_delay_time = 0;

/* --- NoIP raw Ethernet test (EtherType 0x88B5 = IEEE 802 Local Experimental) --- */
#define NOIP_ETHERTYPE  0x88B5u
static uint32_t noip_tx_cnt = 0u;
static uint32_t noip_rx_cnt = 0u;
SYS_TIME_HANDLE timerHandle;

/* Maximum expected PTP frame size on wire.
 * Sync/FollowUp messages are ≤ 76 bytes; Announce up to ~90 bytes.
 * 128 bytes gives comfortable headroom for all standard PTP message types. */
#define PTP_MAX_FRAME_SIZE  128u

/* Buffer for a single pending PTP frame received in pktEth0Handler */
typedef struct {
    uint8_t  data[PTP_MAX_FRAME_SIZE]; /* buffer for PTP frame payload             */
    uint16_t length;                   /* frame length in bytes                    */
    uint64_t rxTimestamp;              /* hardware RX timestamp from LAN865x       */
    bool     pending;                  /* true when a frame is waiting to be read  */
} PTP_FRAME_BUFFER;

static PTP_FRAME_BUFFER ptp_rx_buffer = {0};

static void app_wait_ms(uint32_t ms)
{
    uint64_t start = SYS_TIME_Counter64Get();
    uint64_t ticks = ((uint64_t)SYS_TIME_FrequencyGet() * (uint64_t)ms) / 1000ULL;
    while ((SYS_TIME_Counter64Get() - start) < ticks) {
    }
}

/* Track LAN865x driver ready state to detect reinit-complete while in GM mode */
static bool lan865x_prev_ready = false;

// LAN865X Register access variables
volatile bool lan_reg_operation_complete = false;
volatile bool lan_reg_operation_success = false;
volatile uint32_t lan_reg_read_value = 0;

/* TODO:  Add any necessary local functions.
 */


void BRIDGE_TimerCallback(uintptr_t context) {
    if (my_delay_time)my_delay_time--;
}

// LAN865X Register callback for read operations
void lan_read_callback(void *reserved1, bool success, uint32_t addr, uint32_t value, void *pTag, void *reserved2) {
    lan_reg_operation_success = success;
    lan_reg_read_value = value;
    lan_reg_operation_complete = true;
    
    if (success) {
        SYS_CONSOLE_PRINT("LAN865X Read: Addr=0x%08X Value=0x%08X\n\r", (unsigned int)addr, (unsigned int)value);
    } else {
        SYS_CONSOLE_PRINT("LAN865X Read failed for addr=0x%08X\n\r", (unsigned int)addr);
    }
}

// LAN865X Register callback for write operations
void lan_write_callback(void *reserved1, bool success, uint32_t addr, uint32_t value, void *pTag, void *reserved2) {
    lan_reg_operation_success = success;
    lan_reg_operation_complete = true;
    
    if (success) {
        SYS_CONSOLE_PRINT("LAN865X Write: Addr=0x%08X Value=0x%08X - OK\n\r", (unsigned int)addr, (unsigned int)value);
    } else {
        SYS_CONSOLE_PRINT("LAN865X Write failed: Addr=0x%08X Value=0x%08X\n\r", (unsigned int)addr, (unsigned int)value);
    }
}

// Help command for Test group
static void test_help(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    SYS_CONSOLE_PRINT("Test group commands:\n\r");
    SYS_CONSOLE_PRINT("  help           - Show this help\n\r");
    SYS_CONSOLE_PRINT("  timestamp      - Show build timestamp\n\r");
    SYS_CONSOLE_PRINT("  ipdump <mode>  - Enable IP packet dumping (0=off, 1=eth0, 2=eth1, 3=both)\n\r");
    SYS_CONSOLE_PRINT("  fwd <mode>     - Set forwarding mode (0=off, 1=on)\n\r");
    SYS_CONSOLE_PRINT("  stats          - Show TX/RX software counters for eth0 and eth1\n\r");
    SYS_CONSOLE_PRINT("  lan_read <addr> - Read LAN865X register (hex address)\n\r");
    SYS_CONSOLE_PRINT("  lan_write <addr> <value> - Write LAN865X register (hex addr, hex value)\n\r");
    SYS_CONSOLE_PRINT("  noip_send <n> [gap_ms] - Send N raw Ethernet frames (EtherType 0x88B5) on T1S\n\r");
    SYS_CONSOLE_PRINT("  noip_stat      - Show NoIP TX/RX counters\n\r");
    SYS_CONSOLE_PRINT("  dump <addr> <count> - Dump memory (hex addr, decimal or hex count)\n\r");
    SYS_CONSOLE_PRINT("\n\rExample: Test lan_read 0x00000004\n\r");
    SYS_CONSOLE_PRINT("Example: Test dump 0x20000000 64\n\r");
}

// stats command: print TX/RX software counters for both interfaces
static void cmd_stats(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    TCPIP_MAC_RX_STATISTICS rxStats;
    TCPIP_MAC_TX_STATISTICS txStats;
    const char *ifNames[] = {"eth0", "eth1"};
    int i;
    for (i = 0; i < 2; i++) {
        TCPIP_NET_HANDLE netH = TCPIP_STACK_NetHandleGet(ifNames[i]);
        if (netH == NULL) {
            SYS_CONSOLE_PRINT("%s: not found\n\r", ifNames[i]);
            continue;
        }
        if (TCPIP_STACK_NetMACStatisticsGet(netH, &rxStats, &txStats)) {
            SYS_CONSOLE_PRINT("%s TX: ok=%d err=%d qFull=%d pend=%d\n\r",
                ifNames[i], txStats.nTxOkPackets, txStats.nTxErrorPackets,
                txStats.nTxQueueFull, txStats.nTxPendBuffers);
            SYS_CONSOLE_PRINT("%s RX: ok=%d err=%d nobufs=%d pend=%d\n\r",
                ifNames[i], rxStats.nRxOkPackets, rxStats.nRxErrorPackets,
                rxStats.nRxBuffNotAvailable, rxStats.nRxPendBuffers);
        } else {
            SYS_CONSOLE_PRINT("%s: stats not available\n\r", ifNames[i]);
        }
    }
}

// Timestamp command to show build info
static void show_timestamp(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    SYS_CONSOLE_PRINT("======================================\n\r");
    SYS_CONSOLE_PRINT("T1S Packet Sniffer - Build Info\n\r");
    SYS_CONSOLE_PRINT("Build Timestamp: "__DATE__" "__TIME__"\n\r");
    SYS_CONSOLE_PRINT("======================================\n\r");
}

bool TelnetAuthenticationHandler(const char* user, const char* password, const TCPIP_TELNET_CONN_INFO* pInfo, const void* hParam) {

    if ((strcmp(user, "admin") == 0) && (strcmp(password, "password") == 0)) {
        SYS_CONSOLE_PRINT("Telnet Access Authenticated\n\r");
        return true;
    } else {
        SYS_CONSOLE_PRINT("Telnet Access Declined\n\r");
        return false;
    }
}

const void* TelnetHandlerParam;

// *****************************************************************************
// *****************************************************************************
// Section: Application Initialization and State Machine Functions
// *****************************************************************************
// *****************************************************************************

/*******************************************************************************
  Function:
    void APP_Initialize ( void )

  Remarks:
    See prototype in app.h.
 */

void APP_Initialize(void) {
    /* Place the App state machine in its initial state. */
    appData.state = APP_STATE_INIT;

    TCPIP_TELNET_AuthenticationRegister(TelnetAuthenticationHandler, &TelnetHandlerParam);

    timerHandle = SYS_TIME_TimerCreate(0, SYS_TIME_MSToCount(1000), &BRIDGE_TimerCallback, (uintptr_t) NULL, SYS_TIME_PERIODIC);
    SYS_TIME_TimerStart(timerHandle);

    Command_Init();
    /* TODO: Initialize your application's state machine and other
     * parameters.
     */
}

/******************************************************************************
  Function:
    void APP_Tasks ( void )

  Remarks:
    See prototype in app.h.
 */

void APP_Tasks(void) {

    /* Check the application's current state. */
    switch (appData.state) {
            /* Application's initial state. */
        case APP_STATE_INIT:
        {
            bool appInitialized = true;

            my_delay_time = 5;
            if (appInitialized) {

                appData.state = APP_STATE_WAIT;
            }
            break;
        }

        case APP_STATE_WAIT:
            if (my_delay_time == 0) {
                appData.state = APP_STATE_SERVICE_TASKS;
            }
            break;

        case APP_STATE_SERVICE_TASKS:
        {
            SYS_CONSOLE_PRINT("======================================\n\r ");
            SYS_CONSOLE_PRINT("T1S Packet Sniffer\n\r ");
            SYS_CONSOLE_PRINT("Build Timestamp: "__DATE__" "__TIME__"\n\r" );
            TCPIP_NET_HANDLE eth0_net_hd = TCPIP_STACK_IndexToNet(0);
            TCPIP_STACK_PacketHandlerRegister(eth0_net_hd, pktEth0Handler, MyEth0HandlerParam);
            TCPIP_NET_HANDLE eth1_net_hd = TCPIP_STACK_IndexToNet(1);
            TCPIP_STACK_PacketHandlerRegister(eth1_net_hd, pktEth1Handler, MyEth1HandlerParam);
            PTP_FOL_Init();
            appData.state = APP_STATE_IDLE;
            break;
        }

            /* TODO: implement your application state machine.*/
        case APP_STATE_IDLE:
        {
            /* === GM Service: call PTP_GM_Service() every 1 ms === */
            static uint64_t last_gm_tick  = 0u;
            static uint64_t last_fol_tick = 0u;
            static uint64_t ticks_per_ms  = 0u;
            if (ticks_per_ms == 0u) {
                ticks_per_ms = (uint64_t)SYS_TIME_FrequencyGet() / 1000ULL;
            }
            uint64_t current_tick = SYS_TIME_Counter64Get();

            if (PTP_FOL_GetMode() == PTP_MASTER) {
                if ((current_tick - last_gm_tick) >= ticks_per_ms) {
                    PTP_GM_Service();
                    last_gm_tick = current_tick;
                }
            }

            if (PTP_FOL_GetMode() == PTP_SLAVE) {
                if ((current_tick - last_fol_tick) >= ticks_per_ms) {
                    PTP_FOL_Service();
                    last_fol_tick = current_tick;
                }
            }

            /* === FOL Service: process a buffered PTP frame ===
             * ptp_rx_buffer.pending is set by pktEth0Handler() and cleared here.
             * On ARM Cortex-M, aligned bool writes are single-instruction atomic,
             * so no explicit critical-section is needed for this flag.
             * All standard PTP message types (Sync, FollowUp, Announce, Delay_Req)
             * are buffered here; stale frames of any type are overwritten by the
             * next arrival, which is acceptable because PTP frames arrive at a
             * maximum rate of once every 125 ms and APP_Tasks() runs far more
             * frequently than that. */
            if (PTP_FOL_GetMode() == PTP_SLAVE && ptp_rx_buffer.pending) {
                ptp_rx_buffer.pending = false;
                PTP_FOL_OnFrame(ptp_rx_buffer.data,
                                ptp_rx_buffer.length,
                                ptp_rx_buffer.rxTimestamp);
            }

            /* Re-run PTP_GM_Init() if the LAN865x driver recovers from a
             * reinit (triggered by TC6Error_SyncLost / LOFE) while in GM mode.
             * The reinit clears all TX-Match registers written by PTP_GM_Init(),
             * so they must be reprogrammed once the driver is READY again. */
            bool lan865x_ready = DRV_LAN865X_IsReady(0u);
            if (!lan865x_prev_ready && lan865x_ready &&
                (PTP_FOL_GetMode() == PTP_MASTER))
            {
                SYS_CONSOLE_PRINT("[PTP-GM] driver ready after reinit - re-applying TX-Match config\r\n");
                PTP_GM_Init();
            }
            lan865x_prev_ready = lan865x_ready;
            break;
        }

            /* The default state should never be executed. */
        default:
        {
            /* TODO: Handle error in application's state machine. */
            break;
        }
    }
}

bool pktEth0Handler(TCPIP_NET_HANDLE hNet, struct _tag_TCPIP_MAC_PACKET* rxPkt, uint16_t frameType, const void* hParam) {
    static uint32_t packet_counter = 0;
    uint8_t *puc_s;
    uint32_t data_len;
    bool ret_val = false;

    packet_counter++;

    /* NoIP raw test frame (EtherType 0x88B5): increment counter + print, free buffer */
    if (frameType == NOIP_ETHERTYPE) {
        noip_rx_cnt++;
        const uint8_t *p = rxPkt->pMacLayer;
        uint32_t seq = ((uint32_t)p[14] << 24) | ((uint32_t)p[15] << 16)
                     | ((uint32_t)p[16] <<  8) |  (uint32_t)p[17];
        SYS_CONSOLE_PRINT("[NoIP-RX] #%u seq=%u from %02X:%02X:%02X:%02X:%02X:%02X len=%d\r\n",
            (unsigned)noip_rx_cnt, (unsigned)seq,
            p[6], p[7], p[8], p[9], p[10], p[11],
            rxPkt->pDSeg->segLen);
        DumpMem((uint32_t)rxPkt->pMacLayer, rxPkt->pDSeg->segLen);
        TCPIP_PKT_PacketAcknowledge(rxPkt, TCPIP_MAC_PKT_ACK_RX_OK);
        return true;
    }

    /* PTP frame (EtherType 0x88F7): buffer for processing in APP_Tasks, do not forward to IP stack */
    if (frameType == 0x88F7u) {
        uint64_t rxTs = 0u;
        if (g_ptp_rx_ts.valid) {
            rxTs = g_ptp_rx_ts.rxTimestamp;
            g_ptp_rx_ts.valid = false;
        }
        if (ipdump_mode == 1u || ipdump_mode == 3u) {
            SYS_CONSOLE_PRINT("E0:PTP[0x88F7] len=%u ts=%llu\r\n",
                              (unsigned)rxPkt->pDSeg->segLen,
                              (unsigned long long)rxTs);
            DumpMem((uint32_t)rxPkt->pMacLayer, rxPkt->pDSeg->segLen);
        }
        /* Store the frame for later processing in APP_Tasks().
         * If a frame is already pending, the older (stale) frame is overwritten
         * because PTP Sync frames that are not processed promptly become irrelevant. */
        uint16_t copyLen = rxPkt->pDSeg->segLen;
        if (copyLen > (uint16_t)sizeof(ptp_rx_buffer.data)) {
            copyLen = (uint16_t)sizeof(ptp_rx_buffer.data);
        }
        memcpy(ptp_rx_buffer.data, rxPkt->pMacLayer, copyLen);
        ptp_rx_buffer.length      = copyLen;
        ptp_rx_buffer.rxTimestamp = rxTs;
        ptp_rx_buffer.pending     = true;
        TCPIP_PKT_PacketAcknowledge(rxPkt, TCPIP_MAC_PKT_ACK_RX_OK);
        return true;
    }

    if (ipdump_mode == 1 || ipdump_mode == 3) {
        SYS_CONSOLE_PRINT("E0:%d\n\r", packet_counter);

        puc_s = rxPkt->pMacLayer;
        data_len = rxPkt->pDSeg->segLen;
        DumpMem((uint32_t) puc_s, data_len);
    }

    if ( fwd_mode == 1) {
        /* Raw IP Packet received from eth0 (T1S) is send to eth1 (100BaseT)*/
        TCPIP_NET_HANDLE NetHdl = TCPIP_STACK_IndexToNet(1);
        DRV_GMAC_PacketTx(((TCPIP_NET_IF*) NetHdl)->hIfMac, rxPkt);
        ret_val = true;
    }

    /* a return value of true, tell the TCP stack not to handle this packet */
    return ret_val;
}

bool pktEth1Handler(TCPIP_NET_HANDLE hNet, struct _tag_TCPIP_MAC_PACKET* rxPkt, uint16_t frameType, const void* hParam) {
    static uint32_t packet_counter = 0;
    uint8_t *puc_s;
    uint32_t data_len;

    packet_counter++;

    if (ipdump_mode == 2 || ipdump_mode == 3) {
        SYS_CONSOLE_PRINT("E1:%d\n\r", packet_counter);

        puc_s = rxPkt->pDSeg->segLoad;
        data_len = rxPkt->pDSeg->segLen;
        DumpMem((uint32_t) puc_s, data_len);
    }
    return false;
}

void DumpMem(uint32_t addr, uint32_t count) {
    uint32_t ix, jx, kx;
    uint8_t *puc;
    char str[64];
    int flag = 0;

    puc = (uint8_t *) addr;
    puc = (uint8_t *) addr;

    jx = kx = 0;
    for (ix = 0; ix < count; ix++) {
        if ((ix % 16) == 0) {
            if (flag == 1) {
                str[16] = 0;
                kx = 0;
                SYS_CONSOLE_PRINT("   %s\n\r", str);
            }
            SYS_CONSOLE_PRINT("%08x: ", puc);
            flag = 1;
            jx = 0;
        }
        SYS_CONSOLE_PRINT(" %02x", *puc);
        kx++;
        if ((*puc > 31) && (*puc < 127))
            str[jx++] = *puc;
        else
            str[jx++] = '.';
        puc++;
    }
    for (; kx < 16; kx++) {
        SYS_CONSOLE_PRINT("   ");
    }
    str[jx] = 0;
    SYS_CONSOLE_PRINT("   %s", str);
    SYS_CONSOLE_PRINT("\n\r");
}

static void my_dump(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    //const void* cmdIoParam = pCmdIO->cmdIoParam;

    ipdump_mode = strtoul(argv[1], NULL, 16);
    if (ipdump_mode == 0) {
        SYS_CONSOLE_PRINT("IP Layer Dump de-activated\n\r");
    } else if (ipdump_mode == 1) {
        SYS_CONSOLE_PRINT("IP Layer Dump activated on eth0\n\r");
    } else if (ipdump_mode == 2) {
        SYS_CONSOLE_PRINT("IP Layer Dump activated on eth1\n\r");
    } else if (ipdump_mode == 3) {
        SYS_CONSOLE_PRINT("IP Layer Dump activated on eth0 and eth1\n\r");
    } else {
        SYS_CONSOLE_PRINT("Parameter out of range\n\r");
    }

}

static void my_fwd(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {

    fwd_mode = strtoul(argv[1], NULL, 16);
    if (fwd_mode == 0) {
        SYS_CONSOLE_PRINT("Forward mode set to off\n\r");
    } else if (fwd_mode == 1) {
        SYS_CONSOLE_PRINT("Forward mode set to on\n\r");
    }

}

// Memory dump command: dump <address> <count>
static void cmd_mem_dump(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    if (argc != 3) {
        SYS_CONSOLE_PRINT("Usage: dump <address_hex> <count>\n\r");
        SYS_CONSOLE_PRINT("Example: dump 0x20000000 64\n\r");
        return;
    }

    uint32_t addr  = strtoul(argv[1], NULL, 0);
    uint32_t count = strtoul(argv[2], NULL, 0);

    if (count == 0) {
        SYS_CONSOLE_PRINT("Count must be > 0\n\r");
        return;
    }

    SYS_CONSOLE_PRINT("Memory dump: 0x%08X  %u bytes\n\r", (unsigned int)addr, (unsigned int)count);
    DumpMem(addr, count);
}

// LAN865X Register read command
static void lan_read(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    if (argc != 2) {
        SYS_CONSOLE_PRINT("Usage: lan_read <address_hex>\n\r");
        SYS_CONSOLE_PRINT("Example: lan_read 0x00040000\n\r");
        return;
    }
    
    uint32_t addr = strtoul(argv[1], NULL, 0);  // Support both hex (0x) and decimal
    
    // Reset flags
    lan_reg_operation_complete = false;
    lan_reg_operation_success = false;
    lan_reg_read_value = 0;
    
    TCPIP_MAC_RES result = DRV_LAN865X_ReadRegister(0, addr, true, lan_read_callback, NULL);
    
    if (result == TCPIP_MAC_RES_OK) {
        SYS_CONSOLE_PRINT("LAN865X Read initiated for addr=0x%08X\n\r", (unsigned int)addr);
        // Result printed asynchronously by lan_read_callback
    } else {
        SYS_CONSOLE_PRINT("LAN865X Read failed to start: result=%d\n\r", result);
    }
}

// LAN865X Register write command  
static void lan_write(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    if (argc != 3) {
        SYS_CONSOLE_PRINT("Usage: lan_write <address_hex> <value_hex>\n\r");
        SYS_CONSOLE_PRINT("Example: lan_write 0x00040000 0x12345678\n\r");
        return;
    }
    
    uint32_t addr = strtoul(argv[1], NULL, 0);   // Support both hex (0x) and decimal
    uint32_t value = strtoul(argv[2], NULL, 0);  // Support both hex (0x) and decimal
    
    // Reset flags
    lan_reg_operation_complete = false;
    lan_reg_operation_success = false;
    
    TCPIP_MAC_RES result = DRV_LAN865X_WriteRegister(0, addr, value, true, lan_write_callback, NULL);
    
    if (result == TCPIP_MAC_RES_OK) {
        SYS_CONSOLE_PRINT("LAN865X Write initiated: addr=0x%08X value=0x%08X\n\r", (unsigned int)addr, (unsigned int)value);
        // Result printed asynchronously by lan_write_callback
    } else {
        SYS_CONSOLE_PRINT("LAN865X Write failed to start: result=%d\n\r", result);
    }
}


static void cmd_ptp_mode(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv) {
    if (argc != 2) {
        SYS_CONSOLE_PRINT("Usage: ptp_mode [off|follower|master]\r\n");
        return;
    }
    if (strcmp(argv[1], "off") == 0) {
        PTP_GM_Deinit();
        PTP_FOL_SetMode(PTP_DISABLED);
        SYS_CONSOLE_PRINT("[PTP] disabled\r\n");
    } else if (strcmp(argv[1], "follower") == 0) {
        PTP_FOL_SetMode(PTP_SLAVE);
        SYS_CONSOLE_PRINT("[PTP] follower mode (PLCA node %u)\r\n", (unsigned)DRV_LAN865X_PLCA_NODE_ID_IDX0);
    } else if (strcmp(argv[1], "master") == 0) {
        PTP_GM_Init();
        PTP_FOL_SetMode(PTP_MASTER);
        SYS_CONSOLE_PRINT("[PTP] grandmaster mode (PLCA node 0)\r\n");
    } else {
        SYS_CONSOLE_PRINT("Unknown mode: %s\r\n", argv[1]);
    }
}

static void cmd_ptp_status(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv) {
    const char *modeStr[] = {"disabled", "master", "slave"};
    uint32_t cnt = 0u, state = 0u;
    PTP_GM_GetStatus(&cnt, &state);
    SYS_CONSOLE_PRINT("[PTP] mode=%s gmSyncs=%u gmState=%u\r\n",
                       modeStr[PTP_FOL_GetMode()],
                       (unsigned)cnt, (unsigned)state);
}

static void cmd_ptp_interval(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv) {
    if (argc != 2) {
        SYS_CONSOLE_PRINT("Usage: ptp_interval <ms>\r\n");
        return;
    }
    uint32_t ms = (uint32_t)strtoul(argv[1], NULL, 10);
    PTP_GM_SetSyncInterval(ms);
    SYS_CONSOLE_PRINT("[PTP-GM] sync interval set to %u ms\r\n", (unsigned)ms);
}

static void cmd_ptp_dst(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv) {
    if (argc != 2) {
        ptp_gm_dst_mode_t mode = PTP_GM_GetDstMode();
        SYS_CONSOLE_PRINT("Usage: ptp_dst [multicast|broadcast]\r\n");
        SYS_CONSOLE_PRINT("[PTP-GM] dst=%s\r\n",
                          (mode == PTP_GM_DST_BROADCAST) ? "broadcast" : "multicast");
        return;
    }

    if (strcmp(argv[1], "multicast") == 0 || strcmp(argv[1], "mc") == 0) {
        PTP_GM_SetDstMode(PTP_GM_DST_MULTICAST);
        SYS_CONSOLE_PRINT("[PTP-GM] dst=multicast (01:80:C2:00:00:0E)\r\n");
    } else if (strcmp(argv[1], "broadcast") == 0 || strcmp(argv[1], "bc") == 0) {
        PTP_GM_SetDstMode(PTP_GM_DST_BROADCAST);
        SYS_CONSOLE_PRINT("[PTP-GM] dst=broadcast (FF:FF:FF:FF:FF:FF)\r\n");
    } else {
        SYS_CONSOLE_PRINT("Unknown dst mode: %s\r\n", argv[1]);
    }
}

static void cmd_ptp_offset(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv) {
    int64_t  off     = 0;
    uint64_t off_abs = 0;
    PTP_FOL_GetOffset(&off, &off_abs);
    SYS_CONSOLE_PRINT("[PTP] offset=%lld ns  abs=%llu ns\r\n",
                      (long long)off, (unsigned long long)off_abs);
}

static void cmd_ptp_reset(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv) {
    PTP_FOL_Reset();
    SYS_CONSOLE_PRINT("[PTP] follower servo reset to UNINIT\r\n");
}

uint8_t frame[60];

/* noip_send <n> [gap_ms]  — send N raw Ethernet frames (EtherType 0x88B5) on eth0/T1S */
static void cmd_noip_send(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv)
{
    uint32_t count = 5u;
    uint32_t gap_ms = 0u;
    if (argc >= 2) { count = (uint32_t)strtoul(argv[1], NULL, 10); }
    if (argc >= 3) { gap_ms = (uint32_t)strtoul(argv[2], NULL, 10); }
    if (count == 0u || count > 100u) {
        SYS_CONSOLE_PRINT("[NoIP] count must be 1..100\r\n");
        return;
    }
    if (gap_ms > 1000u) {
        SYS_CONSOLE_PRINT("[NoIP] gap_ms must be 0..1000\r\n");
        return;
    }

    SYS_CONSOLE_PRINT("[NoIP-TX] start count=%u gap_ms=%u\r\n", (unsigned)count, (unsigned)gap_ms);

    /* Get our MAC from the T1S interface (index 0 = eth0) */
    TCPIP_NET_HANDLE netH = TCPIP_STACK_IndexToNet(0);
    const uint8_t  *pMac  = TCPIP_STACK_NetAddressMac(netH);

    
    /* DST: Layer-2 broadcast */
    frame[0]=0xFFu; frame[1]=0xFFu; frame[2]=0xFFu;
    frame[3]=0xFFu; frame[4]=0xFFu; frame[5]=0xFFu;
    /* SRC: our MAC */
    if (pMac != NULL) { memcpy(&frame[6], pMac, 6u); }
    else              { memset(&frame[6], 0u,   6u); }
    /* EtherType 0x88B5 */
    frame[12] = (uint8_t)((NOIP_ETHERTYPE >> 8u) & 0xFFu);
    frame[13] = (uint8_t)( NOIP_ETHERTYPE        & 0xFFu);
    /* Payload: 4-byte sequence + 42-byte fill to reach 60-byte min frame */
    memset(&frame[14], 0xAAu, 46u);

    uint32_t i;
    for (i = 0u; i < count; i++) {
        noip_tx_cnt++;
        frame[14] = (uint8_t)((noip_tx_cnt >> 24u) & 0xFFu);
        frame[15] = (uint8_t)((noip_tx_cnt >> 16u) & 0xFFu);
        frame[16] = (uint8_t)((noip_tx_cnt >>  8u) & 0xFFu);
        frame[17] = (uint8_t)( noip_tx_cnt          & 0xFFu);
        if (!DRV_LAN865X_SendRawEthFrame(0u, frame, (uint16_t)sizeof(frame), 0x00u, NULL, NULL)) {
            SYS_CONSOLE_PRINT("[NoIP-TX] send failed at seq=%u\r\n", (unsigned)noip_tx_cnt);
            noip_tx_cnt--;
            break;
        }
        SYS_CONSOLE_PRINT("[NoIP-TX] sent seq=%u\r\n", (unsigned)noip_tx_cnt);
        if (gap_ms > 0u) {
            app_wait_ms(gap_ms);
        }
    }
}

static void cmd_noip_stat(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv)
{
    SYS_CONSOLE_PRINT("[NoIP] TX=%u  RX=%u\r\n", (unsigned)noip_tx_cnt, (unsigned)noip_rx_cnt);
}

static void cmd_ptp_regs(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv)
{
    (void)pCmdIO; (void)argc; (void)argv;
    /* Triggers a register dump from within PTP_GM_Service (GM_STATE_WAIT_PERIOD).
     * This avoids injecting TC6 control chunks during active PLCA traffic, which
     * would cause TC6Error_SyncLost (Loss of Framing Error). */
    PTP_GM_RequestRegDump();
    SYS_CONSOLE_PRINT("[PTP] reg dump requested — output follows after next WAIT_PERIOD\r\n");
}

const SYS_CMD_DESCRIPTOR msd_cmd_tbl[] = {
    {"help", (SYS_CMD_FNC) test_help, ": show Test group commands"},
    {"timestamp", (SYS_CMD_FNC) show_timestamp, ": show build timestamp"},
    {"ipdump", (SYS_CMD_FNC) my_dump, ": dump rx ip packets (0:off 1:eth0 2:eth1 3:both)"},
    {"fwd", (SYS_CMD_FNC) my_fwd, ": fwd (0:off 1:on default:on)"},
    {"stats", (SYS_CMD_FNC) cmd_stats, ": show TX/RX counters for eth0 and eth1"},
    {"lan_read", (SYS_CMD_FNC) lan_read, ": read LAN865X register (lan_read <addr_hex>)"},
    {"lan_write", (SYS_CMD_FNC) lan_write, ": write LAN865X register (lan_write <addr_hex> <value_hex>)"},
    {"dump", (SYS_CMD_FNC) cmd_mem_dump, ": dump memory (dump <addr_hex> <count>)"},
    {"ptp_mode",     (SYS_CMD_FNC) cmd_ptp_mode,     ": set PTP mode (off|follower|master) — master=PLCA node 0, follower=PLCA node 1"},
    {"ptp_status",   (SYS_CMD_FNC) cmd_ptp_status,   ": show PTP mode, sync count, offset"},
    {"ptp_interval", (SYS_CMD_FNC) cmd_ptp_interval, ": set GM Sync interval in ms (default 125)"},
    {"ptp_dst",      (SYS_CMD_FNC) cmd_ptp_dst,      ": set PTP DST MAC (multicast|broadcast)"},
    {"ptp_offset",   (SYS_CMD_FNC) cmd_ptp_offset,   ": show follower time offset [ns]"},
    {"ptp_reset",    (SYS_CMD_FNC) cmd_ptp_reset,    ": reset PTP follower servo to UNINIT"},
    {"ptp_regs",    (SYS_CMD_FNC) cmd_ptp_regs,    ": dump TX-Match registers via GM state machine (no SPI collision)"},
    {"noip_send",    (SYS_CMD_FNC) cmd_noip_send,    ": send N raw Ethernet frames bypassing TCP stack (noip_send <n> [gap_ms])"},
    {"noip_stat",    (SYS_CMD_FNC) cmd_noip_stat,    ": show NoIP TX/RX counters"},
};

bool Command_Init(void) {
    bool ret = true;  // Start with success

    if (!SYS_CMD_ADDGRP(msd_cmd_tbl, sizeof (msd_cmd_tbl) / sizeof (*msd_cmd_tbl), "Test", ": Test Commands")) {
        ret = false;  // If SYS_CMD_ADDGRP fails, return failure
    }
    return ret;
}


/*******************************************************************************
 End of File
 */
