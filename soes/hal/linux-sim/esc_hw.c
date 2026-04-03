/*
 * Licensed under the GNU General Public License version 2 with exceptions. See
 * LICENSE file in the project root for full license information
 */

/** \file
 * \brief
 * ESC hardware layer functions for simulated ESC on Linux.
 *
 * Replaces the LAN9252 / ET1100 hardware with a simple in-memory buffer.
 * All ESC_read / ESC_write calls operate on this buffer, allowing the full
 * SOES slave stack to run on any Linux machine without EtherCAT hardware.
 */

#include "esc.h"
#include "esc_hw.h"
#include "esc_eep.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

/* ---- Simulated ESC memory ------------------------------------------------ */

static uint8_t esc_mem[ESC_SIM_MEM_SIZE];
static pthread_mutex_t esc_mutex = PTHREAD_MUTEX_INITIALIZER;

uint8_t * ESC_sim_get_mem (void)
{
   return esc_mem;
}

void ESC_sim_lock (void)
{
   pthread_mutex_lock (&esc_mutex);
}

void ESC_sim_unlock (void)
{
   pthread_mutex_unlock (&esc_mutex);
}

/* ---- Helper: little-endian write into esc_mem ---------------------------- */

static void mem_write_le16 (uint16_t address, uint16_t value)
{
   if ((uint32_t)address + 2u <= ESC_SIM_MEM_SIZE)
   {
      esc_mem[address]     = (uint8_t)(value & 0xFF);
      esc_mem[address + 1] = (uint8_t)((value >> 8) & 0xFF);
   }
}

static uint16_t mem_read_le16 (uint16_t address)
{
   if ((uint32_t)address + 2u <= ESC_SIM_MEM_SIZE)
   {
      return (uint16_t)(esc_mem[address] | ((uint16_t)esc_mem[address + 1] << 8));
   }
   return 0;
}

/* ---- Simulation control API ---------------------------------------------- */

void ESC_sim_set_alcontrol (uint16_t alcontrol)
{
   /* Write AL Control register (0x0120) in little-endian */
   mem_write_le16 (ESCREG_ALCONTROL, alcontrol);

   /* Set the AL Control event bit in AL Event register (0x0220) */
   uint16_t alevent = mem_read_le16 (ESCREG_ALEVENT);
   alevent |= ESCREG_ALEVENT_CONTROL;
   mem_write_le16 (ESCREG_ALEVENT, alevent);
}

void ESC_sim_set_dlstatus (uint16_t dlstatus)
{
   mem_write_le16 (ESCREG_DLSTATUS, dlstatus);
}

/* ---- ESC_read / ESC_write (called by the stack) -------------------------- */

/** ESC read function used by the Slave stack.
 *
 * @param[in]   address  = address of ESC register to read
 * @param[out]  buf      = pointer to buffer to read in
 * @param[in]   len      = number of bytes to read
 */
void ESC_read (uint16_t address, void *buf, uint16_t len)
{
   pthread_mutex_lock (&esc_mutex);

   if (((uint32_t)address + len) <= ESC_SIM_MEM_SIZE)
   {
      memcpy (buf, &esc_mem[address], len);
   }
   else
   {
      memset (buf, 0, len);
   }

   /* Mimic ET1100 behaviour: provide ALevent on every read.
    *
    * In a real ESC the AL Event register (0x0220) continuously reflects
    * pending event conditions.  Specific bits are cleared only when the
    * slave acknowledges the condition (e.g. reading AL Control clears the
    * CONTROL bit, reading EEPROM registers clears the EEP bit, reading
    * SM Activate clears the corresponding SM bit).
    *
    * We OR the esc_mem ALEVENT into ESCvar.ALevent so that bits accumulate
    * until explicitly consumed.
    */
   if (address != ESCREG_ALEVENT)
   {
      uint16_t raw_event = (uint16_t)(esc_mem[ESCREG_ALEVENT]
                           | ((uint16_t)esc_mem[ESCREG_ALEVENT + 1] << 8));
      ESCvar.ALevent = etohs (raw_event);
   }

   /* Clear specific ALevent bits based on what register was just read,
    * mimicking the hardware auto-acknowledge behaviour.
    */
   if (address == ESCREG_ALCONTROL)
   {
      /* Reading AL Control acknowledges the control event */
      uint16_t ev = (uint16_t)(esc_mem[ESCREG_ALEVENT]
                    | ((uint16_t)esc_mem[ESCREG_ALEVENT + 1] << 8));
      ev &= (uint16_t)~ESCREG_ALEVENT_CONTROL;
      esc_mem[ESCREG_ALEVENT]     = (uint8_t)(ev & 0xFF);
      esc_mem[ESCREG_ALEVENT + 1] = (uint8_t)((ev >> 8) & 0xFF);
   }
   else if (address == ESCREG_EECONTSTAT)
   {
      /* Reading EEPROM control/status acknowledges the EEP event */
      uint16_t ev = (uint16_t)(esc_mem[ESCREG_ALEVENT]
                    | ((uint16_t)esc_mem[ESCREG_ALEVENT + 1] << 8));
      ev &= (uint16_t)~ESCREG_ALEVENT_EEP;
      esc_mem[ESCREG_ALEVENT]     = (uint8_t)(ev & 0xFF);
      esc_mem[ESCREG_ALEVENT + 1] = (uint8_t)((ev >> 8) & 0xFF);
   }
   else if (address >= 0x0800 && address < (0x0800 + 4 * 8))
   {
      /* Reading SM registers (0x0800+): if reading an SM Activate register,
       * clear the corresponding SM event bit.
       */
      int sm_idx = (int)((address - 0x0800) / 8);
      uint16_t sm_base_addr = (uint16_t)(0x0800 + sm_idx * 8);
      /* SM Activate is at offset +6 within the 8-byte SM block */
      if (address == (uint16_t)(sm_base_addr + 6))
      {
         uint16_t ev = (uint16_t)(esc_mem[ESCREG_ALEVENT]
                       | ((uint16_t)esc_mem[ESCREG_ALEVENT + 1] << 8));
         ev &= (uint16_t)~(0x0100 << sm_idx);  /* Clear SMn event bit */
         esc_mem[ESCREG_ALEVENT]     = (uint8_t)(ev & 0xFF);
         esc_mem[ESCREG_ALEVENT + 1] = (uint8_t)((ev >> 8) & 0xFF);
      }
   }

   /* PDI-side SM data read: When the slave stack reads from an SM data area
    * (e.g., SM0 mailbox), clear the SM status MBXstat bit to indicate
    * the PDI has consumed the data.  This is what real ESC hardware does
    * automatically.
    */
   {
      int si;
      for (si = 0; si < 4; si++)
      {
         uint16_t sm_base = (uint16_t)(0x0800 + si * 8);
         uint16_t sm_addr = (uint16_t)(esc_mem[sm_base]
                            | ((uint16_t)esc_mem[sm_base + 1] << 8));
         uint16_t sm_len  = (uint16_t)(esc_mem[sm_base + 2]
                            | ((uint16_t)esc_mem[sm_base + 3] << 8));
         if (sm_len == 0) continue;
         if (address < (sm_addr + sm_len) && ((uint32_t)address + len) > sm_addr)
         {
            /* PDI read overlaps this SM data area — clear MBXstat */
            esc_mem[sm_base + 5] &= (uint8_t)~0x08;
         }
      }
   }

   pthread_mutex_unlock (&esc_mutex);
}

/** ESC write function used by the Slave stack.
 *
 * @param[in]   address  = address of ESC register to write
 * @param[out]  buf      = pointer to buffer to write from
 * @param[in]   len      = number of bytes to write
 */
void ESC_write (uint16_t address, void *buf, uint16_t len)
{
   pthread_mutex_lock (&esc_mutex);

   if (((uint32_t)address + len) <= ESC_SIM_MEM_SIZE)
   {
      memcpy (&esc_mem[address], buf, len);
   }

   /* PDI-side SM data write: When the slave stack writes to an SM data area
    * (e.g., SM1 mailbox out), set the SM status MBXstat bit to indicate
    * there is new data for the master to read.  Real ESC hardware does this
    * automatically.
    *
    * Additionally, clear the corresponding SM event bit in ALevent.
    * In real ESC hardware, a PDI write to the outbox resets the SM state
    * machine for that SM channel, clearing any pending event from a
    * previous master read.  Without this, a stale SM1 event (e.g. from
    * a master "drain" read) would cause the slave stack to immediately
    * ack its own freshly written response, corrupting it.
    */
   {
      int si;
      for (si = 0; si < 4; si++)
      {
         uint16_t sm_base = (uint16_t)(0x0800 + si * 8);
         uint16_t sm_addr = (uint16_t)(esc_mem[sm_base]
                            | ((uint16_t)esc_mem[sm_base + 1] << 8));
         uint16_t sm_len  = (uint16_t)(esc_mem[sm_base + 2]
                            | ((uint16_t)esc_mem[sm_base + 3] << 8));
         if (sm_len == 0) continue;
         if (address < (sm_addr + sm_len) && ((uint32_t)address + len) > sm_addr)
         {
            /* In real ESC hardware, the SM state machine triggers (sets
             * MBXstat / buffer status) only when the LAST byte of the SM
             * data area is written.  This distinguishes a real mailbox post
             * (ESC_writembx writes the full region including the last byte)
             * from an ack/clear write (ESC_ackmbxread writes only byte 0).
             *
             * Without this check, ESC_ackmbxread's 1-byte write would
             * falsely set MBXstat=1, causing the master to read garbage.
             */
            uint16_t sm_last = (uint16_t)(sm_addr + sm_len - 1);
            if (address <= sm_last && ((uint32_t)address + len) > sm_last)
            {
               /* Write covers the last byte — set MBXstat */
               esc_mem[sm_base + 5] |= 0x08;
            }

            /* Clear the SMn event bit in ALevent so that stale events
             * from previous master reads don't cause premature ack.
             */
            uint16_t ev = (uint16_t)(esc_mem[ESCREG_ALEVENT]
                          | ((uint16_t)esc_mem[ESCREG_ALEVENT + 1] << 8));
            ev &= (uint16_t)~(0x0100 << si);
            esc_mem[ESCREG_ALEVENT]     = (uint8_t)(ev & 0xFF);
            esc_mem[ESCREG_ALEVENT + 1] = (uint8_t)((ev >> 8) & 0xFF);
         }
      }
   }

   /* Mimic ESC hardware behaviour for EEPROM control/status register:
    * When the PDI (slave stack) writes to ESCREG_EECONTSTAT (0x0502),
    * the real ESC automatically clears the busy bit to acknowledge that
    * the command has been processed.  Without this, EEP_process() loops
    * forever because it writes back the status with busy still set.
    */
   if (address == ESCREG_EECONTSTAT || (address < ESCREG_EECONTSTAT &&
       (address + len) > ESCREG_EECONTSTAT))
   {
      /* Clear busy bit (bit 15 of the 16-bit register at 0x0502-0x0503).
       * Busy is bit 7 of byte 0x0503.
       */
      esc_mem[ESCREG_EECONTSTAT + 1] &= 0x7F;
   }

   /* Snapshot ALevent on every write too, so ESCvar.ALevent stays current. */
   {
      uint16_t raw_event = (uint16_t)(esc_mem[ESCREG_ALEVENT]
                           | ((uint16_t)esc_mem[ESCREG_ALEVENT + 1] << 8));
      ESCvar.ALevent = etohs (raw_event);
   }

   /* If the stack writes to AL Event (0x0220), that's an explicit set/clear
    * of event bits from PDI side — allow it through (already written above
    * via memcpy).
    */

   pthread_mutex_unlock (&esc_mutex);
}

/** ESC reset – clear the simulated memory */
void ESC_reset (void)
{
   memset (esc_mem, 0, sizeof (esc_mem));
}

/** ESC init – prepare the simulated ESC for operation.
 *
 * @param[in] config  = stack configuration (user_arg unused for sim)
 */
void ESC_init (const esc_cfg_t * config)
{
   (void)config;

   /* Start with clean memory */
   memset (esc_mem, 0, sizeof (esc_mem));

   /* Set DL Status to "link up" (bit 0 = 1) and signal on all ports
    * so the startup wait-loop in ecat_slv_init() completes immediately.
    * Bits 4,5 indicate port 0 link, bits 8,9 indicate port 1 link.
    */
   ESC_sim_set_dlstatus (0x0311);

   /* Initialize EEPROM emulation */
   EEP_init ();

   printf ("soes_sim: ESC_init – simulated ESC memory initialized (%u bytes)\n",
           (unsigned)sizeof (esc_mem));
}

/** ESC EEPROM handler – called from ecat_slv main loop via callback.
 *  Processes EEPROM read/write/reload commands from the master.
 */
void ESC_eep_handler (void)
{
   EEP_process ();
}
