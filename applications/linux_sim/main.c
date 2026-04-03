/*
 * Licensed under the GNU General Public License version 2 with exceptions. See
 * LICENSE file in the project root for full license information
 */

/** \file
 * \brief
 * Simulated EtherCAT Slave application for Linux.
 *
 * Runs the full SOES slave stack with a memory-backed ESC and a software
 * EtherCAT frame processor.  The frame processor opens a raw AF_PACKET
 * socket on a specified network interface, receives EtherCAT frames from
 * a real SOEM master, maps datagrams to ESC register reads/writes, and
 * returns modified frames with correct WKC.
 *
 * Usage:  sim_demo <interface>
 *         sim_demo eth0
 *         sim_demo veth1
 *
 * Build with:  cmake -DSIM_VARIANT=ON ..
 * Requires:    root or CAP_NET_RAW capability for raw sockets.
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
#include "esc_foe.h"
#include "utypes.h"
#include "esc_hw.h"
#include "esc_ecat.h"

/* Application variables */
_Objects Obj;

static volatile int running = 1;

/* ── FoE ────────────────────────────────────────────────────────────────── */
/*
 * Simple in-memory FoE sink.  Accepts writes to "firmware.bin" with password
 * 0 from any state (write_only_in_boot = 0), stores up to FOE_BUF_SIZE bytes.
 * Prints a confirmation on completion.
 */
#define FOE_BUF_SIZE 65536

static uint8_t  foe_recv_buf[FOE_BUF_SIZE];
static uint32_t foe_recv_total = 0;

/* write_function convention: return 0 on success, non-zero on failure */
static uint32_t foe_write_fn (foe_file_cfg_t *self, uint8_t *data, size_t length)
{
   (void)self;
   size_t avail = FOE_BUF_SIZE - foe_recv_total;
   if (length > avail)
   {
      return 1;  /* no space — signal error */
   }
   memcpy (foe_recv_buf + foe_recv_total, data, length);
   foe_recv_total += (uint32_t)length;
   return 0;  /* success */
}

static foe_file_cfg_t foe_files[] = {
   {
      .name               = "firmware.bin",
      .max_data           = FOE_BUF_SIZE,
      .dest_start_address = 0,
      .address_offset     = 0,
      .total_size         = 0,
      .filepass           = 0,
      .write_only_in_boot = 0,
      .padding            = 0,
      .write_function     = foe_write_fn,
   },
};

static uint8_t    foe_shared_buf[512];  /* internal FoE chunk buffer */
static foe_cfg_t  foe_config = {
   .fbuffer     = foe_shared_buf,
   .buffer_size = sizeof (foe_shared_buf),
   .n_files     = 1,
   .files       = foe_files,
};

/* ── DC ─────────────────────────────────────────────────────────────────── */
/*
 * DC check handler called by SOES during PREOP→SAFEOP when the master has
 * enabled DC synchronisation.  We accept any sync0 cycle time > 0 and record
 * it for diagnostic printing.
 */
static uint16_t dc_check_handler (void)
{
   /* Only validate DC cycle time when the master has actually activated DC
    * sync (ESCREG_SYNC_ACT bit 0 set).  During slave recovery ecx_reconfig_slave()
    * drives INIT→PREOP→SAFEOP without re-writing DC registers, so
    * SYNC0_CYCLE_TIME is 0 even though DC may have been used previously.
    * Checking activation first avoids a false ALERR_DCSYNC0CYCLETIME that
    * would block the SAFEOP transition and prevent recovery. */
   uint8_t sync_act = 0;
   ESC_read (ESCREG_SYNC_ACT, &sync_act, sizeof (sync_act));
   if (!(sync_act & ESCREG_SYNC_ACT_ACTIVATED))
   {
      /* DC not activated by master — nothing to check */
      return 0;
   }

   uint32_t cycle_ns = 0;
   ESC_read (ESCREG_SYNC0_CYCLE_TIME, &cycle_ns, sizeof (cycle_ns));
   cycle_ns = etohl (cycle_ns);
   if (cycle_ns == 0)
   {
      return ALERR_DCSYNC0CYCLETIME;
   }
   printf ("sim: DC sync0 cycle time = %u ns (%.3f ms)\n",
           cycle_ns, (double)cycle_ns / 1e6);
   return 0;
}

/* ---------- Signal handler ------------------------------------------------ */

static void signal_handler (int sig)
{
   /* Use write() not printf() — signal-handler-safe */
   const char msg[] = "sim: Signal received, shutting down...\n";
   (void)!write (STDOUT_FILENO, msg, sizeof (msg) - 1);
   (void)sig;
   running = 0;
}

/* ---------- Application callbacks ----------------------------------------- */

/** Called by DIG_process when it needs fresh input data from the application.
 *  We simulate a simple counter and echo the control byte as status.
 */
void cb_get_inputs (void)
{
   static int32_t counter = 0;

   /* Simulate: apply gain & offset to counter */
   Obj.Inputs.Value = (int32_t)(counter * (int32_t)Obj.Parameters.Gain
                                + (int32_t)Obj.Parameters.Offset);
   Obj.Inputs.Status = Obj.Outputs.Control;

   counter++;
}

/** Called by DIG_process when new output data has arrived from the master.
 *  In simulation we just log it periodically.
 */
void cb_set_outputs (void)
{
   static int print_cnt = 0;
   if ((print_cnt++ % 1000) == 0)
   {
      printf ("sim: Outputs – Control=0x%02X  Setpoint=%d\n",
              Obj.Outputs.Control, Obj.Outputs.Setpoint);
   }
}

/* ---------- Defaults hook ------------------------------------------------- */

static void set_defaults (void)
{
   Obj.Parameters.Gain   = 1;
   Obj.Parameters.Offset = 0;

   memset (&Obj.Inputs,  0, sizeof (Obj.Inputs));
   memset (&Obj.Outputs, 0, sizeof (Obj.Outputs));

   printf ("sim: Default parameters set (Gain=%u, Offset=%u)\n",
           Obj.Parameters.Gain, Obj.Parameters.Offset);
}

/* ---------- State change hooks -------------------------------------------- */

static void post_state_change (uint8_t *as, uint8_t *an)
{
   const char *state_names[] = {
      "?", "INIT", "PRE-OP", "BOOT", "SAFE-OP",
      "?", "?", "?", "OP"
   };
   uint8_t new_state = (*an) & 0x0F;
   const char *name = (new_state < 9) ? state_names[new_state] : "UNKNOWN";
   printf ("sim: State change -> %s (0x%02X)\n", name, *an);
}

/* ---------- Main ---------------------------------------------------------- */

int main (int argc, char *argv[])
{
   const char *ifname = NULL;

   /* Disable stdout buffering so output appears immediately in
    * Docker logs and piped/redirected environments.
    */
   setbuf (stdout, NULL);
   setbuf (stderr, NULL);

   printf ("=== SOES Simulated EtherCAT Slave ===\n");
   printf ("Software ESC with EtherCAT frame processor\n\n");

   if (argc < 2)
   {
      fprintf (stderr, "Usage: %s <interface>\n", argv[0]);
      fprintf (stderr, "Example: %s eth0\n", argv[0]);
      fprintf (stderr, "         %s veth1\n", argv[0]);
      return 1;
   }
   ifname = argv[1];

   signal (SIGINT,  signal_handler);
   signal (SIGTERM, signal_handler);

   static esc_cfg_t config =
   {
      .user_arg = "sim",
      .use_interrupt = 0,
      /* Watchdog: counts down by 1 per ecat_slv() call (every ~100us).
       * A non-RT kernel can oversleep usleep(2000) by 100ms+, easily
       * exceeding a tight 15ms window (150 * 100us).  5000 * 100us = 500ms
       * is tolerant of scheduling jitter while still catching a dead master. */
      .watchdog_cnt = 5000,
      .set_defaults_hook = set_defaults,
      .pre_state_change_hook = NULL,
      .post_state_change_hook = post_state_change,
      .application_hook = NULL,
      .safeoutput_override = NULL,
      .pre_object_download_hook = NULL,
      .post_object_download_hook = NULL,
      .rxpdo_override = NULL,
      .txpdo_override = NULL,
      .esc_hw_interrupt_enable = NULL,
      .esc_hw_interrupt_disable = NULL,
      .esc_hw_eep_handler = ESC_eep_handler,
      .esc_check_dc_handler = dc_check_handler,
   };

   printf ("sim: Initializing slave stack...\n");
   FOE_config (&foe_config);
   ecat_slv_init (&config);
   printf ("sim: Slave stack initialized\n");

   /* Pre-populate the ESC Type register (0x0000) so BRD to ECT_REG_TYPE
    * returns a non-zero value (SOEM uses WKC of this BRD to count slaves).
    * Register 0x0000-0x0001: ESC Type (e.g., 0x0002 = ET1100 compatible)
    */
   {
      uint8_t *mem = ESC_sim_get_mem ();
      /* Type register: 0x0002 = "IP Core" type, Revision 0 */
      mem[0x0000] = 0x02;
      mem[0x0001] = 0x00;
      /* Build register (0x0002-0x0003) */
      mem[0x0002] = 0x01;
      mem[0x0003] = 0x00;
      /* Number of supported FMMUs (0x0004) */
      mem[0x0004] = 0x04;
      /* Number of supported SyncManagers (0x0005) */
      mem[0x0005] = 0x04;
      /* RAM size (0x0006) = 8 (unit: KiB) */
      mem[0x0006] = 0x08;
      /* Port descriptor (0x0007): Port0 = MII (0x03) */
      mem[0x0007] = 0x03;
      /* ESC features supported (0x0008-0x0009):
       * bit 2 = DC supported, bit 6 = DC 64-bit
       * Set 0x0004 = DC supported
       */
      mem[0x0008] = 0x04;
      mem[0x0009] = 0x00;

      /* Station address (0x0010) = 0 initially */
      mem[0x0010] = 0x00;
      mem[0x0011] = 0x00;

      /* DL Status (0x0110): port0 open + comm established */
      mem[0x0110] = 0x11;
      mem[0x0111] = 0x03;

      /* AL Status (0x0130): INIT state */
      mem[0x0130] = ESCinit;
      mem[0x0131] = 0x00;

      /* PDI Control (0x0140): SPI slave = 0x05 */
      mem[0x0140] = 0x05;

      /* EEPROM config (0x0500): PDI has EEPROM control = 0 (master has it) */
      mem[0x0500] = 0x00;

      /* EEPROM status (0x0502): 8-byte read supported (bit 6 = 1), not busy */
      mem[0x0502] = 0x40;  /* EC_ESTAT_R64 = 0x0040 */
      mem[0x0503] = 0x00;
   }

   /* Start the EtherCAT frame processor on the specified interface */
   printf ("sim: Starting EtherCAT frame processor on '%s'...\n", ifname);
   if (ESC_ecat_start (ifname, 0) != 0)
   {
      fprintf (stderr, "sim: Failed to start frame processor\n");
      return 1;
   }
   printf ("sim: Frame processor running – waiting for master...\n");

   /* Main slave loop – drives SOES state machine, EEPROM emulation,
    * mailbox processing, PDO exchange, etc. */
   while (running)
   {
      ecat_slv ();
      usleep (100);  /* 100 us polling – fast enough for master timeouts */
   }

   printf ("\nsim: Shutting down...\n");
   printf ("sim: FoE total received: %u bytes\n", foe_recv_total);
   ESC_ecat_stop ();
   printf ("sim: Done.\n");

   return 0;
}
