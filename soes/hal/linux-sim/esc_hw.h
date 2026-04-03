/*
 * Licensed under the GNU General Public License version 2 with exceptions. See
 * LICENSE file in the project root for full license information
 */

/** \file
 * \brief
 * ESC hardware layer functions for simulated ESC on Linux.
 *
 * Provides a memory-backed ESC register space so the SOES slave stack can
 * run on any Linux machine (desktop, server, cloud VM, container) without
 * real EtherCAT hardware.
 */

#ifndef __ESC_HW_SIM_H__
#define __ESC_HW_SIM_H__

#include <stdint.h>

/*
 * Total simulated ESC memory size.
 * 0x0000 – 0x0FFF : ESC registers (4 KiB)
 * 0x1000 – 0x1FFF : Process data / mailbox RAM (4 KiB)
 */
#define ESC_SIM_MEM_SIZE    0x2000

/** Return pointer to the raw simulated ESC memory (for test harnesses). */
uint8_t * ESC_sim_get_mem (void);

/** Lock / unlock the ESC memory mutex.
 *  The frame processor thread MUST hold this lock when accessing ESC memory
 *  directly (via ESC_sim_get_mem) to prevent races with the main stack
 *  thread that calls ESC_read / ESC_write.
 */
void ESC_sim_lock (void);
void ESC_sim_unlock (void);

/** Inject a simulated AL Control event.
 *
 * A test harness or network adapter thread can call this to poke the
 * AL Control register and set the corresponding AL Event bit so the
 * stack processes a state-change request.
 *
 * @param[in] alcontrol  Value to write into AL Control (0x0120)
 */
void ESC_sim_set_alcontrol (uint16_t alcontrol);

/** Set the DL Status register.
 *
 * Useful to indicate "link up" (bit 0) after init so the stack leaves the
 * startup-wait loop.
 *
 * @param[in] dlstatus  Value to write into DL Status (0x0110)
 */
void ESC_sim_set_dlstatus (uint16_t dlstatus);

/** ESC EEPROM handler – to be registered as esc_hw_eep_handler callback. */
void ESC_eep_handler (void);

#endif /* __ESC_HW_SIM_H__ */
