/*******************************************************************************
  System Configuration Header

  File Name:
    configuration.h

  Summary:
    Build-time configuration header for the system defined by this project.

  Description:
    An MPLAB Project may have multiple configurations.  This file defines the
    build-time options for a single configuration.

  Remarks:
    This configuration header must not define any prototypes or data
    definitions (or include any files that do).  It only provides macro
    definitions for build-time configuration options

*******************************************************************************/

// DOM-IGNORE-BEGIN
/*******************************************************************************
* Copyright (C) 2018 Microchip Technology Inc. and its subsidiaries.
*
* Subject to your compliance with these terms, you may use Microchip software
* and any derivatives exclusively with Microchip products. It is your
* responsibility to comply with third party license terms applicable to your
* use of third party software (including open source software) that may
* accompany Microchip software.
*
* THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
* EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
* WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
* PARTICULAR PURPOSE.
*
* IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
* INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
* WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
* BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
* FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
* ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
* THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
*******************************************************************************/
// DOM-IGNORE-END

#ifndef CONFIGURATION_H
#define CONFIGURATION_H

// *****************************************************************************
// *****************************************************************************
// Section: Included Files
// *****************************************************************************
// *****************************************************************************
/*  This section Includes other configuration headers necessary to completely
    define this configuration.
*/

#include "user.h"
#include "device.h"

// DOM-IGNORE-BEGIN
#ifdef __cplusplus  // Provide C++ Compatibility

extern "C" {

#endif
// DOM-IGNORE-END

// *****************************************************************************
// *****************************************************************************
// Section: System Configuration
// *****************************************************************************
// *****************************************************************************



// *****************************************************************************
// *****************************************************************************
// Section: System Service Configuration
// *****************************************************************************
// *****************************************************************************
/* TIME System Service Configuration Options */
#define SYS_TIME_INDEX_0                            (0)
#define SYS_TIME_MAX_TIMERS                         (5)
#define SYS_TIME_HW_COUNTER_WIDTH                   (16)
#define SYS_TIME_HW_COUNTER_PERIOD                  (0xFFFFU)
#define SYS_TIME_HW_COUNTER_HALF_PERIOD             (SYS_TIME_HW_COUNTER_PERIOD>>1)
#define SYS_TIME_CPU_CLOCK_FREQUENCY                (120000000)
#define SYS_TIME_COMPARE_UPDATE_EXECUTION_CYCLES    (232)

#define SYS_CONSOLE_INDEX_0                       0





#define SYS_CMD_ENABLE
#define SYS_CMD_DEVICE_MAX_INSTANCES       SYS_CONSOLE_DEVICE_MAX_INSTANCES
#define SYS_CMD_PRINT_BUFFER_SIZE          2048U
#define SYS_CMD_BUFFER_DMA_READY



#define SYS_DEBUG_ENABLE
#define SYS_DEBUG_GLOBAL_ERROR_LEVEL       SYS_ERROR_DEBUG
#define SYS_DEBUG_BUFFER_DMA_READY
#define SYS_DEBUG_USE_CONSOLE


#define SYS_CONSOLE_DEVICE_MAX_INSTANCES   			(1U)
#define SYS_CONSOLE_UART_MAX_INSTANCES 	   			(1U)
#define SYS_CONSOLE_USB_CDC_MAX_INSTANCES 	   		(0U)
#define SYS_CONSOLE_PRINT_BUFFER_SIZE        		(2048U)




// *****************************************************************************
// *****************************************************************************
// Section: Driver Configuration
// *****************************************************************************
// *****************************************************************************

/*** LAN865X Driver Configuration ***/
/*** Driver Compilation and static configuration options. ***/
#define TCPIP_IF_LAN865X

#define DRV_LAN865X_INSTANCES_NUMBER         1

#define DRV_LAN865X_SPI_DRIVER_INSTANCE_IDX0 0
#define DRV_LAN865X_CLIENT_INSTANCES_IDX0    1
#define DRV_LAN865X_SPI_FREQ_IDX0            15000000
#define DRV_LAN865X_MAC_RX_DESCRIPTORS_IDX0  2
#define DRV_LAN865X_MAX_RX_BUFFER_IDX0       1536
#define DRV_LAN865X_SPI_CS_IDX0              SYS_PORT_PIN_PC15
#define DRV_LAN865X_INTERRUPT_PIN_IDX0       SYS_PORT_PIN_PC14
#define DRV_LAN865X_RESET_PIN_IDX0           SYS_PORT_PIN_PC18
#define DRV_LAN865X_PROMISCUOUS_IDX0         false
#define DRV_LAN865X_TX_CUT_THROUGH_IDX0      true
#define DRV_LAN865X_RX_CUT_THROUGH_IDX0      false
#define DRV_LAN865X_CHUNK_SIZE_IDX0          64
#define DRV_LAN865X_CHUNK_XACT_IDX0          31
#define DRV_LAN865X_PLCA_ENABLE_IDX0         true
#define DRV_LAN865X_PLCA_NODE_ID_IDX0        1
#define DRV_LAN865X_PLCA_NODE_COUNT_IDX0     8
#define DRV_LAN865X_PLCA_BURST_COUNT_IDX0    0
#define DRV_LAN865X_PLCA_BURST_TIMER_IDX0    128



/* SPI Driver Common Configuration Options */
#define DRV_SPI_INSTANCES_NUMBER              (1U)

/* SPI Driver Instance 0 Configuration Options */
#define DRV_SPI_INDEX_0                       0
#define DRV_SPI_CLIENTS_NUMBER_IDX0           1
#define DRV_SPI_DMA_MODE
#define DRV_SPI_XMIT_DMA_CH_IDX0              SYS_DMA_CHANNEL_0
#define DRV_SPI_RCV_DMA_CH_IDX0               SYS_DMA_CHANNEL_1
#define DRV_SPI_QUEUE_SIZE_IDX0               1

/*** MIIM Driver Configuration ***/
#define DRV_MIIM_ETH_MODULE_ID_0                GMAC_BASE_ADDRESS
#define DRV_MIIM_DRIVER_INDEX_0                 0
#define DRV_MIIM_INSTANCES_NUMBER           1
#define DRV_MIIM_INSTANCE_OPERATIONS        4
#define DRV_MIIM_INSTANCE_CLIENTS           2
#define DRV_MIIM_CLIENT_OP_PROTECTION   false
#define DRV_MIIM_COMMANDS   false
#define DRV_MIIM_DRIVER_OBJECT              DRV_MIIM_OBJECT_BASE_Default            





// *****************************************************************************
// *****************************************************************************
// Section: Middleware & Other Library Configuration
// *****************************************************************************
// *****************************************************************************

	/*** tcpip_cmd Configuration ***/
	#define TCPIP_STACK_COMMAND_ENABLE



/* Network Configuration Index 0 */
#define TCPIP_NETWORK_DEFAULT_INTERFACE_NAME_IDX0 "LAN865x"

#define TCPIP_NETWORK_DEFAULT_HOST_NAME_IDX0              "MCHP_LAN865x"
#define TCPIP_NETWORK_DEFAULT_MAC_ADDR_IDX0               "00:04:25:1C:A0:02"

#define TCPIP_NETWORK_DEFAULT_IP_ADDRESS_IDX0         "192.168.100.11"
#define TCPIP_NETWORK_DEFAULT_IP_MASK_IDX0            "255.255.255.0"
#define TCPIP_NETWORK_DEFAULT_GATEWAY_IDX0            "192.168.100.1"
#define TCPIP_NETWORK_DEFAULT_DNS_IDX0                "192.168.100.1"
#define TCPIP_NETWORK_DEFAULT_SECOND_DNS_IDX0         ""
#define TCPIP_NETWORK_DEFAULT_POWER_MODE_IDX0         "full"
#define TCPIP_NETWORK_DEFAULT_INTERFACE_FLAGS_IDX0            \
                                                    TCPIP_NETWORK_CONFIG_IP_STATIC
                                                    
#define TCPIP_NETWORK_DEFAULT_MAC_DRIVER_IDX0         DRV_LAN865X_MACObject



/* Network Configuration Index 1 */
#define TCPIP_NETWORK_DEFAULT_INTERFACE_NAME_IDX1 "GMAC"
#define TCPIP_IF_GMAC  

#define TCPIP_NETWORK_DEFAULT_HOST_NAME_IDX1              "MCHPBOARD_C"
#define TCPIP_NETWORK_DEFAULT_MAC_ADDR_IDX1               "00:04:25:1C:A0:02"

#define TCPIP_NETWORK_DEFAULT_IP_ADDRESS_IDX1         "192.168.100.12"
#define TCPIP_NETWORK_DEFAULT_IP_MASK_IDX1            "255.255.255.0"
#define TCPIP_NETWORK_DEFAULT_GATEWAY_IDX1            "192.168.100.1"
#define TCPIP_NETWORK_DEFAULT_DNS_IDX1                "192.168.100.1"
#define TCPIP_NETWORK_DEFAULT_SECOND_DNS_IDX1         ""
#define TCPIP_NETWORK_DEFAULT_POWER_MODE_IDX1         "full"
#define TCPIP_NETWORK_DEFAULT_INTERFACE_FLAGS_IDX1            \
                                                    TCPIP_NETWORK_CONFIG_IP_STATIC
                                                    
#define TCPIP_NETWORK_DEFAULT_MAC_DRIVER_IDX1         DRV_GMAC_Object



/*** TCPIP Heap Configuration ***/
#define TCPIP_STACK_USE_INTERNAL_HEAP
#define TCPIP_STACK_DRAM_SIZE                       65536
#define TCPIP_STACK_DRAM_RUN_LIMIT                  2048

#define TCPIP_STACK_MALLOC_FUNC                     malloc

#define TCPIP_STACK_CALLOC_FUNC                     calloc

#define TCPIP_STACK_FREE_FUNC                       free



#define TCPIP_STACK_HEAP_USE_FLAGS                   TCPIP_STACK_HEAP_FLAG_ALLOC_UNCACHED

#define TCPIP_STACK_HEAP_USAGE_CONFIG                TCPIP_STACK_HEAP_USE_DEFAULT

#define TCPIP_STACK_SUPPORTED_HEAPS                  1




// *****************************************************************************
// *****************************************************************************
// Section: TCPIP Stack Configuration
// *****************************************************************************
// *****************************************************************************


#define TCPIP_STACK_TICK_RATE		        		5
#define TCPIP_STACK_SECURE_PORT_ENTRIES             10
#define TCPIP_STACK_LINK_RATE		        		333

#define TCPIP_STACK_ALIAS_INTERFACE_SUPPORT   false

#define TCPIP_PACKET_LOG_ENABLE     0

/* TCP/IP stack event notification */
#define TCPIP_STACK_USE_EVENT_NOTIFICATION
#define TCPIP_STACK_USER_NOTIFICATION   false
#define TCPIP_STACK_DOWN_OPERATION   true
#define TCPIP_STACK_IF_UP_DOWN_OPERATION   true
#define TCPIP_STACK_MAC_DOWN_OPERATION  true
#define TCPIP_STACK_INTERFACE_CHANGE_SIGNALING   false
#define TCPIP_STACK_CONFIGURATION_SAVE_RESTORE   true
#define TCPIP_STACK_EXTERN_PACKET_PROCESS   false
#define TCPIP_STACK_RUN_TIME_INIT   false

#define TCPIP_STACK_INTMAC_COUNT           1





/*** GMAC Configuration ***/
#define DRV_GMAC
#define DRV_SAME5x
#define TCPIP_GMAC_TX_DESCRIPTORS_COUNT_DUMMY    1
#define TCPIP_GMAC_RX_DESCRIPTORS_COUNT_DUMMY    1
#define TCPIP_GMAC_RX_BUFF_SIZE_DUMMY            64
#define TCPIP_GMAC_TX_BUFF_SIZE_DUMMY            64
/*** QUEUE 0 TX Configuration ***/
#define TCPIP_GMAC_TX_DESCRIPTORS_COUNT_QUE0            8
#define TCPIP_GMAC_MAX_TX_PKT_SIZE_QUE0                 1536
/*** QUEUE 0 RX Configuration ***/
#define TCPIP_GMAC_RX_DESCRIPTORS_COUNT_QUE0            8
#define TCPIP_GMAC_RX_BUFF_SIZE_QUE0                    1536
#define TCPIP_GMAC_RX_DEDICATED_BUFFERS_QUE0            8
#define TCPIP_GMAC_RX_ADDL_BUFF_COUNT_QUE0              2
#define TCPIP_GMAC_RX_BUFF_COUNT_THRESHOLD_QUE0         1
#define TCPIP_GMAC_RX_BUFF_ALLOC_COUNT_QUE0             2
#define TCPIP_GMAC_RX_FILTERS                       \
                                                        TCPIP_MAC_RX_FILTER_TYPE_BCAST_ACCEPT |\
                                                        TCPIP_MAC_RX_FILTER_TYPE_MCAST_ACCEPT |\
                                                        TCPIP_MAC_RX_FILTER_TYPE_UCAST_ACCEPT |\
                                                        TCPIP_MAC_RX_FILTER_TYPE_CRC_ERROR_REJECT |\
                                                          0
       
#define TCPIP_GMAC_SCREEN1_COUNT_QUE        0 
#define TCPIP_GMAC_SCREEN2_COUNT_QUE        0  

#define TCPIP_GMAC_ETH_OPEN_FLAGS                   \
                                                        TCPIP_ETH_OPEN_AUTO |\
                                                        TCPIP_ETH_OPEN_FDUPLEX |\
                                                        TCPIP_ETH_OPEN_HDUPLEX |\
                                                        TCPIP_ETH_OPEN_100 |\
                                                        TCPIP_ETH_OPEN_10 |\
                                                        TCPIP_ETH_OPEN_MDIX_AUTO |\
                                                        TCPIP_ETH_OPEN_RMII |\
                                                        0

#define TCPIP_GMAC_MODULE_ID                       GMAC_BASE_ADDRESS

#define TCPIP_INTMAC_PERIPHERAL_CLK                 120000000

#define DRV_GMAC_RX_CHKSM_OFFLOAD               (TCPIP_MAC_CHECKSUM_NONE)           
#define DRV_GMAC_TX_CHKSM_OFFLOAD               (TCPIP_MAC_CHECKSUM_NONE)       
#define TCPIP_GMAC_TX_PRIO_COUNT                1
#define TCPIP_GMAC_RX_PRIO_COUNT                1
#define DRV_GMAC_NUMBER_OF_QUEUES               1
#define DRV_GMAC_RMII_MODE                      0



#define DRV_LAN8740_PHY_CONFIG_FLAGS       ( 0 \
                                                    | DRV_ETHPHY_CFG_RMII \
                                                    )
                                                    
#define DRV_LAN8740_PHY_LINK_INIT_DELAY            500
#define DRV_LAN8740_PHY_ADDRESS                    0
#define DRV_LAN8740_PHY_PERIPHERAL_ID              GMAC_BASE_ADDRESS
#define DRV_ETHPHY_LAN8740_NEG_INIT_TMO            1
#define DRV_ETHPHY_LAN8740_NEG_DONE_TMO            2000
#define DRV_ETHPHY_LAN8740_RESET_CLR_TMO           500


#define TCPIP_STACK_NETWORK_INTERAFCE_COUNT  	2



/*** Bridge Configuration ***/
#define TCPIP_STACK_USE_MAC_BRIDGE
#define TCPIP_STACK_MAC_BRIDGE_COMMANDS false
#define TCPIP_MAC_BRIDGE_FDB_TABLE_ENTRIES          17
#define TCPIP_MAC_BRIDGE_MAX_PORTS_NO               2
#define TCPIP_MAC_BRIDGE_PACKET_POOL_SIZE           8
#define TCPIP_MAC_BRIDGE_PACKET_SIZE                1536
#define TCPIP_MAC_BRIDGE_PACKET_POOL_REPLENISH      2
#define TCPIP_MAC_BRIDGE_DCPT_POOL_SIZE             16
#define TCPIP_MAC_BRIDGE_DCPT_POOL_REPLENISH        4
/* Advanced */
#define TCPIP_MAC_BRIDGE_ENTRY_TIMEOUT              300
#define TCPIP_MAC_BRIDGE_MAX_TRANSIT_DELAY          1
#define TCPIP_MAC_BRIDGE_TASK_RATE                  333

#define TCPIP_MAC_BRIDGE_STATISTICS          		false
#define TCPIP_MAC_BRIDGE_EVENT_NOTIFY          		false

#define TCPIP_MAC_BRIDGE_IF_NAME_TABLE false

#define TCPIP_MC_BRIDGE_INIT_FLAGS                  \
                                                    0

#define TCPIP_STACK_MAC_BRIDGE_DISABLE_GLUE_PORTS false




// *****************************************************************************
// *****************************************************************************
// Section: Application Configuration
// *****************************************************************************
// *****************************************************************************


//DOM-IGNORE-BEGIN
#ifdef __cplusplus
}
#endif
//DOM-IGNORE-END

#endif // CONFIGURATION_H
/*******************************************************************************
 End of File
*/
