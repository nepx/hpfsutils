/* fat.h -- FAT definitions
   Copyright (c) 1995-2008 by Eberhard Mattes

This file is part of fst.

fst is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

fst is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with fst; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */


#pragma pack(1)

#ifdef __GNUC__
/* For GCC ports which don't support #pragma pack(1) */
#define PACKED __attribute((packed))
#else
#define PACKED
#endif

typedef struct
{
  BYTE name[8+3] PACKED;
  BYTE attr PACKED;
  BYTE reserved[8] PACKED;
  USHORT ea PACKED;             /* Or upper 16 bits of cluster for FAT32 */
  USHORT time PACKED;
  USHORT date PACKED;
  USHORT cluster16 PACKED;
  ULONG size PACKED;
} FAT_DIRENT;

typedef struct
{
  BYTE flag PACKED;
  USHORT name1[5] PACKED;
  BYTE attr PACKED;
  BYTE reserved PACKED;
  BYTE checksum PACKED;
  USHORT name2[6] PACKED;
  USHORT cluster PACKED;
  USHORT name3[2] PACKED;
} VFAT_DIRENT;

typedef union
{
  BYTE raw[2048];
  struct
    {
      BYTE   jump[3] PACKED;
      BYTE   oem[8] PACKED;
      USHORT bytes_per_sector PACKED;
      BYTE   sectors_per_cluster PACKED;
      USHORT reserved_sectors PACKED;
      BYTE   fats PACKED;
      USHORT root_entries PACKED;
      USHORT sectors PACKED;
      BYTE   media PACKED;
      USHORT sectors_per_fat PACKED;
      USHORT sectors_per_track PACKED;
      USHORT heads PACKED;
      USHORT hidden_sectors_lo PACKED;
      USHORT hidden_sectors_hi PACKED;
      ULONG  large_sectors PACKED;
      union
      {
        struct
        {
          BYTE   drive_no PACKED;
          BYTE   reserved PACKED;
          BYTE   extended_sig PACKED;
          ULONG  vol_id PACKED;
          BYTE   vol_label[11] PACKED;
          BYTE   vol_type[8] PACKED;
        } fat;
        struct
        {
          ULONG  sectors_per_fat PACKED;
          USHORT flags PACKED;
          BYTE   version[2] PACKED;
          ULONG  root_cluster PACKED;
          USHORT info_sector PACKED;
          USHORT boot_sector_backup PACKED;
          BYTE   reserved[12] PACKED;
        } fat32;
      } r;
    } boot;
#ifdef OS2 /* TODO: define FEALIST etc. for Unix */
  struct
    {
      BYTE   magic[2] PACKED;	/* "ED" */
      USHORT unused[15] PACKED;
      USHORT table[240] PACKED;
    } ea1;
  struct
    {
      BYTE    magic[2] PACKED;	/* "EA" */
      USHORT  rel_cluster PACKED;
      ULONG   need_eas PACKED;
      BYTE    name[14] PACKED;
      BYTE    unknown[4] PACKED;
      FEALIST fealist PACKED;
    } ea3;
#endif
  } FAT_SECTOR;
#pragma pack()
