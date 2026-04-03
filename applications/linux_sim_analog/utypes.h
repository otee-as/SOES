#ifndef __UTYPES_H__
#define __UTYPES_H__

#include "cc.h"

/*
 * Analog I/O slave object dictionary storage.
 *
 * Models a 4-channel analog I/O module:
 *   TxPDO (slave → master): 4 × int16 measured values (AI0..AI3)
 *   RxPDO (master → slave): 4 × int16 setpoint/DAC values (AO0..AO3)
 *   SDO    0x8000: channel scaling — Gain (uint16) and Offset (int16) per channel
 */

typedef struct
{
   /* Inputs (TxPDO – slave to master): 4 analog input channels */
   struct
   {
      int16_t AI0;
      int16_t AI1;
      int16_t AI2;
      int16_t AI3;
   } Inputs;

   /* Outputs (RxPDO – master to slave): 4 analog output channels */
   struct
   {
      int16_t AO0;
      int16_t AO1;
      int16_t AO2;
      int16_t AO3;
   } Outputs;

   /* Parameters (SDO): per-channel scaling */
   struct
   {
      uint32_t Gain;    /* scale factor applied to simulated ADC ramp */
      int16_t  Offset;  /* DC offset added to all input channels */
   } Parameters;

} _Objects;

extern _Objects Obj;

#endif /* __UTYPES_H__ */
