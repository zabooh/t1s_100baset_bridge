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
#include <stdlib.h>           /* malloc/free - C-heap largest-free-block probe */
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
#include "env.h"
#include "lan865x_diag.h"
#include "noip_test.h"
#include "ptp_follower.h"

/* Banner printed once at start-up and by the 'timestamp' command. It names the
 * firmware and the role, because a demo runs two boards side by side and the
 * console is the only thing that tells them apart. */
#define APP_FW_NAME   "T1S PTP Follower"
#define APP_FW_ROLE   "single interface eth0 = 10BASE-T1S, wall clock steered from Sync/Follow_Up"
#define APP_FW_HELP   "help | lanhelp | ptpf status | stats | showenv"


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


void DumpMem(uint32_t addr, uint32_t count);
bool Command_Init(void);

uint32_t ipdump_mode = 0;
uint32_t my_delay_time = 0;

SYS_TIME_HANDLE timerHandle;

/* =========================================================
 * Deferred Packet Logging
 * =========================================================
 * Packet handlers store metadata into a ring buffer instead
 * of calling SYS_CONSOLE_PRINT()/DumpMem() directly.
 * APP_Tasks() drains the buffer (max 10 entries per call). */

#define PKT_LOG_BUF_SIZE    64u   /* ring buffer capacity; must be a power of 2 */
/* Full-frame capture: frame stored in shared pool (up to PKT_LOG_MAX_FRAME_SIZE bytes each) */
#define PKT_LOG_MAX_FRAMES     16u    /* number of full-size frames bufferable in pool */
#define PKT_LOG_MAX_FRAME_SIZE 1518u  /* max bytes per frame (standard Ethernet MTU)  */

typedef enum {
    PKT_LOG_NOIP = 0,  /* NoIP (0x88B5) frame from eth0 */
    PKT_LOG_ETH0 = 2,  /* generic frame from eth0        */
    PKT_LOG_ETH1 = 3,  /* generic frame from eth1        */
} pkt_log_type_t;

typedef struct {
    uint64_t       timestamp;    /* SYS_TIME_Counter64Get()                    */
    uint32_t       pkt_counter;  /* per-handler packet counter                 */
    uint32_t       noip_seq;     /* NoIP sequence number                       */
    uint16_t       frame_type;   /* EtherType                                  */
    uint16_t       length;       /* actual frame length in bytes               */
    uint32_t       data_offset;  /* offset into frame_data_pool[]              */
    uint16_t       data_len;     /* bytes stored in pool (may be 0 if dropped) */
    uint8_t        iface;        /* 0 = eth0, 1 = eth1                         */
    uint8_t        truncated;    /* 1 if frame data was truncated to fit pool  */
    pkt_log_type_t log_type;     /* entry classification                       */
    uint8_t        mac_src[6];   /* source MAC (extracted separately)          */
} PKT_LOG_ENTRY;

typedef struct {
    PKT_LOG_ENTRY     entries[PKT_LOG_BUF_SIZE];
    volatile uint32_t write_idx;     /* updated only by packet handlers  */
    volatile uint32_t read_idx;      /* updated only by APP_Tasks        */
    volatile uint32_t overflow_cnt;
    volatile uint32_t total_logged;
} PKT_LOG_BUF;

static PKT_LOG_BUF pkt_log = {0};

/* Shared circular pool for storing complete frame bytes.
 * Holds up to PKT_LOG_MAX_FRAMES full-size Ethernet frames.
 * Aligned to 4 bytes for efficient ARM word-aligned access. */
#define FRAME_DATA_POOL_SIZE  ((uint32_t)PKT_LOG_MAX_FRAMES * (uint32_t)PKT_LOG_MAX_FRAME_SIZE)

typedef struct {
    uint8_t  pool[FRAME_DATA_POOL_SIZE]; /* circular frame data storage           */
    uint32_t write_offset;               /* next write position in pool (0-based) */
} FRAME_DATA_POOL;

static FRAME_DATA_POOL frame_data_pool __attribute__((aligned(4))) = {0};

/* Lock-free single-producer/single-consumer ring buffer write.
 * On ARM Cortex-M, 32-bit aligned stores are single-instruction atomic.
 * write_idx is committed last so the reader never observes a partial entry.
 * Newest entries are dropped when the buffer is full.
 *
 * frame_data/frame_len provide the complete frame bytes to copy into the
 * shared pool.  The pool write_offset is advanced after the copy.
 * Wraparound safety: if the frame does not fit at the current write_offset
 * the function attempts to wrap to offset 0.  It only wraps if no pending
 * log entry references data in [0, copy_len), otherwise the frame is
 * truncated to the remaining bytes at the end of the pool.
 */
static void PktLog_Write(PKT_LOG_ENTRY *entry,
                         const uint8_t *frame_data, uint16_t frame_len)
{
    uint32_t next = (pkt_log.write_idx + 1u) & (PKT_LOG_BUF_SIZE - 1u);
    if (next == pkt_log.read_idx) {
        pkt_log.overflow_cnt++;
        return; /* ring buffer full – drop newest entry */
    }

    /* Clamp captured length to the maximum supported frame size */
    uint16_t copy_len = (frame_len > (uint16_t)PKT_LOG_MAX_FRAME_SIZE)
                        ? (uint16_t)PKT_LOG_MAX_FRAME_SIZE : frame_len;

    uint32_t pool_offset    = frame_data_pool.write_offset;
    uint8_t  truncated_flag = 0u;

    if (frame_data != NULL && copy_len > 0u) {
        uint32_t remaining = FRAME_DATA_POOL_SIZE - frame_data_pool.write_offset;

        if ((uint32_t)copy_len > remaining) {
            /* Frame does not fit at the current write position.
             * Attempt to wrap to the beginning of the pool.
             * This is safe only when no pending entry holds data in [0, copy_len). */
            bool ring_empty = (pkt_log.read_idx == pkt_log.write_idx);
            bool wrap_safe  = ring_empty ||
                              (pkt_log.entries[pkt_log.read_idx].data_offset >= (uint32_t)copy_len);

            if (wrap_safe) {
                /* Wrap: restart from pool beginning */
                pool_offset = 0u;
            } else {
                /* Cannot wrap safely – truncate to whatever space remains */
                copy_len       = (uint16_t)remaining;
                truncated_flag = 1u;
            }
        }

        if (copy_len > 0u) {
            memcpy(&frame_data_pool.pool[pool_offset], frame_data, copy_len);
            /* Advance the pool write pointer; reset to 0 if we exactly filled the end */
            uint32_t new_offset = pool_offset + (uint32_t)copy_len;
            frame_data_pool.write_offset = (new_offset >= FRAME_DATA_POOL_SIZE) ? 0u : new_offset;
        }
    }

    /* Store pool reference and flags in the ring entry */
    entry->data_offset = pool_offset;
    entry->data_len    = copy_len;
    entry->truncated   = truncated_flag;

    pkt_log.entries[pkt_log.write_idx] = *entry;
    pkt_log.total_logged++;
    pkt_log.write_idx = next; /* commit – must be the last store */
}

/* Read one entry from the ring buffer; returns false if empty. */
static bool PktLog_Read(PKT_LOG_ENTRY *entry)
{
    if (pkt_log.read_idx == pkt_log.write_idx) {
        return false; /* buffer empty */
    }
    *entry = pkt_log.entries[pkt_log.read_idx];
    pkt_log.read_idx = (pkt_log.read_idx + 1u) & (PKT_LOG_BUF_SIZE - 1u);
    return true;
}

/* LAN865x register access, transmitter test modes and PLCA live in their own,
 * self-contained module so they can be lifted into another project unchanged:
 * see lan865x_diag.c/.h. This file only calls LAN865X_DIAG_Initialize() and
 * LAN865X_DIAG_Tasks(). */

/* TODO:  Add any necessary local functions.
 */


void BRIDGE_TimerCallback(uintptr_t context) {
    if (my_delay_time)my_delay_time--;
}

// Help command for Test group
static void test_help(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    SYS_CONSOLE_PRINT("Test group commands:\n\r");
    SYS_CONSOLE_PRINT("  help                         - Show this help\n\r");
    SYS_CONSOLE_PRINT("  timestamp                    - Show build timestamp\n\r");
    SYS_CONSOLE_PRINT("  ipdump <mode>                - Dump RX IP packets (0=off, 1=eth0, 2=eth1, 3=both)\n\r");
    SYS_CONSOLE_PRINT("  stats                        - Show TX/RX software counters for eth0 and eth1\n\r");
    SYS_CONSOLE_PRINT("  dump <addr> <count>          - Dump memory (hex addr, count)\n\r");
    SYS_CONSOLE_PRINT("  logclear                     - Clear deferred packet log buffer\n\r");
    SYS_CONSOLE_PRINT("  logstat                      - Show deferred log statistics\n\r");
    SYS_CONSOLE_PRINT("\n\rLAN865x registers, test modes, PLCA: see 'lanhelp'\n\r");
}

// stats command: print TX/RX software counters for both interfaces
static void cmd_stats(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    TCPIP_MAC_RX_STATISTICS rxStats;
    TCPIP_MAC_TX_STATISTICS txStats;
    const char *ifNames[] = {"eth0"};      /* the follower has one interface */
    int i;
    for (i = 0; i < 1; i++) {
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
    SYS_CONSOLE_PRINT("======================================================\n\r");
    SYS_CONSOLE_PRINT(" %s\n\r", APP_FW_NAME);
    SYS_CONSOLE_PRINT(" %s\n\r", APP_FW_ROLE);
    SYS_CONSOLE_PRINT(" Build: "__DATE__" "__TIME__"\n\r");
    SYS_CONSOLE_PRINT("======================================================\n\r");
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
    LAN865X_DIAG_Initialize();
    NOIP_Initialize();
    PTP_FOL_Initialize();
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
            SYS_CONSOLE_PRINT("\n\r======================================================\n\r");
            SYS_CONSOLE_PRINT(" %s\n\r", APP_FW_NAME);
            SYS_CONSOLE_PRINT(" %s\n\r", APP_FW_ROLE);
            SYS_CONSOLE_PRINT(" Build: "__DATE__" "__TIME__"\n\r");
            SYS_CONSOLE_PRINT(" Commands: %s\n\r", APP_FW_HELP);
            SYS_CONSOLE_PRINT("======================================================\n\r");
            TCPIP_NET_HANDLE eth0_net_hd = TCPIP_STACK_IndexToNet(0);
            TCPIP_STACK_PacketHandlerRegister(eth0_net_hd, pktEth0Handler, MyEth0HandlerParam);
            env_apply();   /* push the persisted network config into the stack (once, stack is up) */
            appData.state = APP_STATE_IDLE;
            break;
        }

            /* TODO: implement your application state machine.*/
        case APP_STATE_IDLE:
        {
            static uint64_t ticks_per_ms  = 0u;
            if (ticks_per_ms == 0u) {
                ticks_per_ms = (uint64_t)SYS_TIME_FrequencyGet() / 1000ULL;
            }

            /* Register access / test modes / PLCA - see lan865x_diag.c */
            LAN865X_DIAG_Tasks();

            /* PTP follower: pair Sync/Follow_Up and measure - see ptp_follower.c */
            PTP_FOL_Tasks();

            /* === Deferred packet log output (max 10 entries per APP_Tasks iteration) === */
            if (ticks_per_ms > 0u) {
                PKT_LOG_ENTRY log_e;
                uint32_t max_print = 10u;
                while (max_print-- > 0u && PktLog_Read(&log_e)) {
                    uint64_t ts_ms = log_e.timestamp / ticks_per_ms;
                    switch (log_e.log_type) {
                        case PKT_LOG_NOIP:
                            NOIP_PrintRxLine(log_e.pkt_counter, log_e.noip_seq,
                                             log_e.mac_src, log_e.length, ts_ms);
                            if (log_e.data_len > 0u) {
                                DumpMem((uint32_t)&frame_data_pool.pool[log_e.data_offset], log_e.data_len);
                            }
                            break;
                        case PKT_LOG_ETH0:
                            SYS_CONSOLE_PRINT("E0:%u len=%u ts=%llu ms%s\r\n",
                                (unsigned)log_e.pkt_counter, (unsigned)log_e.length,
                                (unsigned long long)ts_ms,
                                log_e.truncated ? " [TRUNC]" : "");
                            if (log_e.data_len > 0u) {
                                DumpMem((uint32_t)&frame_data_pool.pool[log_e.data_offset], log_e.data_len);
                            }
                            break;
                        case PKT_LOG_ETH1:
                            SYS_CONSOLE_PRINT("E1:%u len=%u ts=%llu ms%s\r\n",
                                (unsigned)log_e.pkt_counter, (unsigned)log_e.length,
                                (unsigned long long)ts_ms,
                                log_e.truncated ? " [TRUNC]" : "");
                            if (log_e.data_len > 0u) {
                                DumpMem((uint32_t)&frame_data_pool.pool[log_e.data_offset], log_e.data_len);
                            }
                            break;
                        default:
                            break;
                    }
                }
            }
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

    packet_counter++;

    /* NoIP raw test frame: the module owns the EtherType, the frame layout and
     * the counters. The deferred log ring buffer stays here because ipdump shares
     * it, so the printing happens later in the drain loop (see PKT_LOG_NOIP). */
    if (NOIP_IsNoIpFrame(frameType)) {
        const uint8_t *p = rxPkt->pMacLayer;
        PKT_LOG_ENTRY log_e = {0};
        log_e.timestamp   = SYS_TIME_Counter64Get();
        log_e.pkt_counter = NOIP_CountRx();
        log_e.noip_seq    = NOIP_SeqFromFrame(p);
        log_e.frame_type  = frameType;
        log_e.length      = rxPkt->pDSeg->segLen;
        log_e.iface       = 0u;
        log_e.log_type    = PKT_LOG_NOIP;
        memcpy(log_e.mac_src, &p[6], 6u);
        PktLog_Write(&log_e, rxPkt->pMacLayer, rxPkt->pDSeg->segLen);
        TCPIP_PKT_PacketAcknowledge(rxPkt, TCPIP_MAC_PKT_ACK_RX_OK);
        return true;
    }

    if (ipdump_mode == 1 || ipdump_mode == 3) {
        PKT_LOG_ENTRY log_e = {0};
        log_e.timestamp   = SYS_TIME_Counter64Get();
        log_e.pkt_counter = packet_counter;
        log_e.frame_type  = frameType;
        log_e.length      = rxPkt->pDSeg->segLen;
        log_e.iface       = 0u;
        log_e.log_type    = PKT_LOG_ETH0;
        memcpy(log_e.mac_src, &rxPkt->pMacLayer[6], 6u);
        PktLog_Write(&log_e, rxPkt->pMacLayer, rxPkt->pDSeg->segLen);
    }

    /* eth0<->eth1 L2 bridging is done by the Harmony MAC bridge, not here.
     * Return false so the frame goes to normal stack/bridge processing. */
    return false;
}

void DumpMem(uint32_t addr, uint32_t count) {
    uint32_t ix, jx, kx;
    uint8_t *puc;
    char str[64];
    int flag = 0;

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

static void cmd_logclear(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv) {
    (void)pCmdIO; (void)argc; (void)argv;
    pkt_log.read_idx     = pkt_log.write_idx; /* drain pending entries */
    pkt_log.overflow_cnt = 0u;
    pkt_log.total_logged = 0u;
    frame_data_pool.write_offset = 0u;
    SYS_CONSOLE_PRINT("[LOG] ring buffer cleared\r\n");
}

static void cmd_logstat(SYS_CMD_DEVICE_NODE *pCmdIO, int argc, char **argv) {
    (void)pCmdIO; (void)argc; (void)argv;
    uint32_t wi      = pkt_log.write_idx;  /* snapshot volatile index */
    uint32_t pending = (wi - pkt_log.read_idx) & (PKT_LOG_BUF_SIZE - 1u);
    SYS_CONSOLE_PRINT("[LOG] total=%u pending=%u overflows=%u bufsize=%u\r\n",
        (unsigned)pkt_log.total_logged, (unsigned)pending,
        (unsigned)pkt_log.overflow_cnt, (unsigned)PKT_LOG_BUF_SIZE);
    SYS_CONSOLE_PRINT("[LOG] pool_offset=%u pool_size=%u (%u frames x %u bytes)\r\n",
        (unsigned)frame_data_pool.write_offset,
        (unsigned)FRAME_DATA_POOL_SIZE,
        (unsigned)PKT_LOG_MAX_FRAMES,
        (unsigned)PKT_LOG_MAX_FRAME_SIZE);
}

static void my_dump(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    //const void* cmdIoParam = pCmdIO->cmdIoParam;

    ipdump_mode = strtoul(argv[1], NULL, 16);
    if (ipdump_mode == 0) {
        SYS_CONSOLE_PRINT("IP Layer Dump de-activated\n\r");
    } else if (ipdump_mode == 1) {
        SYS_CONSOLE_PRINT("IP Layer Dump activated on eth0\n\r");
    } else {
        SYS_CONSOLE_PRINT("Parameter out of range\n\r");
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

/* meminfo: free memory on BOTH heaps.
 *  - C-runtime heap: XC32 uses nano-malloc (no mallinfo, and the whole heap is
 *    sbrk'd up front with free blocks tracked internally), so we report the total
 *    reserved size (_eheap-_heap) and PROBE the largest allocatable block with a
 *    non-destructive malloc/free binary search - a real "largest free chunk".
 *  - TCP/IP stack heap: the DRAM pool where packets/sockets/the MAC bridge
 *    allocate (same figures as the built-in 'heapinfo'). */
extern char _heap;            /* linker: C-runtime heap start (absolute symbol)  */
extern char _eheap;           /* linker: C-runtime heap end (= _heap + heap size) */
static size_t cheap_largest_free(size_t cap) {
    size_t lo = 1u, hi = cap, best = 0u;
    while (lo <= hi) {
        size_t mid = lo + (hi - lo) / 2u;
        void *p = malloc(mid);
        if (p) { free(p); best = mid; lo = mid + 1u; }
        else   { if (mid == 0u) break; hi = mid - 1u; }
    }
    return best;
}
static void cmd_meminfo(SYS_CMD_DEVICE_NODE* pCmdIO, int argc, char** argv) {
    size_t total = (size_t)((uintptr_t)&_eheap - (uintptr_t)&_heap);  /* via uintptr_t: not UB pointer subtraction */
    size_t largest = cheap_largest_free(total);
    TCPIP_STACK_HEAP_HANDLE h;
    (void)pCmdIO; (void)argc; (void)argv;

    SYS_CONSOLE_PRINT("C-runtime heap: total=%u  largest free block=%u  (nano-malloc; no exact free count)\r\n",
        (unsigned)total, (unsigned)largest);

    h = TCPIP_STACK_HeapHandleGet(TCPIP_STACK_HEAP_TYPE_INTERNAL_HEAP, 0);
    if (h != 0) {
        SYS_CONSOLE_PRINT("TCP/IP heap:    size=%u  free=%u  maxblock=%u  highwater=%u\r\n",
            (unsigned)TCPIP_STACK_HEAP_Size(h), (unsigned)TCPIP_STACK_HEAP_FreeSize(h),
            (unsigned)TCPIP_STACK_HEAP_MaxSize(h), (unsigned)TCPIP_STACK_HEAP_HighWatermark(h));
    } else {
        SYS_CONSOLE_PRINT("TCP/IP heap:    (no handle)\r\n");
    }
}

const SYS_CMD_DESCRIPTOR msd_cmd_tbl[] = {
    {"help", (SYS_CMD_FNC) test_help, ": show Test group commands"},
    {"timestamp", (SYS_CMD_FNC) show_timestamp, ": show build timestamp"},
    {"ipdump", (SYS_CMD_FNC) my_dump, ": dump rx ip packets (0:off 1:eth0)"},
    {"stats", (SYS_CMD_FNC) cmd_stats, ": show TX/RX counters for eth0"},
    {"meminfo", (SYS_CMD_FNC) cmd_meminfo, ": free memory on the C-runtime heap and the TCP/IP heap"},
    {"dump", (SYS_CMD_FNC) cmd_mem_dump, ": dump memory (dump <addr_hex> <count>)"},
    {"logclear",     (SYS_CMD_FNC) cmd_logclear,     ": clear deferred packet log buffer"},
    {"logstat",      (SYS_CMD_FNC) cmd_logstat,      ": show deferred log statistics (total, pending, overflows)"},
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
