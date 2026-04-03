#ifndef __ECAT_OPTIONS_H__
#define __ECAT_OPTIONS_H__

#include "cc.h"

/*
 * EtherCAT stack options for the analog I/O simulated slave.
 *
 * 4×int16 RxPDO + 4×int16 TxPDO = 8 bytes each direction.
 * Mailbox: CoE only (no FoE, no EoE) — typical for a simple analog I/O module.
 * DC: advertised and handled.
 */

#define USE_FOE   0
#define USE_EOE   0

#define MBXSIZE     128
#define MBXSIZEBOOT 128
#define MBXBUFFERS  3

#define MBX0_sma   0x1000
#define MBX0_sml   MBXSIZE
#define MBX0_sme   (MBX0_sma + MBX0_sml - 1)
#define MBX0_smc   0x26
#define MBX1_sma   (MBX0_sma + MBX0_sml)
#define MBX1_sml   MBXSIZE
#define MBX1_sme   (MBX1_sma + MBX1_sml - 1)
#define MBX1_smc   0x22

#define MBX0_sma_b  0x1000
#define MBX0_sml_b  MBXSIZEBOOT
#define MBX0_sme_b  (MBX0_sma_b + MBX0_sml_b - 1)
#define MBX0_smc_b  0x26
#define MBX1_sma_b  (MBX0_sma_b + MBX0_sml_b)
#define MBX1_sml_b  MBXSIZEBOOT
#define MBX1_sme_b  (MBX1_sma_b + MBX1_sml_b - 1)
#define MBX1_smc_b  0x22

/* PDO SyncManagers — must not overlap mailbox region (ends at 0x1100) */
#define SM2_sma   0x1100   /* RxPDO: 4×int16 = 8 bytes */
#define SM2_sml   8        /* RxPDO length: 4 channels × 2 bytes = 8 bytes */
#define SM2_smc   0x24
#define SM2_act   1
#define SM3_sma   0x1180   /* TxPDO: 4×int16 = 8 bytes */
#define SM3_sml   8        /* TxPDO length: 4 channels × 2 bytes = 8 bytes */
#define SM3_smc   0x20
#define SM3_act   1

#define MAX_RXPDO_SIZE   64
#define MAX_TXPDO_SIZE   64

/* 4 PDO mappings per SM (one entry per channel) */
#define MAX_MAPPINGS_SM2  4
#define MAX_MAPPINGS_SM3  4

#endif /* __ECAT_OPTIONS_H__ */
