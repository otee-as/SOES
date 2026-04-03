/*
 * Licensed under the GNU General Public License version 2 with exceptions. See
 * LICENSE file in the project root for full license information
 */

/** \file
 * \brief
 * Software EtherCAT frame processor for the simulated ESC.
 *
 * Receives raw EtherCAT frames from a network interface, processes each
 * datagram (APRD/APWR/FPRD/FPWR/BRD/BWR/LRD/LWR/LRW etc.), performs
 * the corresponding reads/writes on the simulated ESC memory, updates
 * the Working Counter, and sends the modified frame back.
 */

#define _GNU_SOURCE  /* for pthread_timedjoin_np */

#include "esc_ecat.h"
#include "esc_hw.h"
#include "esc_eep.h"
#include "esc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <arpa/inet.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

/* ---- Configuration ------------------------------------------------------- */

/** Configured station address.  Set by master via APWR to register 0x0010.
 *  We also track it so FPRD/FPWR can match.
 */
static uint16_t configured_addr = 0;

/** Auto-increment position counter.  Each slave decrements the ADP field
 *  in AP-type datagrams.  Position 0 means "first slave on the bus".
 */
static int slave_position = 0;

/** Volatile flag for the receiver thread. */
static volatile int ecat_running = 0;
static pthread_t ecat_thread;
static int raw_sock = -1;

/* ---- FMMU support -------------------------------------------------------- */

/** Cached FMMU configuration, read from ESC register space 0x0600+.
 *  Each FMMU is 16 bytes.
 */
typedef struct
{
   uint32_t log_start;     /* Logical start address */
   uint16_t log_length;    /* Length in bytes */
   uint8_t  log_start_bit; /* Start bit within first byte */
   uint8_t  log_end_bit;   /* End bit within last byte */
   uint16_t phys_start;    /* Physical start address in ESC memory */
   uint8_t  phys_start_bit;
   uint8_t  type;          /* 1=read (inputs), 2=write (outputs), 3=read+write */
   uint8_t  active;        /* bit 0 = enabled */
} fmmu_t;

/** Read FMMU configuration from ESC register space. */
static void fmmu_read (int idx, fmmu_t *f)
{
   uint8_t *mem = ESC_sim_get_mem ();
   unsigned base = 0x0600 + (unsigned)idx * 16;

   f->log_start     = (uint32_t)mem[base]
                     | ((uint32_t)mem[base + 1] << 8)
                     | ((uint32_t)mem[base + 2] << 16)
                     | ((uint32_t)mem[base + 3] << 24);
   f->log_length    = (uint16_t)(mem[base + 4] | ((uint16_t)mem[base + 5] << 8));
   f->log_start_bit = mem[base + 6];
   f->log_end_bit   = mem[base + 7];
   f->phys_start    = (uint16_t)(mem[base + 8] | ((uint16_t)mem[base + 9] << 8));
   f->phys_start_bit = mem[base + 10];
   f->type          = mem[base + 11];
   f->active        = mem[base + 12];
}

/* ---- ESC register helpers ------------------------------------------------ */

/** Read from simulated ESC memory directly (bypassing stack's ESC_read
 *  to avoid ALevent side effects during frame processing).
 */
static void esc_mem_read (uint16_t addr, void *buf, uint16_t len)
{
   uint8_t *mem = ESC_sim_get_mem ();
   if ((uint32_t)addr + len <= ESC_SIM_MEM_SIZE)
   {
      memcpy (buf, &mem[addr], len);
   }
   else
   {
      memset (buf, 0, len);
   }
}

/** Write to simulated ESC memory directly. */
static void esc_mem_write (uint16_t addr, const void *buf, uint16_t len)
{
   uint8_t *mem = ESC_sim_get_mem ();
   if ((uint32_t)addr + len <= ESC_SIM_MEM_SIZE)
   {
      memcpy (&mem[addr], buf, len);
   }
}

/** Read a LE16 from ESC memory */
static uint16_t esc_mem_read16 (uint16_t addr)
{
   uint8_t b[2];
   esc_mem_read (addr, b, 2);
   return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

/** Write a LE16 to ESC memory */
static void esc_mem_write16 (uint16_t addr, uint16_t val)
{
   uint8_t b[2];
   b[0] = (uint8_t)(val & 0xFF);
   b[1] = (uint8_t)((val >> 8) & 0xFF);
   esc_mem_write (addr, b, 2);
}

/* ---- AL event helpers ---------------------------------------------------- */

/** Set bits in the AL Event register (0x0220) */
static void set_alevent_bits (uint16_t bits)
{
   uint16_t alevent = esc_mem_read16 (ESCREG_ALEVENT);
   alevent |= bits;
   esc_mem_write16 (ESCREG_ALEVENT, alevent);
}

/* ---- Synchronous EEPROM command processing ------------------------------- */

/** Handle EEPROM command synchronously in the frame processor.
 *
 * In real ESC hardware, when the master writes to the EEPROM control register
 * (0x0502), the ESC immediately sets the busy bit, executes the command,
 * writes results, and clears busy — all before the next frame arrives.
 *
 * SOEM does NOT set the busy bit itself; it expects the ESC to handle it.
 * EEP_process() in the main loop checks the busy bit and returns if not set.
 * So we must process EEPROM commands synchronously here in the frame processor,
 * otherwise SOEM polls "not busy" immediately and reads stale data.
 */
static void eeprom_process_sync (void)
{
   uint8_t *mem = ESC_sim_get_mem ();

   /* Read the EEPROM control/status register (2 bytes at 0x0502-0x0503) */
   uint16_t contstat = esc_mem_read16 (ESCREG_EECONTSTAT);

   /* Extract command from bits 8-10 of the 16-bit register */
   uint8_t cmd = (uint8_t)((contstat >> 8) & 0x07);

   if (cmd == EEP_CMD_IDLE)
      return;

   /* Read the EEPROM address register (4 bytes at 0x0504-0x0507) */
   uint32_t addr = (uint32_t)mem[0x0504]
                 | ((uint32_t)mem[0x0505] << 8)
                 | ((uint32_t)mem[0x0506] << 16)
                 | ((uint32_t)mem[0x0507] << 24);

   /* Set busy bit to signal the command is being processed */
   contstat |= 0x8000;
   esc_mem_write16 (ESCREG_EECONTSTAT, contstat);

   /* Clear error bits */
   contstat = (uint16_t)(contstat & 0x87FFu);

   /* Always read 8 bytes – our simulated ESC advertises R64 capability.
    * The master may overwrite the R64 status bit (0x0040) when it writes
    * the command to 0x0502, but the capability is fixed.  We always return
    * 8 bytes and restore the R64 flag in the status register.
    */
   uint16_t read_size = 8;

   uint8_t eep_buf[8];
   memset (eep_buf, 0, sizeof (eep_buf));

   switch (cmd)
   {
      case EEP_CMD_READ:
      case EEP_CMD_RELOAD:
      {
         /* Read from emulated EEPROM into the data register at 0x0508 */
         if (EEP_read (addr * 2, eep_buf, read_size) != 0)
         {
            /* Set ack error bit (bit 13) */
            contstat |= 0x2000u;
         }
         else
         {
            esc_mem_write (ESCREG_EEDATA, eep_buf, read_size);
         }
         break;
      }

      case EEP_CMD_WRITE:
      {
         /* Read data from 0x0508, write to EEPROM */
         esc_mem_read (ESCREG_EEDATA, eep_buf, 2);
         if (EEP_write (addr * 2, eep_buf, 2) != 0)
         {
            contstat |= 0x2000u;  /* ack error */
         }
         break;
      }

      default:
         contstat |= 0x2000u;  /* ack error for unknown commands */
         break;
   }

   /* Clear busy bit and restore R64 capability flag to signal completion */
   contstat = (uint16_t)((contstat & 0x7FFFu) | 0x0040u);
   esc_mem_write16 (ESCREG_EECONTSTAT, contstat);
}

/** Determine which SM was written to and raise the corresponding event bit.
 *  Also update SM status registers to emulate ESC hardware behaviour:
 *  - When master writes to SM data area: set MBXstat bit (bit 3) in status
 *  - This allows the slave stack (PDI side) to detect new mailbox data
 */
static void check_sm_event (uint16_t addr, uint16_t len)
{
   /* SM registers at 0x0800 + n*8 */
   /* SM data areas: check if the written address falls within an SM's
    * physical start address range.
    */
   uint8_t *mem = ESC_sim_get_mem ();
   int i;
   for (i = 0; i < ESC_SM_COUNT; i++)
   {
      unsigned sm_base = 0x0800 + (unsigned)i * 8;
      uint16_t sm_addr = (uint16_t)(mem[sm_base] | ((uint16_t)mem[sm_base + 1] << 8));
      uint16_t sm_len  = (uint16_t)(mem[sm_base + 2] | ((uint16_t)mem[sm_base + 3] << 8));
      uint8_t  sm_act  = mem[sm_base + 6];

      if (sm_len == 0 || !(sm_act & 0x01))
         continue;

      /* Check for overlap: [addr, addr+len) overlaps [sm_addr, sm_addr+sm_len) */
      if (addr < (sm_addr + sm_len) && (addr + len) > sm_addr)
      {
         /* Raise SM event for this SM */
         uint16_t sm_event_bit = (uint16_t)(0x0100 << i); /* SM0=0x0100, SM1=0x0200 ... */
         set_alevent_bits (sm_event_bit);

         /* Set SM status register MBXstat/BUFstat bit (bit 3) to indicate
          * that master has written new data to this SM's data area.
          * In a real ESC this is done automatically by hardware.
          * The PDI side (slave stack) reads this to know a mailbox is full.
          */
         mem[sm_base + 5] |= 0x08;  /* Status byte bit 3 = written/full */
      }
   }

   /* Also check if SM configuration registers themselves were written */
   if (addr >= 0x0800 && addr < (0x0800 + ESC_SM_COUNT * 8))
   {
      set_alevent_bits (ESCREG_ALEVENT_SMCHANGE);
   }

   /* Check if AL Control was written */
   if (addr <= ESCREG_ALCONTROL && (addr + len) > ESCREG_ALCONTROL)
   {
      set_alevent_bits (ESCREG_ALEVENT_CONTROL);
   }

   /* Check if EEPROM control was written – process command synchronously */
   if (addr <= 0x0502 && (addr + len) > 0x0502)
   {
      eeprom_process_sync ();
      /* Note: we still raise the EEP event for the main loop's bookkeeping,
       * but by this point the command is already completed. */
      set_alevent_bits (ESCREG_ALEVENT_EEP);
   }
}

/** Handle SM events triggered by master READS from SM data areas.
 *  When the master reads from SM1 (mailbox out), the SM1 status bit should
 *  be cleared to indicate the mailbox was consumed, and SM1 event is raised
 *  so the slave stack knows the master picked up the response.
 */
static void check_sm_read_event (uint16_t addr, uint16_t len)
{
   uint8_t *mem = ESC_sim_get_mem ();
   int i;
   for (i = 0; i < ESC_SM_COUNT; i++)
   {
      unsigned sm_base = 0x0800 + (unsigned)i * 8;
      uint16_t sm_addr = (uint16_t)(mem[sm_base] | ((uint16_t)mem[sm_base + 1] << 8));
      uint16_t sm_len  = (uint16_t)(mem[sm_base + 2] | ((uint16_t)mem[sm_base + 3] << 8));
      uint8_t  sm_act  = mem[sm_base + 6];

      if (sm_len == 0 || !(sm_act & 0x01))
         continue;

      /* Check for overlap: [addr, addr+len) overlaps [sm_addr, sm_addr+sm_len) */
      if (addr < (sm_addr + sm_len) && (addr + len) > sm_addr)
      {
         /* Raise SM event to notify slave stack */
         uint16_t sm_event_bit = (uint16_t)(0x0100 << i);
         set_alevent_bits (sm_event_bit);

         /* Clear SM status MBXstat bit – master has read the data.
          * In a real ESC, reading from a mailbox-out SM clears the status.
          */
         mem[sm_base + 5] &= (uint8_t)~0x08;
      }
   }
}

/* ---- Logical address (FMMU) processing ---------------------------------- */

/** Process a logical read: gather data from ESC physical memory into
 *  the datagram data area according to FMMU mappings.
 *  Returns 1 if any FMMU matched (for WKC).
 */
static int logical_read (uint32_t log_addr, uint8_t *data, uint16_t len)
{
   int matched = 0;
   int i;

   for (i = 0; i < ESC_FMMU_COUNT; i++)
   {
      fmmu_t f;
      fmmu_read (i, &f);

      if (!(f.active & 0x01) || f.log_length == 0)
         continue;

      /* FMMU must be a read type (1=read from slave, 3=read+write) */
      if (!(f.type & 0x01))
         continue;

      /* Check overlap between [log_addr, log_addr+len) and
       * [f.log_start, f.log_start+f.log_length) */
      uint32_t fmmu_end = f.log_start + f.log_length;
      uint32_t dg_end = log_addr + len;

      if (log_addr >= fmmu_end || dg_end <= f.log_start)
         continue;

      /* Compute the overlap region */
      uint32_t start = (log_addr > f.log_start) ? log_addr : f.log_start;
      uint32_t end = (dg_end < fmmu_end) ? dg_end : fmmu_end;

      /* Offset into datagram data */
      uint32_t dg_off = start - log_addr;
      /* Offset into FMMU mapped physical range */
      uint32_t phys_off = start - f.log_start;
      uint16_t phys_addr = (uint16_t)(f.phys_start + phys_off);
      uint16_t copy_len = (uint16_t)(end - start);

      esc_mem_read (phys_addr, &data[dg_off], copy_len);
      matched = 1;
   }
   return matched;
}

/** Process a logical write: scatter data from the datagram into ESC physical
 *  memory according to FMMU mappings.
 *  Returns 1 if any FMMU matched (for WKC).
 */
static int logical_write (uint32_t log_addr, const uint8_t *data, uint16_t len)
{
   int matched = 0;
   int i;

   for (i = 0; i < ESC_FMMU_COUNT; i++)
   {
      fmmu_t f;
      fmmu_read (i, &f);

      if (!(f.active & 0x01) || f.log_length == 0)
         continue;

      /* FMMU must be a write type (2=write to slave, 3=read+write) */
      if (!(f.type & 0x02))
         continue;

      uint32_t fmmu_end = f.log_start + f.log_length;
      uint32_t dg_end = log_addr + len;

      if (log_addr >= fmmu_end || dg_end <= f.log_start)
         continue;

      uint32_t start = (log_addr > f.log_start) ? log_addr : f.log_start;
      uint32_t end = (dg_end < fmmu_end) ? dg_end : fmmu_end;

      uint32_t dg_off = start - log_addr;
      uint32_t phys_off = start - f.log_start;
      uint16_t phys_addr = (uint16_t)(f.phys_start + phys_off);
      uint16_t copy_len = (uint16_t)(end - start);

      esc_mem_write (phys_addr, &data[dg_off], copy_len);

      /* Check if the written physical address triggers SM events */
      check_sm_event (phys_addr, copy_len);
      matched = 1;
   }
   return matched;
}

/* ---- Datagram processing ------------------------------------------------- */

/**
 * Process a single EtherCAT datagram.
 *
 * @param[in,out] dgram     Pointer to the 10-byte datagram header (cmd, idx, ADP, ADO, len+flags)
 * @param[in]     data      Pointer to the datagram data area (after the header)
 * @param[in]     data_len  Length of the data area (from dlength field, low 11 bits)
 * @param[in,out] wkc_ptr   Pointer to the 16-bit WKC field after the data
 *
 * Returns: non-zero if this slave processed the datagram (for debug logging)
 */
static int process_datagram (uint8_t *dgram, uint8_t *data,
                             uint16_t data_len, uint8_t *wkc_ptr)
{
   uint8_t cmd = dgram[0];
   /* uint8_t idx = dgram[1]; -- index, passed through */
   int16_t adp = (int16_t)(dgram[2] | ((uint16_t)dgram[3] << 8)); /* ADP: slave address/position */
   uint16_t ado = (uint16_t)(dgram[4] | ((uint16_t)dgram[5] << 8)); /* ADO: offset/register address */

   /* Current WKC */
   uint16_t wkc = (uint16_t)(wkc_ptr[0] | ((uint16_t)wkc_ptr[1] << 8));

   int addressed = 0;  /* did this slave process the datagram? */

   switch (cmd)
   {
      /* ---- Auto-Increment commands ---- */
      /* ADP is treated as a position counter. If ADP == 0, this slave is
       * addressed. The slave then increments ADP (wraps around). */
      case EC_CMD_APRD:
      {
         if (adp == 0)
         {
            esc_mem_read (ado, data, data_len);
            check_sm_read_event (ado, data_len);
            wkc++;
            addressed = 1;
         }
         /* Increment the position counter in the frame */
         adp++;
         dgram[2] = (uint8_t)(adp & 0xFF);
         dgram[3] = (uint8_t)((adp >> 8) & 0xFF);
         break;
      }

      case EC_CMD_APWR:
      {
         if (adp == 0)
         {
            esc_mem_write (ado, data, data_len);
            check_sm_event (ado, data_len);

            /* Track configured address assignment */
            if (ado == ESCREG_ADDRESS)
            {
               configured_addr = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
               printf ("ecat: Configured address set to 0x%04X\n", configured_addr);
            }

            wkc++;
            addressed = 1;
         }
         adp++;
         dgram[2] = (uint8_t)(adp & 0xFF);
         dgram[3] = (uint8_t)((adp >> 8) & 0xFF);
         break;
      }

      case EC_CMD_APRW:
      {
         if (adp == 0)
         {
            /* Read first, then write */
            uint8_t tmp[1500];
            uint16_t clen = (data_len > sizeof(tmp)) ? (uint16_t)sizeof(tmp) : data_len;
            esc_mem_read (ado, tmp, clen);
            esc_mem_write (ado, data, data_len);
            check_sm_event (ado, data_len);
            memcpy (data, tmp, clen);
            wkc += 3;
            addressed = 1;
         }
         adp++;
         dgram[2] = (uint8_t)(adp & 0xFF);
         dgram[3] = (uint8_t)((adp >> 8) & 0xFF);
         break;
      }

      /* ---- Configured Address (Fixed Physical) commands ---- */
      case EC_CMD_FPRD:
      {
         uint16_t target = (uint16_t)adp;
         if (target == configured_addr && configured_addr != 0)
         {
            esc_mem_read (ado, data, data_len);
            check_sm_read_event (ado, data_len);
            wkc++;
            addressed = 1;
         }
         break;
      }

      case EC_CMD_FPWR:
      {
         uint16_t target = (uint16_t)adp;
         if (target == configured_addr && configured_addr != 0)
         {
            esc_mem_write (ado, data, data_len);
            check_sm_event (ado, data_len);

            /* Track station address re-assignment during recovery.
             * ecx_recover_slave() first assigns EC_TEMPNODE (0xFFFF) via APWR,
             * verifies the slave identity via EEPROM reads, then reassigns the
             * original configured address via FPWR to EC_TEMPNODE.
             * Without updating configured_addr here the slave would keep
             * responding to 0xFFFF while the master switches to the real
             * configadr, causing all subsequent FPWR/FPRD to get WKC=0. */
            if (ado == ESCREG_ADDRESS && data_len >= 2)
            {
               uint16_t new_addr = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
               if (new_addr != configured_addr)
               {
                  configured_addr = new_addr;
                  printf ("ecat: Configured address reassigned to 0x%04X\n", configured_addr);
               }
            }

            wkc++;
            addressed = 1;
         }
         break;
      }

      case EC_CMD_FPRW:
      {
         uint16_t target = (uint16_t)adp;
         if (target == configured_addr && configured_addr != 0)
         {
            uint8_t tmp[1500];
            uint16_t clen = (data_len > sizeof(tmp)) ? (uint16_t)sizeof(tmp) : data_len;
            esc_mem_read (ado, tmp, clen);
            esc_mem_write (ado, data, data_len);
            check_sm_event (ado, data_len);
            memcpy (data, tmp, clen);
            wkc += 3;
            addressed = 1;
         }
         break;
      }

      /* ---- Broadcast commands ---- */
      case EC_CMD_BRD:
      {
         /* All slaves respond: OR data from our registers into datagram */
         uint8_t tmp[1500];
         uint16_t clen = (data_len > sizeof(tmp)) ? (uint16_t)sizeof(tmp) : data_len;
         esc_mem_read (ado, tmp, clen);
         /* BRD: logical OR of all slave data */
         unsigned k;
         for (k = 0; k < clen; k++)
         {
            data[k] |= tmp[k];
         }
         wkc++;
         addressed = 1;
         break;
      }

      case EC_CMD_BWR:
      {
         esc_mem_write (ado, data, data_len);
         check_sm_event (ado, data_len);

         /* Track configured address from broadcast too */
         if (ado == ESCREG_ADDRESS)
         {
            configured_addr = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
         }

         wkc++;
         addressed = 1;
         break;
      }

      case EC_CMD_BRW:
      {
         uint8_t tmp[1500];
         uint16_t clen = (data_len > sizeof(tmp)) ? (uint16_t)sizeof(tmp) : data_len;
         esc_mem_read (ado, tmp, clen);
         esc_mem_write (ado, data, data_len);
         check_sm_event (ado, data_len);
         memcpy (data, tmp, clen);
         wkc += 3;
         addressed = 1;
         break;
      }

      /* ---- Logical addressing commands (use FMMU) ---- */
      case EC_CMD_LRD:
      {
         /* Logical address is formed from ADP(low16) + ADO(high16)
          * Actually: logical address = ADP | (ADO << 16)
          * Wait, in EtherCAT the logical address for LRD/LWR/LRW:
          *   ADP contains low 16 bits, ADO contains high 16 bits
          * So logical_addr = (uint32_t)adp | ((uint32_t)ado << 16)
          * But ADP is signed... we need the raw 16-bit value.
          */
         uint32_t log_addr = (uint32_t)(uint16_t)adp | ((uint32_t)ado << 16);
         if (logical_read (log_addr, data, data_len))
         {
            wkc++;
            addressed = 1;
         }
         break;
      }

      case EC_CMD_LWR:
      {
         uint32_t log_addr = (uint32_t)(uint16_t)adp | ((uint32_t)ado << 16);
         if (logical_write (log_addr, data, data_len))
         {
            wkc++;
            addressed = 1;
         }
         break;
      }

      case EC_CMD_LRW:
      {
         /* Logical read-write: read current, then write new */
         uint32_t log_addr = (uint32_t)(uint16_t)adp | ((uint32_t)ado << 16);
         uint8_t tmp[1500];
         uint16_t clen = (data_len > sizeof(tmp)) ? (uint16_t)sizeof(tmp) : data_len;
         int r_match, w_match;

         /* Read the current data first: start from the incoming frame data so
          * that bytes outside this slave's FMMU range are preserved intact
          * (another slave upstream may already have written its TxPDO there). */
         memcpy (tmp, data, clen);
         r_match = logical_read (log_addr, tmp, clen);

         /* Write the incoming data */
         w_match = logical_write (log_addr, data, data_len);

         /* Return the previously-read data */
         memcpy (data, tmp, clen);

         /* WKC: +1 for read, +2 for write, +3 for both */
         if (r_match && w_match)
            wkc += 3;
         else if (r_match)
            wkc += 1;
         else if (w_match)
            wkc += 2;

         addressed = (r_match || w_match);
         break;
      }

      case EC_CMD_ARMW:
      {
         /* Auto-increment read, multiple write - mostly used for DC */
         if (adp == 0)
         {
            esc_mem_read (ado, data, data_len);
            wkc++;
            addressed = 1;
         }
         else
         {
            /* Not position 0: write data to register */
            esc_mem_write (ado, data, data_len);
         }
         adp++;
         dgram[2] = (uint8_t)(adp & 0xFF);
         dgram[3] = (uint8_t)((adp >> 8) & 0xFF);
         break;
      }

      case EC_CMD_FRMW:
      {
         uint16_t target = (uint16_t)adp;
         if (target == configured_addr && configured_addr != 0)
         {
            esc_mem_read (ado, data, data_len);
            wkc++;
            addressed = 1;
         }
         else
         {
            /* Not our address: write data to register */
            esc_mem_write (ado, data, data_len);
         }
         break;
      }

      case EC_CMD_NOP:
      default:
         break;
   }

   /* Write back WKC */
   wkc_ptr[0] = (uint8_t)(wkc & 0xFF);
   wkc_ptr[1] = (uint8_t)((wkc >> 8) & 0xFF);

   return addressed;
}

/* ---- Frame processing ---------------------------------------------------- */

/** Process a complete EtherCAT Ethernet frame.
 *
 * @param[in,out] frame     Raw Ethernet frame buffer (modified in-place)
 * @param[in]     frame_len Total frame length
 * @return Number of datagrams processed by this slave
 */
static int process_frame (uint8_t *frame, int frame_len)
{
   int datagrams_processed = 0;

   /* Ethernet header: 6 dst + 6 src + 2 ethertype = 14 bytes */
   if (frame_len < 16)
      return 0;

   /* Check EtherType */
   uint16_t ethertype = (uint16_t)((frame[12] << 8) | frame[13]);
   if (ethertype != ETH_P_ECAT)
      return 0;

   /* EtherCAT header: 2 bytes after Ethernet header
    * Bits 0-10: datagram length (total datagrams payload)
    * Bit 12: reserved (frame type, should be 1 for EtherCAT)
    */
   uint16_t ecat_hdr = (uint16_t)(frame[14] | ((uint16_t)frame[15] << 8));
   uint16_t ecat_len = ecat_hdr & 0x07FF;
   (void)ecat_len;

   /* Walk through datagrams starting at offset 16 */
   int offset = 16;
   int more = 1;

   while (more && offset < frame_len)
   {
      /* Datagram header: 10 bytes
       * [0]    command
       * [1]    index
       * [2-3]  ADP (slave address / position)
       * [4-5]  ADO (register address / offset)
       * [6-7]  dlength (bits 0-10) + flags (bit 15 = more datagrams)
       * [8-9]  IRQ (interrupt request, usually 0)
       */
      if (offset + 10 > frame_len)
         break;

      uint8_t *dgram = &frame[offset];
      uint16_t dlength_raw = (uint16_t)(dgram[6] | ((uint16_t)dgram[7] << 8));
      uint16_t data_len = dlength_raw & 0x07FF;
      more = (dlength_raw & 0x8000) ? 1 : 0;

      /* Data starts at offset+10, WKC is at offset+10+data_len */
      uint8_t *data = &frame[offset + 10];
      int wkc_offset = offset + 10 + data_len;

      if (wkc_offset + 2 > frame_len)
         break;

      uint8_t *wkc_ptr = &frame[wkc_offset];

      /* Process this datagram */
      if (process_datagram (dgram, data, data_len, wkc_ptr))
      {
         datagrams_processed++;
      }

      /* Move to next datagram: header(10) + data(data_len) + wkc(2) */
      offset = wkc_offset + 2;
   }

   return datagrams_processed;
}

/* ---- Receiver thread ----------------------------------------------------- */

static void * ecat_receiver_thread (void *arg)
{
   (void)arg;
   uint8_t buf[1518]; /* Max Ethernet frame */
   int n;

   /* Block SIGTERM/SIGINT in this thread so they are delivered
    * to the main thread (which sets 'running = 0' and then calls
    * ESC_ecat_stop to shut us down).
    */
   {
      sigset_t ss;
      sigemptyset (&ss);
      sigaddset (&ss, SIGTERM);
      sigaddset (&ss, SIGINT);
      pthread_sigmask (SIG_BLOCK, &ss, NULL);
   }

   printf ("ecat: Frame processor thread started (sock=%d)\n", raw_sock);

   /* Set a receive timeout so we can check ecat_running periodically */
   {
      struct timeval tv;
      tv.tv_sec  = 0;
      tv.tv_usec = 10000; /* 10 ms */
      setsockopt (raw_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
   }

   while (ecat_running)
   {
      struct sockaddr_ll saddr;
      socklen_t saddr_len = sizeof (saddr);
      int fd = raw_sock;

      if (fd < 0 || !ecat_running)
         break;

      n = (int)recvfrom (fd, buf, sizeof (buf), 0,
                         (struct sockaddr *)&saddr, &saddr_len);
      if (n < 0)
      {
         if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;
         /* Socket was closed or other fatal error – exit thread */
         break;
      }

      /* Skip frames we sent ourselves (outgoing) */
      if (saddr.sll_pkttype == PACKET_OUTGOING)
         continue;

      /* Must be at least Ethernet header(14) + EtherCAT header(2) */
      if (n < 16)
         continue;

      /* Verify EtherType */
      uint16_t ethertype = (uint16_t)((buf[12] << 8) | buf[13]);
      if (ethertype != ETH_P_ECAT)
         continue;

      /* Process the frame - modifies datagrams and WKC in place.
       * Hold the ESC memory lock for the entire frame so that the
       * main thread's ESC_read/ESC_write and our register accesses
       * don't interleave.
       */
      ESC_sim_lock ();
      (void)process_frame (buf, n);
      ESC_sim_unlock ();

      /* Always forward the frame back, even when this slave was not addressed.
       * In a real EtherCAT ring every slave passes the frame through regardless
       * of whether any datagram matched it.  Dropping unaddressed frames breaks
       * the relay model used by CI tests with multiple simulated slaves. */
      {
         /* Swap source and destination MAC addresses so the frame goes back */
         uint8_t tmp_mac[6];
         memcpy (tmp_mac, &buf[0], 6);
         memcpy (&buf[0], &buf[6], 6);
         memcpy (&buf[6], tmp_mac, 6);

         /* Send the modified (or unmodified) frame back */
         struct sockaddr_ll dest;
         memset (&dest, 0, sizeof (dest));
         dest.sll_family   = AF_PACKET;
         dest.sll_ifindex  = saddr.sll_ifindex;
         dest.sll_protocol = htons (ETH_P_ECAT);

         if (sendto (fd, buf, (size_t)n, 0,
                     (struct sockaddr *)&dest, sizeof (dest)) < 0)
         {
            if (errno != EINTR)
               perror ("ecat: sendto");
         }
      }
   }

   printf ("ecat: Frame processor thread exiting\n");
   return NULL;
}

/* ---- Public API ---------------------------------------------------------- */

int ESC_ecat_start (const char *ifname, int position)
{
   struct ifreq ifr;
   struct sockaddr_ll sll;
   int ifindex;

   slave_position = position;
   configured_addr = 0;

   /* Create raw socket for EtherCAT frames */
   raw_sock = socket (AF_PACKET, SOCK_RAW, htons (ETH_P_ECAT));
   if (raw_sock < 0)
   {
      perror ("ecat: socket(AF_PACKET)");
      fprintf (stderr, "ecat: Need root/CAP_NET_RAW to open raw sockets\n");
      return -1;
   }

   /* Get interface index */
   memset (&ifr, 0, sizeof (ifr));
   strncpy (ifr.ifr_name, ifname, IFNAMSIZ - 1);
   if (ioctl (raw_sock, SIOCGIFINDEX, &ifr) < 0)
   {
      perror ("ecat: ioctl(SIOCGIFINDEX)");
      close (raw_sock);
      raw_sock = -1;
      return -1;
   }
   ifindex = ifr.ifr_ifindex;

   /* Bind to the interface */
   memset (&sll, 0, sizeof (sll));
   sll.sll_family   = AF_PACKET;
   sll.sll_ifindex  = ifindex;
   sll.sll_protocol = htons (ETH_P_ECAT);
   if (bind (raw_sock, (struct sockaddr *)&sll, sizeof (sll)) < 0)
   {
      perror ("ecat: bind");
      close (raw_sock);
      raw_sock = -1;
      return -1;
   }

   /* Set promiscuous mode on the interface */
   memset (&ifr, 0, sizeof (ifr));
   strncpy (ifr.ifr_name, ifname, IFNAMSIZ - 1);
   if (ioctl (raw_sock, SIOCGIFFLAGS, &ifr) == 0)
   {
      ifr.ifr_flags |= IFF_PROMISC;
      ioctl (raw_sock, SIOCSIFFLAGS, &ifr);
   }

   printf ("ecat: Bound to interface '%s' (index %d)\n", ifname, ifindex);

   /* Start receiver thread */
   ecat_running = 1;
   if (pthread_create (&ecat_thread, NULL, ecat_receiver_thread, NULL) != 0)
   {
      perror ("ecat: pthread_create");
      close (raw_sock);
      raw_sock = -1;
      return -1;
   }

   return 0;
}

void ESC_ecat_stop (void)
{
   int fd;
   struct timespec ts;

   /* Signal the thread to stop */
   ecat_running = 0;

   /* Close the socket – this will cause recvfrom() to return
    * with EBADF or ENOTSOCK, breaking the receive loop.
    */
   fd = raw_sock;
   raw_sock = -1;
   if (fd >= 0)
   {
      close (fd);
   }

   /* Try a timed join — give the thread 500ms to exit gracefully.
    * If it doesn't (e.g. stuck in recvfrom), cancel it.
    */
   clock_gettime (CLOCK_REALTIME, &ts);
   ts.tv_nsec += 500000000L;  /* 500 ms */
   if (ts.tv_nsec >= 1000000000L)
   {
      ts.tv_sec += 1;
      ts.tv_nsec -= 1000000000L;
   }

   if (pthread_timedjoin_np (ecat_thread, NULL, &ts) != 0)
   {
      printf ("ecat: Thread did not exit in time, cancelling...\n");
      pthread_cancel (ecat_thread);
      pthread_join (ecat_thread, NULL);
   }

   printf ("ecat: Frame processor stopped\n");
}
