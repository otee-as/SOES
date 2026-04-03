#include "esc_coe.h"
#include "utypes.h"
#include <stddef.h>

/*
 * Object dictionary for the analog I/O simulated slave.
 *
 * Identity:
 *   Vendor  0x0000CAFE  (same vendor as sim slave)
 *   Product 0x00000002  (analog I/O module)
 *   Rev     0x00000001
 *
 * PDO layout:
 *   RxPDO 0x1600: AO0..AO3  (4×int16, 8 bytes)  — master → slave
 *   TxPDO 0x1A00: AI0..AI3  (4×int16, 8 bytes)  — slave → master
 *
 * Parameters (SDO 0x8000):
 *   :01 Gain   uint16  applied to simulated ADC ramp
 *   :02 Offset int16   DC offset added to all AI channels
 */

#ifndef HW_REV
#define HW_REV "1.0"
#endif
#ifndef SW_REV
#define SW_REV "1.0"
#endif

static const char acName1000[] = "Device Type";
static const char acName1008[] = "Device Name";
static const char acName1009[] = "Hardware Version";
static const char acName100A[] = "Software Version";
static const char acName1018[] = "Identity Object";
static const char acName1018_00[] = "Max SubIndex";
static const char acName1018_01[] = "Vendor ID";
static const char acName1018_02[] = "Product Code";
static const char acName1018_03[] = "Revision Number";
static const char acName1018_04[] = "Serial Number";

/* RxPDO mapping 0x1600: Analog Outputs (master → slave) */
static const char acName1600[]    = "Analog Outputs";
static const char acName1600_00[] = "Max SubIndex";
static const char acName1600_01[] = "AO0";
static const char acName1600_02[] = "AO1";
static const char acName1600_03[] = "AO2";
static const char acName1600_04[] = "AO3";

/* TxPDO mapping 0x1A00: Analog Inputs (slave → master) */
static const char acName1A00[]    = "Analog Inputs";
static const char acName1A00_00[] = "Max SubIndex";
static const char acName1A00_01[] = "AI0";
static const char acName1A00_02[] = "AI1";
static const char acName1A00_03[] = "AI2";
static const char acName1A00_04[] = "AI3";

static const char acName1C00[]    = "Sync Manager Communication Type";
static const char acName1C00_00[] = "Max SubIndex";
static const char acName1C00_01[] = "Communications Type SM0";
static const char acName1C00_02[] = "Communications Type SM1";
static const char acName1C00_03[] = "Communications Type SM2";
static const char acName1C00_04[] = "Communications Type SM3";

static const char acName1C12[]    = "Sync Manager 2 PDO Assignment";
static const char acName1C12_00[] = "Max SubIndex";
static const char acName1C12_01[] = "PDO Mapping";

static const char acName1C13[]    = "Sync Manager 3 PDO Assignment";
static const char acName1C13_00[] = "Max SubIndex";
static const char acName1C13_01[] = "PDO Mapping";

/* Process data objects */
static const char acName6000[]    = "Analog Inputs";
static const char acName6000_00[] = "Max SubIndex";
static const char acName6000_01[] = "AI0";
static const char acName6000_02[] = "AI1";
static const char acName6000_03[] = "AI2";
static const char acName6000_04[] = "AI3";

static const char acName7000[]    = "Analog Outputs";
static const char acName7000_00[] = "Max SubIndex";
static const char acName7000_01[] = "AO0";
static const char acName7000_02[] = "AO1";
static const char acName7000_03[] = "AO2";
static const char acName7000_04[] = "AO3";

static const char acName8000[]    = "Parameters";
static const char acName8000_00[] = "Max SubIndex";
static const char acName8000_01[] = "Gain";
static const char acName8000_02[] = "Offset";

/* ── SDO objects ──────────────────────────────────────────────────────────── */

const _objd SDO1000[] =
{
   {0x0, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1000, 0x00001389, NULL},
};
const _objd SDO1008[] =
{
   {0x0, DTYPE_VISIBLE_STRING, 128, ATYPE_RO, acName1008, 0, "SOES AnalogIO"},
};
const _objd SDO1009[] =
{
   {0x0, DTYPE_VISIBLE_STRING, 0, ATYPE_RO, acName1009, 0, HW_REV},
};
const _objd SDO100A[] =
{
   {0x0, DTYPE_VISIBLE_STRING, 0, ATYPE_RO, acName100A, 0, SW_REV},
};
const _objd SDO1018[] =
{
   {0x00, DTYPE_UNSIGNED8,  8,  ATYPE_RO, acName1018_00, 4,          NULL},
   {0x01, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1018_01, 0x0000CAFE, NULL},
   {0x02, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1018_02, 0x0002,     NULL},
   {0x03, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1018_03, 0x0001,     NULL},
   {0x04, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1018_04, 0x00000002, NULL},
};

/* RxPDO 0x1600: 4×int16 Analog Outputs (master → slave)
 * Encoding: 0x7000_xx_10  = object 0x7000, sub xx, 16 bits */
const _objd SDO1600[] =
{
   {0x00, DTYPE_UNSIGNED8,  8,  ATYPE_RO, acName1600_00, 4,          NULL},
   {0x01, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1600_01, 0x70000110, NULL},
   {0x02, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1600_02, 0x70000210, NULL},
   {0x03, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1600_03, 0x70000310, NULL},
   {0x04, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1600_04, 0x70000410, NULL},
};

/* TxPDO 0x1A00: 4×int16 Analog Inputs (slave → master)
 * Encoding: 0x6000_xx_10  = object 0x6000, sub xx, 16 bits */
const _objd SDO1A00[] =
{
   {0x00, DTYPE_UNSIGNED8,  8,  ATYPE_RO, acName1A00_00, 4,          NULL},
   {0x01, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1A00_01, 0x60000110, NULL},
   {0x02, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1A00_02, 0x60000210, NULL},
   {0x03, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1A00_03, 0x60000310, NULL},
   {0x04, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1A00_04, 0x60000410, NULL},
};

const _objd SDO1C00[] =
{
   {0x00, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1C00_00, 4, NULL},
   {0x01, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1C00_01, 1, NULL},
   {0x02, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1C00_02, 2, NULL},
   {0x03, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1C00_03, 3, NULL},
   {0x04, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1C00_04, 4, NULL},
};

const _objd SDO1C12[] =
{
   {0x00, DTYPE_UNSIGNED8,  8,  ATYPE_RO, acName1C12_00, 1,      NULL},
   {0x01, DTYPE_UNSIGNED16, 16, ATYPE_RO, acName1C12_01, 0x1600, NULL},
};

const _objd SDO1C13[] =
{
   {0x00, DTYPE_UNSIGNED8,  8,  ATYPE_RO, acName1C13_00, 1,      NULL},
   {0x01, DTYPE_UNSIGNED16, 16, ATYPE_RO, acName1C13_01, 0x1A00, NULL},
};

/* Process data: TxPDO (slave → master) */
const _objd SDO6000[] =
{
   {0x00, DTYPE_UNSIGNED8, 8,  ATYPE_RO, acName6000_00, 4, NULL},
   {0x01, DTYPE_INTEGER16, 16, ATYPE_RO, acName6000_01, 0, &Obj.Inputs.AI0},
   {0x02, DTYPE_INTEGER16, 16, ATYPE_RO, acName6000_02, 0, &Obj.Inputs.AI1},
   {0x03, DTYPE_INTEGER16, 16, ATYPE_RO, acName6000_03, 0, &Obj.Inputs.AI2},
   {0x04, DTYPE_INTEGER16, 16, ATYPE_RO, acName6000_04, 0, &Obj.Inputs.AI3},
};

/* Process data: RxPDO (master → slave) */
const _objd SDO7000[] =
{
   {0x00, DTYPE_UNSIGNED8, 8,  ATYPE_RO, acName7000_00, 4, NULL},
   {0x01, DTYPE_INTEGER16, 16, ATYPE_RO, acName7000_01, 0, &Obj.Outputs.AO0},
   {0x02, DTYPE_INTEGER16, 16, ATYPE_RO, acName7000_02, 0, &Obj.Outputs.AO1},
   {0x03, DTYPE_INTEGER16, 16, ATYPE_RO, acName7000_03, 0, &Obj.Outputs.AO2},
   {0x04, DTYPE_INTEGER16, 16, ATYPE_RO, acName7000_04, 0, &Obj.Outputs.AO3},
};

/* Parameters */
const _objd SDO8000[] =
{
   {0x00, DTYPE_UNSIGNED8,  8,  ATYPE_RO, acName8000_00, 2, NULL},
   {0x01, DTYPE_UNSIGNED32, 32, ATYPE_RW, acName8000_01, 1, &Obj.Parameters.Gain},
   {0x02, DTYPE_INTEGER16,  16, ATYPE_RW, acName8000_02, 0, &Obj.Parameters.Offset},
};

const _objectlist SDOobjects[] =
{
   {0x1000, OTYPE_VAR,    0, 0, acName1000, SDO1000},
   {0x1008, OTYPE_VAR,    0, 0, acName1008, SDO1008},
   {0x1009, OTYPE_VAR,    0, 0, acName1009, SDO1009},
   {0x100A, OTYPE_VAR,    0, 0, acName100A, SDO100A},
   {0x1018, OTYPE_RECORD, 4, 0, acName1018, SDO1018},
   {0x1600, OTYPE_RECORD, 4, 0, acName1600, SDO1600},
   {0x1A00, OTYPE_RECORD, 4, 0, acName1A00, SDO1A00},
   {0x1C00, OTYPE_ARRAY,  4, 0, acName1C00, SDO1C00},
   {0x1C12, OTYPE_ARRAY,  1, 0, acName1C12, SDO1C12},
   {0x1C13, OTYPE_ARRAY,  1, 0, acName1C13, SDO1C13},
   {0x6000, OTYPE_RECORD, 4, 0, acName6000, SDO6000},
   {0x7000, OTYPE_RECORD, 4, 0, acName7000, SDO7000},
   {0x8000, OTYPE_RECORD, 2, 0, acName8000, SDO8000},
   {0xffff, 0xff, 0xff, 0xff, NULL, NULL}
};
