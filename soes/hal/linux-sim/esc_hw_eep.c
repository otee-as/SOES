/*
 * Licensed under the GNU General Public License version 2 with exceptions. See
 * LICENSE file in the project root for full license information
 */

/** \file
 * \brief
 * ESC hardware EEPROM emulation for simulated ESC on Linux.
 *
 * Stores the EEPROM content as a flat binary file on disk.
 * If the file doesn't exist on first run, a full SII EEPROM image
 * is generated with all categories that SOEM expects during discovery.
 */

#include "cc.h"
#include "esc.h"
#include "esc_eep.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* SII EEPROM emulated size: 4 KiB (2048 x 16-bit words) */
#define SIM_EEPROM_SIZE    4096

static uint8_t eep_data[SIM_EEPROM_SIZE];
static const char *eep_filename = "sii_eeprom.bin";

/* ---------- SII identity values (must match object dictionary 0x1018) ----- */
#define SII_VENDOR_ID      0x0000CAFE
#define SII_PRODUCT_CODE   0x00000001
#define SII_REVISION       0x00000001
#define SII_SERIAL         0x00000001

/* ---------- SII EEPROM builder -------------------------------------------- */

/** Helper: write a little-endian 16-bit word at byte offset in eep_data */
static void sii_put16 (unsigned offset, uint16_t val)
{
   eep_data[offset]     = (uint8_t)(val & 0xFF);
   eep_data[offset + 1] = (uint8_t)((val >> 8) & 0xFF);
}

/** Helper: write a little-endian 32-bit dword at byte offset in eep_data */
static void sii_put32 (unsigned offset, uint32_t val)
{
   eep_data[offset]     = (uint8_t)(val & 0xFF);
   eep_data[offset + 1] = (uint8_t)((val >> 8) & 0xFF);
   eep_data[offset + 2] = (uint8_t)((val >> 16) & 0xFF);
   eep_data[offset + 3] = (uint8_t)((val >> 24) & 0xFF);
}

/** Compute CRC-8/ITU over 'len' bytes starting at eep_data[0].
 *  The EtherCAT SII spec uses this over the first 14 bytes (words 0-6).
 */
static uint8_t sii_crc8 (unsigned len)
{
   uint8_t crc = 0xFF;
   unsigned i;
   for (i = 0; i < len; i++)
   {
      uint8_t b = eep_data[i];
      unsigned j;
      for (j = 0; j < 8; j++)
      {
         if ((crc ^ b) & 0x80)
            crc = (uint8_t)((crc << 1) ^ 0x07);
         else
            crc = (uint8_t)(crc << 1);
         b = (uint8_t)(b << 1);
      }
   }
   return crc;
}

/** Generate a complete SII EEPROM image with all categories SOEM needs.
 *
 * Layout (byte offsets, word addresses are offset/2):
 *
 *  Words  0-7   (bytes 0x00-0x0F):  Configuration area + CRC
 *  Words  8-15  (bytes 0x10-0x1F):  Vendor ID, Product Code, Revision, Serial
 *  Words 16-19  (bytes 0x20-0x27):  Bootstrap mailbox (if any)
 *  Words 20-23  (bytes 0x28-0x2F):  Standard mailbox config
 *                 Word 24 (0x30):   RxMbxAdr
 *                 Word 25 (0x31):   RxMbxSize  (= MBXSIZE)
 *                 Word 26 (0x32):   TxMbxAdr
 *                 Word 27 (0x33):   TxMbxSize
 *                 Word 28 (0x38):   Mailbox protocol  (CoE = bit2)
 *  Words 64+    (bytes 0x80+):      Categories
 */
static void generate_default_eeprom (void)
{
   unsigned wp;   /* write pointer in bytes */

   memset (eep_data, 0x00, sizeof (eep_data));

   /* ---- Configuration area (words 0-7, bytes 0x00-0x0F) ---- */
   sii_put16 (0x00, 0x0080);  /* Word 0: PDI Control (SPI slave) */
   sii_put16 (0x02, 0x0000);  /* Word 1: PDI Configuration */
   sii_put16 (0x04, 0x0000);  /* Word 2: Sync impulse length */
   sii_put16 (0x06, 0x0000);  /* Word 3: PDI Configuration 2 */
   sii_put16 (0x08, 0x0000);  /* Word 4: Configured Station Alias */
   sii_put16 (0x0A, 0x0000);  /* Word 5: Reserved */
   sii_put16 (0x0C, 0x0000);  /* Word 6: Reserved */
   /* Word 7 (bytes 0x0E-0x0F): CRC in low byte */
   eep_data[0x0E] = sii_crc8 (14);
   eep_data[0x0F] = 0x00;

   /* ---- Identity (words 8-15, bytes 0x10-0x1F) ---- */
   /* Word  8 (0x10): Vendor ID low */
   /* Word  9 (0x12): Vendor ID high */
   sii_put32 (0x10, SII_VENDOR_ID);    /* Words 8-9:  Vendor ID (ECT_SII_MANUF=0x0008) */
   sii_put32 (0x14, SII_PRODUCT_CODE); /* Words 10-11: Product Code (ECT_SII_ID=0x000a) */
   sii_put32 (0x18, SII_REVISION);     /* Words 12-13: Revision (ECT_SII_REV=0x000c) */
   sii_put32 (0x1C, SII_SERIAL);       /* Words 14-15: Serial (ECT_SII_SER=0x000e) */

   /* ---- Reserved words 16-19 (bytes 0x20-0x27) ---- */
   /* Execution delay, port0/1 delay, reserved - leave 0 */

   /* ---- Bootstrap mailbox (words 20-23, bytes 0x28-0x2F) ---- */
   /* ECT_SII_BOOTRXMBX = 0x0014 -> word 0x14 -> byte 0x28 */
   sii_put16 (0x28, 0x1000);  /* Bootstrap Rx Mbx offset */
   sii_put16 (0x2A, 0x0080);  /* Bootstrap Rx Mbx size */
   sii_put16 (0x2C, 0x1080);  /* Bootstrap Tx Mbx offset */
   sii_put16 (0x2E, 0x0080);  /* Bootstrap Tx Mbx size */

   /* ---- Standard mailbox (words 24-29, bytes 0x30-0x3B) ---- */
   /* ECT_SII_RXMBXADR = 0x0018 -> word 0x18 -> byte 0x30 */
   sii_put16 (0x30, 0x1000);  /* Standard Rx Mbx offset (MBX0_sma) */
   /* ECT_SII_MBXSIZE = 0x0019 -> word 0x19 -> byte 0x32 */
   sii_put16 (0x32, 0x0080);  /* Standard Rx Mbx size (128 = MBXSIZE) */
   /* ECT_SII_TXMBXADR = 0x001a -> word 0x1a -> byte 0x34 */
   sii_put16 (0x34, 0x1080);  /* Standard Tx Mbx offset (MBX1_sma) */
   sii_put16 (0x36, 0x0080);  /* Standard Tx Mbx size (128) */
   /* ECT_SII_MBXPROTO = 0x001c -> word 0x1c -> byte 0x38 */
   sii_put16 (0x38, 0x0004);  /* Mailbox protocols supported: bit2 = CoE */

   /* ---- Size field, version ---- */
   /* Words 0x1E, 0x1F (bytes 0x3C-0x3F) = EEPROM size word, version */
   sii_put16 (0x3C, 0x03FF);  /* EEPROM size in words - 1 (1024-1) */
   sii_put16 (0x3E, 0x0001);  /* Version */

   /* ==================================================================== */
   /* Categories start at word 0x0040 (byte 0x80) = ECT_SII_START          */
   /* Each category: [type:16][length_in_words:16][data...]                 */
   /* ==================================================================== */
   wp = 0x80;

   /* ---- Category: Strings (type 10 = 0x000A) ---- */
   {
      /* Strings: [nStrings:8] then for each: [len:8][chars...] */
      const char *s1 = "SOES SimSlave";          /* String 1: Device name */
      const char *s2 = "Simulated EtherCAT IO";  /* String 2: Group name */
      const char *s3 = "rev 1.0";                /* String 3: Image/order */
      unsigned s1len = (unsigned)strlen (s1);
      unsigned s2len = (unsigned)strlen (s2);
      unsigned s3len = (unsigned)strlen (s3);
      /* Total data bytes: 1(count) + 1+s1len + 1+s2len + 1+s3len */
      unsigned data_bytes = 1 + (1 + s1len) + (1 + s2len) + (1 + s3len);
      unsigned data_words = (data_bytes + 1) / 2; /* round up to word */
      unsigned dp;

      sii_put16 (wp, 0x000A);      /* category type = Strings */
      sii_put16 (wp + 2, (uint16_t)data_words);
      dp = wp + 4;

      eep_data[dp++] = 3;  /* number of strings */
      eep_data[dp++] = (uint8_t)s1len;
      memcpy (&eep_data[dp], s1, s1len); dp += s1len;
      eep_data[dp++] = (uint8_t)s2len;
      memcpy (&eep_data[dp], s2, s2len); dp += s2len;
      eep_data[dp++] = (uint8_t)s3len;
      memcpy (&eep_data[dp], s3, s3len); dp += s3len;

      wp += 4 + data_words * 2;
   }

   /* ---- Category: General (type 30 = 0x001E) ---- */
   {
      /* General category is 32 bytes (16 words) of fixed layout */
      unsigned gp;
      sii_put16 (wp, 0x001E);      /* category type = General */
      sii_put16 (wp + 2, 16);      /* length = 16 words */
      gp = wp + 4;

      memset (&eep_data[gp], 0, 32);  /* zero fill */

      /* Byte offsets within the General category data:
       *   SOEM reads at ssigen + N where ssigen points at the length word.
       *   So ssigen+2 = data[0].  The offsets below are relative to the
       *   data start (gp), matching what SOEM expects at ssigen+N = gp+(N-2).
       *
       *   ssigen+0x02 = data[0] = Group string index
       *   ssigen+0x03 = data[1] = Image string index
       *   ssigen+0x04 = data[2] = Order string index
       *   ssigen+0x05 = data[3] = Name string index
       *   ssigen+0x06 = data[4] = reserved
       *   ssigen+0x07 = data[5] = CoE Details
       *   ssigen+0x08 = data[6] = FoE Details
       *   ssigen+0x09 = data[7] = EoE Details
       *   ssigen+0x0A = data[8] = SoE Channels
       *   ssigen+0x0B = data[9] = DS402 Channels
       *   ssigen+0x0C = data[10]= SysmanClass
       *   ssigen+0x0D = data[11]= Flags
       *   ssigen+0x0E = data[12]= Ebus Current low
       *   ssigen+0x0F = data[13]= Ebus Current high
       *   ssigen+0x13 = data[17]= Physical Port config
       */
      eep_data[gp + 0] = 2;   /* Group string index (string 2) */
      eep_data[gp + 1] = 3;   /* Image string index (string 3) */
      eep_data[gp + 2] = 3;   /* Order string index (string 3) */
      eep_data[gp + 3] = 1;   /* Name string index  (string 1) */
      /* gp + 4: reserved */
      eep_data[gp + 5] = 0x27;  /* CoE Details: SDO + SDO Info + PDO Assign + SDO Complete */
      eep_data[gp + 6] = 0x00;  /* FoE Details */
      eep_data[gp + 7] = 0x00;  /* EoE Details */
      eep_data[gp + 8] = 0x00;  /* SoE Channels */
      eep_data[gp + 9] = 0x00;  /* DS402 Channels */
      eep_data[gp + 10] = 0x00; /* SysmanClass */
      eep_data[gp + 11] = 0x00; /* Flags (bit1=blockLRW) */
      /* gp + 12,13: Current on Ebus (mA) - little endian */
      sii_put16 (gp + 12, 0x0000);  /* 0 mA */
      /* gp + 14,15: reserved/group duplicate */
      /* gp + 16: reserved */
      /* gp + 17: Physical port config (ssigen+0x13 = gp+17) */
      eep_data[gp + 17] = 0x01;  /* Port 0: MII, others: not used */

      wp += 4 + 16 * 2;
   }

   /* ---- Category: FMMU (type 40 = 0x0028) ---- */
   {
      /* FMMU category: one byte per FMMU indicating purpose
       * 0x01 = Outputs (used for SM2/RxPDO)
       * 0x02 = Inputs  (used for SM3/TxPDO)
       * 0xFF = not used
       */
      sii_put16 (wp, 0x0028);  /* category type = FMMU */
      sii_put16 (wp + 2, 1);   /* length = 1 word (2 bytes = 2 FMMUs) */
      eep_data[wp + 4] = 0x01; /* FMMU0 -> Outputs */
      eep_data[wp + 5] = 0x02; /* FMMU1 -> Inputs */
      wp += 4 + 1 * 2;
   }

   /* ---- Category: SyncManager (type 41 = 0x0029) ---- */
   {
      /* Each SM entry in the SII is 8 bytes:
       *   PhysAddr[2] Length[2] CtrlReg[1] StatusReg[1] Enable[1] SMtype[1]
       * We define SM0..SM3.
       */
      sii_put16 (wp, 0x0029);  /* category type = SM */
      sii_put16 (wp + 2, 16);  /* length = 16 words (4 SMs * 8 bytes = 32 bytes) */
      unsigned sp = wp + 4;

      /* SM0: Mailbox Out (master -> slave) */
      sii_put16 (sp + 0, 0x1000);  /* PhysAddr = MBX0_sma */
      sii_put16 (sp + 2, 0x0080);  /* Length   = 128 */
      eep_data[sp + 4] = 0x26;     /* CtrlReg  = MBX0_smc (write, 3-buf, PDI int) */
      eep_data[sp + 5] = 0x00;     /* StatusReg */
      eep_data[sp + 6] = 0x01;     /* Enable = activated */
      eep_data[sp + 7] = 0x01;     /* SMtype 1 = Mailbox Out */
      sp += 8;

      /* SM1: Mailbox In (slave -> master) */
      sii_put16 (sp + 0, 0x1080);  /* PhysAddr = MBX1_sma */
      sii_put16 (sp + 2, 0x0080);  /* Length   = 128 */
      eep_data[sp + 4] = 0x22;     /* CtrlReg  = MBX1_smc (read, 3-buf, PDI int) */
      eep_data[sp + 5] = 0x00;     /* StatusReg */
      eep_data[sp + 6] = 0x01;     /* Enable */
      eep_data[sp + 7] = 0x02;     /* SMtype 2 = Mailbox In */
      sp += 8;

      /* SM2: Process Data Out (master -> slave) */
      /* SM2_sma and SM2_sml are defined in ecat_options.h per application */
#ifndef SM2_sml
#define SM2_sml 0x0005  /* default: 5 bytes (linux_sim: Control + Setpoint) */
#endif
      sii_put16 (sp + 0, SM2_sma);  /* PhysAddr from ecat_options.h */
      sii_put16 (sp + 2, SM2_sml);  /* Length from ecat_options.h */
      eep_data[sp + 4] = SM2_smc;   /* CtrlReg from ecat_options.h */
      eep_data[sp + 5] = 0x00;     /* StatusReg */
      eep_data[sp + 6] = 0x01;     /* Enable */
      eep_data[sp + 7] = 0x03;     /* SMtype 3 = Process Data Out */
      sp += 8;

      /* SM3: Process Data In (slave -> master) */
#ifndef SM3_sml
#define SM3_sml 0x0005  /* default: 5 bytes (linux_sim: Status + Value) */
#endif
      sii_put16 (sp + 0, SM3_sma);  /* PhysAddr from ecat_options.h */
      sii_put16 (sp + 2, SM3_sml);  /* Length from ecat_options.h */
      eep_data[sp + 4] = 0x20;     /* CtrlReg  = SM3_smc (read, buffered) */
      eep_data[sp + 5] = 0x00;     /* StatusReg */
      eep_data[sp + 6] = 0x01;     /* Enable */
      eep_data[sp + 7] = 0x04;     /* SMtype 4 = Process Data In */
      sp += 8;

      wp = sp;
   }

   /* ---- Category: TxPDO (type 50 = 0x0032) – Inputs (slave -> master) ---- */
   {
      /* PDO category entry:
       *   PDOIndex[2] nEntries[1] SM[1] DCSyncActivation[1] NameIdx[1]
       *   Flags[2]
       *   Then for each entry: Index[2] SubIndex[1] NameIdx[1] DataType[1]
       *                         BitLength[1] Flags[2]
       *
       * Actually the SII PDO format per ETG.2010:
       *   PDOIndex[2] nEntries[1] SM[1] reserved[1] NameIdx[1] Flags[2]
       *   Per entry: Index[2] SubIndex[1] NameIdx[1] DataType[1] BitLength[1] Flags[2]
       */
      unsigned n_entries = 2;  /* Status(8bit) + Value(32bit) */
      unsigned data_bytes = 8 + n_entries * 8;
      unsigned data_words = data_bytes / 2;
      unsigned pp;

      sii_put16 (wp, 0x0032);     /* category type = TxPDO (inputs) */
      sii_put16 (wp + 2, (uint16_t)data_words);
      pp = wp + 4;

      /* TxPDO 0x1A00 header */
      sii_put16 (pp + 0, 0x1A00);  /* PDO Index */
      eep_data[pp + 2] = (uint8_t)n_entries;  /* nEntries */
      eep_data[pp + 3] = 0x03;     /* SM assignment = SM3 */
      eep_data[pp + 4] = 0x00;     /* DC sync */
      eep_data[pp + 5] = 0x01;     /* Name string index = "SOES SimSlave" */
      sii_put16 (pp + 6, 0x0000);  /* Flags */
      pp += 8;

      /* Entry 1: 0x6000:01 Status, UNSIGNED8, 8 bits */
      sii_put16 (pp + 0, 0x6000);  /* Index */
      eep_data[pp + 2] = 0x01;     /* SubIndex */
      eep_data[pp + 3] = 0x00;     /* NameIdx */
      eep_data[pp + 4] = 0x05;     /* DataType = UNSIGNED8 (ETG.1000 type 0x0005) */
      eep_data[pp + 5] = 8;        /* BitLength */
      sii_put16 (pp + 6, 0x0000);  /* Flags */
      pp += 8;

      /* Entry 2: 0x6000:02 Value, INTEGER32, 32 bits */
      sii_put16 (pp + 0, 0x6000);  /* Index */
      eep_data[pp + 2] = 0x02;     /* SubIndex */
      eep_data[pp + 3] = 0x00;     /* NameIdx */
      eep_data[pp + 4] = 0x04;     /* DataType = INTEGER32 (ETG.1000 type 0x0004) */
      eep_data[pp + 5] = 32;       /* BitLength */
      sii_put16 (pp + 6, 0x0000);  /* Flags */
      pp += 8;

      wp = pp;
   }

   /* ---- Category: RxPDO (type 50 = 0x0032) – Outputs (master -> slave) -- */
   {
      unsigned n_entries = 2;  /* Control(8bit) + Setpoint(32bit) */
      unsigned data_bytes = 8 + n_entries * 8;
      unsigned data_words = data_bytes / 2;
      unsigned pp;

      sii_put16 (wp, 0x0033);     /* category type = RxPDO (outputs) */
      sii_put16 (wp + 2, (uint16_t)data_words);
      pp = wp + 4;

      /* RxPDO 0x1600 header */
      sii_put16 (pp + 0, 0x1600);  /* PDO Index */
      eep_data[pp + 2] = (uint8_t)n_entries;  /* nEntries */
      eep_data[pp + 3] = 0x02;     /* SM assignment = SM2 */
      eep_data[pp + 4] = 0x00;     /* DC sync */
      eep_data[pp + 5] = 0x01;     /* Name string index */
      sii_put16 (pp + 6, 0x0000);  /* Flags */
      pp += 8;

      /* Entry 1: 0x7000:01 Control, UNSIGNED8, 8 bits */
      sii_put16 (pp + 0, 0x7000);
      eep_data[pp + 2] = 0x01;
      eep_data[pp + 3] = 0x00;
      eep_data[pp + 4] = 0x05;     /* UNSIGNED8 */
      eep_data[pp + 5] = 8;
      sii_put16 (pp + 6, 0x0000);
      pp += 8;

      /* Entry 2: 0x7000:02 Setpoint, INTEGER32, 32 bits */
      sii_put16 (pp + 0, 0x7000);
      eep_data[pp + 2] = 0x02;
      eep_data[pp + 3] = 0x00;
      eep_data[pp + 4] = 0x04;     /* INTEGER32 */
      eep_data[pp + 5] = 32;
      sii_put16 (pp + 6, 0x0000);
      pp += 8;

      wp = pp;
   }

   /* ---- End category (type 0x7FFF) ---- */
   sii_put16 (wp, 0x7FFF);
   sii_put16 (wp + 2, 0x0000);

   printf ("soes_sim: Generated SII EEPROM image (%u bytes used of %u)\n",
           wp + 4, (unsigned)sizeof (eep_data));
}

/* ---------- File I/O helpers --------------------------------------------- */

static void eep_load_from_file (void)
{
   FILE *f = fopen (eep_filename, "rb");
   if (f != NULL)
   {
      size_t n = fread (eep_data, 1, sizeof (eep_data), f);
      fclose (f);
      printf ("soes_sim: Loaded EEPROM from '%s' (%u bytes)\n",
              eep_filename, (unsigned)n);
   }
   else
   {
      printf ("soes_sim: No EEPROM file '%s' found – generating defaults\n",
              eep_filename);
      generate_default_eeprom ();
   }
}

static void eep_save_to_file (void)
{
   FILE *f = fopen (eep_filename, "wb");
   if (f != NULL)
   {
      fwrite (eep_data, 1, sizeof (eep_data), f);
      fclose (f);
   }
}

/* ---------- Public EEPROM HAL interface ---------------------------------- */

/** Initialize EEPROM emulation – load from file or create defaults. */
void EEP_init (void)
{
   eep_load_from_file ();
}

/** Read from emulated EEPROM.
 *
 * @param[in]  addr   Byte offset into EEPROM
 * @param[out] data   Destination buffer
 * @param[in]  size   Number of bytes to read
 * @return 0 on success, -1 if out of range
 */
int8_t EEP_read (uint32_t addr, uint8_t *data, uint16_t size)
{
   if ((addr + size) > sizeof (eep_data))
   {
      return -1;
   }
   memcpy (data, &eep_data[addr], size);
   return 0;
}

/** Write to emulated EEPROM.
 *
 * @param[in]  addr   Byte offset into EEPROM
 * @param[in]  data   Source buffer
 * @param[in]  size   Number of bytes to write
 * @return 0 on success, -1 if out of range
 */
int8_t EEP_write (uint32_t addr, uint8_t *data, uint16_t size)
{
   if ((addr + size) > sizeof (eep_data))
   {
      return -1;
   }
   memcpy (&eep_data[addr], data, size);

   /* Persist to disk so changes survive restarts */
   eep_save_to_file ();
   return 0;
}
