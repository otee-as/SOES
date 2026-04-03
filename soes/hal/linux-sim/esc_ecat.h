/*
 * Licensed under the GNU General Public License version 2 with exceptions. See
 * LICENSE file in the project root for full license information
 */

/** \file
 * \brief
 * Software EtherCAT frame processor for the simulated ESC.
 *
 * Listens on a raw socket for EtherCAT frames (EtherType 0x88A4), parses
 * datagrams, maps them to reads/writes of the ESC register memory, and
 * returns the modified frame.  This allows an unmodified SOEM-based master
 * to discover and communicate with the simulated SOES slave over a real
 * (or virtual) Ethernet interface.
 */

#ifndef __ESC_ECAT_H__
#define __ESC_ECAT_H__

#include <stdint.h>

/* EtherCAT EtherType */
#define ETH_P_ECAT          0x88A4

/* EtherCAT datagram command codes */
#define EC_CMD_NOP           0x00
#define EC_CMD_APRD          0x01
#define EC_CMD_APWR          0x02
#define EC_CMD_APRW          0x03
#define EC_CMD_FPRD          0x04
#define EC_CMD_FPWR          0x05
#define EC_CMD_FPRW          0x06
#define EC_CMD_BRD           0x07
#define EC_CMD_BWR           0x08
#define EC_CMD_BRW           0x09
#define EC_CMD_LRD           0x0A
#define EC_CMD_LWR           0x0B
#define EC_CMD_LRW           0x0C
#define EC_CMD_ARMW          0x0D
#define EC_CMD_FRMW          0x0E

/* FMMU count and SM count in our simulated ESC */
#define ESC_FMMU_COUNT       4
#define ESC_SM_COUNT         4

/** Start the EtherCAT frame processor on the given network interface.
 *
 * Creates a background thread that:
 *  - Opens a raw AF_PACKET socket for ETH_P_ECAT
 *  - Receives frames, processes datagrams, modifies WKC
 *  - Sends the modified frame back
 *
 * @param[in]  ifname     Network interface name (e.g. "eth0", "veth0")
 * @param[in]  position   Auto-increment position of this slave (0 for first)
 * @return 0 on success, -1 on error
 */
int ESC_ecat_start (const char *ifname, int position);

/** Stop the EtherCAT frame processor. */
void ESC_ecat_stop (void);

#endif /* __ESC_ECAT_H__ */
