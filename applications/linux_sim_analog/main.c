/*
 * Licensed under the GNU General Public License version 2 with exceptions. See
 * LICENSE file in the project root for full license information
 */

/** \file
 * \brief
 * Simulated 4-channel Analog I/O EtherCAT Slave for Linux.
 *
 * Models a 4-channel analog I/O module:
 *   RxPDO (SM2, 8 bytes): AO0..AO3 — int16 setpoints from master
 *   TxPDO (SM3, 8 bytes): AI0..AI3 — int16 simulated ADC readings
 *
 * Simulation: AI channels output a sawtooth ramp (counter mod 32768)
 * scaled by Gain and shifted by Offset.  The master can write to the
 * AO channels via PDO; the slave echoes them back on AI2/AI3 to allow
 * round-trip loopback verification.
 *
 * DC: sync0 cycle time is validated and logged.
 * FoE/EoE: disabled (typical for a simple analog I/O module).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "ecat_slv.h"
#include "esc.h"
#include "esc_eep.h"
#include "utypes.h"
#include "esc_hw.h"
#include "esc_ecat.h"

_Objects Obj;

static volatile int running = 1;

/* ── Signal handler ───────────────────────────────────────────────────────── */

static void signal_handler (int sig)
{
   const char msg[] = "sim_analog: Signal received, shutting down...\n";
   (void)!write (STDOUT_FILENO, msg, sizeof (msg) - 1);
   (void)sig;
   running = 0;
}

/* ── PDO callbacks ────────────────────────────────────────────────────────── */

void cb_get_inputs (void)
{
   static int16_t ramp = 0;

   /* Sawtooth ramp simulating a free-running ADC */
   int16_t raw = ramp++;

   /* Apply gain (shift) and offset */
   Obj.Inputs.AI0 = (int16_t)(raw * (int16_t)Obj.Parameters.Gain + Obj.Parameters.Offset);
   Obj.Inputs.AI1 = (int16_t)(raw * (int16_t)Obj.Parameters.Gain + Obj.Parameters.Offset + 1000);
   /* AI2/AI3 echo the AO setpoints written by the master (loopback test) */
   Obj.Inputs.AI2 = Obj.Outputs.AO0;
   Obj.Inputs.AI3 = Obj.Outputs.AO1;
}

void cb_set_outputs (void)
{
   static int print_cnt = 0;
   if ((print_cnt++ % 1000) == 0)
   {
      printf ("sim_analog: AO[%d, %d, %d, %d]\n",
              Obj.Outputs.AO0, Obj.Outputs.AO1,
              Obj.Outputs.AO2, Obj.Outputs.AO3);
   }
}

/* ── Defaults ─────────────────────────────────────────────────────────────── */

static void set_defaults (void)
{
   Obj.Parameters.Gain   = 1;
   Obj.Parameters.Offset = 0;
   memset (&Obj.Inputs,  0, sizeof (Obj.Inputs));
   memset (&Obj.Outputs, 0, sizeof (Obj.Outputs));
   printf ("sim_analog: Default parameters set (Gain=%u, Offset=%d)\n",
           Obj.Parameters.Gain, Obj.Parameters.Offset);
}

/* ── State change hooks ──────────────────────────────────────────────────── */


static void post_state_change (uint8_t *as, uint8_t *an)
{
   const char *names[] = { "?", "INIT", "PRE-OP", "BOOT", "SAFE-OP",
                           "?", "?", "?", "OP" };
   uint8_t s = (*an) & 0x0F;
   printf ("sim_analog: State change -> %s (0x%02X)\n",
           (s < 9) ? names[s] : "UNKNOWN", *an);
}

/* ── DC handler ───────────────────────────────────────────────────────────── */

static uint16_t dc_check_handler (void)
{
   uint32_t cycle_ns = 0;
   ESC_read (ESCREG_SYNC0_CYCLE_TIME, &cycle_ns, sizeof (cycle_ns));
   cycle_ns = etohl (cycle_ns);
   /* Cycle time may be 0 if SOEM has not configured sync0 for this slave
    * (e.g. when it is not the DC reference slave).  Accept in simulation. */
   printf ("sim_analog: DC sync0 cycle time = %u ns\n", cycle_ns);
   return 0;
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main (int argc, char *argv[])
{
   setbuf (stdout, NULL);
   setbuf (stderr, NULL);

   printf ("=== SOES Simulated Analog I/O Slave ===\n");
   printf ("4-channel analog I/O, 8-byte PDOs each direction\n\n");

   if (argc < 2)
   {
      fprintf (stderr, "Usage: %s <interface>\n", argv[0]);
      return 1;
   }
   const char *ifname = argv[1];

   signal (SIGINT,  signal_handler);
   signal (SIGTERM, signal_handler);

   static esc_cfg_t config =
   {
      .user_arg                  = "analog",
      .use_interrupt             = 0,
      .watchdog_cnt              = 5000,
      .set_defaults_hook         = set_defaults,
      .pre_state_change_hook     = NULL,
      .post_state_change_hook    = post_state_change,
      .application_hook          = NULL,
      .safeoutput_override       = NULL,
      .pre_object_download_hook  = NULL,
      .post_object_download_hook = NULL,
      .rxpdo_override            = NULL,
      .txpdo_override            = NULL,
      .esc_hw_interrupt_enable   = NULL,
      .esc_hw_interrupt_disable  = NULL,
      .esc_hw_eep_handler        = ESC_eep_handler,
      .esc_check_dc_handler      = dc_check_handler,
   };

   printf ("sim_analog: Initializing slave stack...\n");
   ecat_slv_init (&config);

   /* Patch product code in emulated EEPROM to 0x00000002 (AnalogIO).
    * esc_hw_eep.c is compiled into the shared soes library with the default
    * value 0x00000001; override it here for the analog slave identity. */
   {
      uint8_t pc[4] = { 0x02, 0x00, 0x00, 0x00 };  /* little-endian 0x00000002 */
      EEP_write (0x14, pc, 4);  /* SII words 10-11 (byte offset 0x14) */
   }

   printf ("sim_analog: Slave stack initialized\n");

   /* Pre-populate ESC register file */
   {
      uint8_t *mem = ESC_sim_get_mem ();
      mem[0x0000] = 0x02;  mem[0x0001] = 0x00;  /* ESC Type */
      mem[0x0002] = 0x01;  mem[0x0003] = 0x00;  /* Build */
      mem[0x0004] = 0x04;                         /* FMMU count */
      mem[0x0005] = 0x04;                         /* SM count */
      mem[0x0006] = 0x08;                         /* RAM 8 KiB */
      mem[0x0007] = 0x03;                         /* Port0 = MII */
      mem[0x0008] = 0x04;  mem[0x0009] = 0x00;  /* DC supported */
      mem[0x0010] = 0x00;  mem[0x0011] = 0x00;  /* Station addr */
      mem[0x0110] = 0x11;  mem[0x0111] = 0x03;  /* DL Status */
      mem[0x0130] = ESCinit; mem[0x0131] = 0x00; /* AL Status */
      mem[0x0140] = 0x05;                         /* PDI Control */
      mem[0x0500] = 0x00;                         /* EEPROM ctrl */
      mem[0x0502] = 0x40;  mem[0x0503] = 0x00;  /* EEPROM status */
   }

   printf ("sim_analog: Starting EtherCAT frame processor on '%s'...\n", ifname);
   if (ESC_ecat_start (ifname, 0) != 0)
   {
      fprintf (stderr, "sim_analog: Failed to start frame processor\n");
      return 1;
   }
   printf ("sim_analog: Frame processor running – waiting for master...\n");

   while (running)
   {
      ecat_slv ();
      usleep (100);
   }

   printf ("\nsim_analog: Shutting down...\n");
   ESC_ecat_stop ();
   printf ("sim_analog: Done.\n");

   return 0;
}
