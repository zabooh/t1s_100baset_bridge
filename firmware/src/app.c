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
#include "ptp_ts_ipc.h"
#include "ptp_bridge_task.h"
#include "ptp_gm_task.h"
#include "config/default/system/console/sys_console.h"
#include "config/default/library/tcpip/tcpip.h"
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
SYS_TIME_HANDLE timerHandle;
static SYS_TIME_HANDLE gmTimerHandle = SYS_TIME_HANDLE_INVALID;

// LAN865X Register access variables
volatile bool lan_reg_operation_complete = false;
volatile bool lan_reg_operation_success = false;
volatile uint32_t lan_reg_read_value = 0;

/* TODO:  Add any necessary local functions.
 */


void BRIDGE_TimerCallback(uintptr_t context) {
    if (my_delay_time)my_delay_time--;
}

static void GM_TimerCallback(uintptr_t context) {
    if (PTP_Bridge_GetMode() == PTP_MASTER) {
        PTP_GM_Service();
    }
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

    gmTimerHandle = SYS_TIME_TimerCreate(0, SYS_TIME_MSToCount(1), &GM_TimerCallback, (uintptr_t) NULL, SYS_TIME_PERIODIC);
    SYS_TIME_TimerStart(gmTimerHandle);

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
            PTP_Bridge_Init();
            appData.state = APP_STATE_IDLE;
            break;
        }

            /* TODO: implement your application state machine.*/
        case APP_STATE_IDLE:
            break;

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

    /* PTP frame (EtherType 0x88F7): feed into clock servo, do not forward to IP stack */
    if (frameType == 0x88F7u) {
        uint64_t rxTs = 0u;
        if (g_ptp_rx_ts.valid) {
            rxTs = g_ptp_rx_ts.rxTimestamp;
            g_ptp_rx_ts.valid = false;
        }
        PTP_Bridge_OnFrame(rxPkt->pMacLayer, (uint16_t)rxPkt->pDSeg->segLen, rxTs);
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
    
    TCPIP_MAC_RES result = DRV_LAN865X_ReadRegister(0, addr, false, lan_read_callback, NULL);
    
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


/* Reprogram PLCA node ID at runtime and re-enable PLCA.
 * Sequence: disable → set nodeId/nodeCount → re-enable.
 * nodeCount is kept from the build configuration (DRV_LAN865X_PLCA_NODE_COUNT_IDX0).
 *
 * The nodeId is also written persistently into the driver configuration struct
 * via DRV_LAN865X_SetPlcaNodeId() so that it survives an automatic driver
 * re-initialisation triggered by a "Loss of Framing Error" (STATUS0 bit 4).
 */
static void plca_set_node(uint8_t nodeId)
{
    /* 0. Persist nodeId so driver re-init uses the correct value */
    DRV_LAN865X_SetPlcaNodeId(0u, nodeId);
    /* 1. Disable PLCA */
    DRV_LAN865X_WriteRegister(0u, 0x0004CA01u /* PLCA_CONTROL_0 */, 0u, true, NULL, NULL);
    /* 2. Write new nodeId + nodeCount */
    uint32_t ctrl1 = ((uint32_t)DRV_LAN865X_PLCA_NODE_COUNT_IDX0 << 8u) | nodeId;
    DRV_LAN865X_WriteRegister(0u, 0x0004CA02u /* PLCA_CONTROL_1 */, ctrl1, true, NULL, NULL);
    /* 3. Re-enable PLCA */
    DRV_LAN865X_WriteRegister(0u, 0x0004CA01u /* PLCA_CONTROL_0 */, (1u << 15u), true, NULL, NULL);
}

static void cmd_ptp_mode(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv) {
    if (argc != 2) {
        SYS_CONSOLE_PRINT("Usage: ptp_mode [off|follower|master]\r\n");
        return;
    }
    if (strcmp(argv[1], "off") == 0) {
        PTP_Bridge_SetMode(PTP_DISABLED);
        SYS_CONSOLE_PRINT("[PTP] disabled\r\n");
    } else if (strcmp(argv[1], "follower") == 0) {
        plca_set_node(1u);
        PTP_Bridge_SetMode(PTP_SLAVE);
        SYS_CONSOLE_PRINT("[PTP] follower mode (PLCA node 1)\r\n");
    } else if (strcmp(argv[1], "master") == 0) {
        plca_set_node(0u);
        PTP_GM_Init();
        PTP_Bridge_SetMode(PTP_MASTER);
        SYS_CONSOLE_PRINT("[PTP] grandmaster mode (PLCA node 0 / coordinator)\r\n");
    } else {
        SYS_CONSOLE_PRINT("Unknown mode: %s\r\n", argv[1]);
    }
}

static void cmd_ptp_status(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv) {
    const char *modeStr[] = {"disabled", "master", "slave"};
    uint32_t cnt = 0u, state = 0u;
    PTP_GM_GetStatus(&cnt, &state);
    SYS_CONSOLE_PRINT("[PTP] mode=%s gmSyncs=%u gmState=%u\r\n",
                       modeStr[PTP_Bridge_GetMode()],
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

static void cmd_ptp_offset(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv) {
    int64_t  off     = 0;
    uint64_t off_abs = 0;
    PTP_Bridge_GetOffset(&off, &off_abs);
    SYS_CONSOLE_PRINT("[PTP] offset=%lld ns  abs=%llu ns\r\n",
                      (long long)off, (unsigned long long)off_abs);
}

static void cmd_ptp_reset(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv) {
    PTP_Bridge_Reset();
    SYS_CONSOLE_PRINT("[PTP] follower servo reset to UNINIT\r\n");
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
    {"ptp_offset",   (SYS_CMD_FNC) cmd_ptp_offset,   ": show follower time offset [ns]"},
    {"ptp_reset",    (SYS_CMD_FNC) cmd_ptp_reset,    ": reset PTP follower servo to UNINIT"},
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
