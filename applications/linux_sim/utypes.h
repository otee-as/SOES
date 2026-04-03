#ifndef __UTYPES_H__
#define __UTYPES_H__

#include "cc.h"

/* Object dictionary storage */

typedef struct
{
   /* Inputs (TxPDO – slave to master) */
   struct
   {
      uint8_t  Status;
      int32_t  Value;
   } Inputs;

   /* Outputs (RxPDO – master to slave) */
   struct
   {
      uint8_t  Control;
      int32_t  Setpoint;
   } Outputs;

   /* Parameters (SDO read/write) */
   struct
   {
      uint32_t Gain;
      uint32_t Offset;
   } Parameters;

} _Objects;

extern _Objects Obj;

#endif /* __UTYPES_H__ */
