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
  BYTE name[8+3];
  BYTE attr;
  BYTE reserved[8];
  USHORT ea;             /* Or upper 16 bits of cluster for FAT32 */
  USHORT time;
  USHORT date;
  USHORT cluster16;
  ULONG size;
} PACKED FAT_DIRENT;

typedef struct
{
  BYTE flag;
  USHORT name1[5];
  BYTE attr;
  BYTE reserved;
  BYTE checksum;
  USHORT name2[6];
  USHORT cluster;
  USHORT name3[2];
} PACKED VFAT_DIRENT;

typedef union
{
  BYTE raw[2048];
  struct
    {
      BYTE   jump[3];
      BYTE   oem[8];
      USHORT bytes_per_sector;
      BYTE   sectors_per_cluster;
      USHORT reserved_sectors;
      BYTE   fats;
      USHORT root_entries;
      USHORT sectors;
      BYTE   media;
      USHORT sectors_per_fat;
      USHORT sectors_per_track;
      USHORT heads;
      USHORT hidden_sectors_lo;
      USHORT hidden_sectors_hi;
      ULONG  large_sectors;
      union
      {
        struct
        {
          BYTE   drive_no;
          BYTE   reserved;
          BYTE   extended_sig;
          ULONG  vol_id;
          BYTE   vol_label[11];
          BYTE   vol_type[8];
        } PACKED fat;
        struct
        {
          ULONG  sectors_per_fat;
          USHORT flags;
          BYTE   version[2];
          ULONG  root_cluster;
          USHORT info_sector;
          USHORT boot_sector_backup;
          BYTE   reserved[12];
        } PACKED fat32;
      } r;
    } PACKED boot;
#ifdef OS2 /* TODO: define FEALIST etc. for Unix */
  struct
    {
      BYTE   magic[2];	/* "ED" */
      USHORT unused[15];
      USHORT table[240];
    } PACKED ea1;
  struct
    {
      BYTE    magic[2];	/* "EA" */
      USHORT  rel_cluster;
      ULONG   need_eas;
      BYTE    name[14];
      BYTE    unknown[4];
      FEALIST fealist;
    } PACKED ea3;
#endif
  } FAT_SECTOR;
#pragma pack()
