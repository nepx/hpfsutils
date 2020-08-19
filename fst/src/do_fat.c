/* do_fat.c -- FAT-specific code for fst
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


/* TODO: top 4 bits of cluster number of FAT32 */

#ifdef OS2
#include <os2.h>
#endif
#ifdef WINDOWS
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "fst.h"
#include "crc.h"
#include "diskio.h"
#include "fat.h"


struct vfat
{
  char flag;
  char unprintable;
  BYTE total;
  BYTE index;
  BYTE checksum;
  int start;
  BYTE name[256+1];
};

static ULONG sector_size;
static ULONG first_sector;
static ULONG total_sectors;
static ULONG total_clusters;
static ULONG sectors_per_cluster;
static ULONG bytes_per_cluster;
static ULONG sectors_per_fat;
static ULONG number_of_fats;
static ULONG root_entries;
static ULONG root_sectors;
static ULONG data_sector;
static ULONG what_cluster;
static USHORT **fats16;
static USHORT *fat16;
static ULONG **fats32;
static ULONG *fat32;
static char fat32_flag;
static ULONG free_start;
static ULONG free_length;
static BYTE raw_fat_start[2*4]; /* Raw fat entries 0 and 1 */

static BYTE *usage_vector;      /* One byte per cluster, indicating usage */
static const path_chain **path_vector; /* One path name chain per sector */

static BYTE find_comp[256];     /* Current component of `find_path' */

#ifdef OS2
static char ea_ok;
static ULONG ea_data_start;
static ULONG ea_data_size;
static ULONG ea_data_clusters;
static USHORT ea_table1[240];   /* First table from `EA DATA. SF' */
static USHORT *ea_table2;       /* Second table from `EA DATA. SF' */
static ULONG ea_table2_entries;
static BYTE *ea_usage;          /* One byte per cluster of `EA DATA. SF' */
#endif

#define CLUSTER_TO_SECTOR(c) (((c) - 2) * sectors_per_cluster + data_sector)
#define SECTOR_TO_CLUSTER(s) (((s) - data_sector) / sectors_per_cluster + 2)
#define FAT_ENTRY(c)         (fat32_flag ? fat32[c] : fat16[c])

/* Rotate right a byte by one bit. */

static INLINE BYTE rorb1 (BYTE b)
{
  return (b & 1) ? (b >> 1) | 0x80 : b >> 1;
}


/* Compare two file names pointed to by P1 and P2. */

static int compare_fname (const BYTE *p1, const BYTE *p2)
{
  for (;;)
    {
      if (*p1 == 0 && *p2 == 0)
        return 0;
      if (*p2 == 0)
        return 1;
      if (*p1 == 0)
        return -1;
      if (cur_case_map[*p1] > cur_case_map[*p2])
        return 1;
      if (cur_case_map[*p1] < cur_case_map[*p2])
        return -1;
      ++p1; ++p2;
    }
}


/* Return a pointer to a string containing a formatted range of
   cluster numbers.  Note that the pointer points to static memory; do
   not use format_cluster_range() more than once in one expression! */

static const char *format_cluster_range (ULONG start, ULONG count)
{
  static char buf[60];

  if (count == 1)
    sprintf (buf, "cluster %lu", start);
  else
    sprintf (buf, "%lu clusters %lu-%lu", count, start, start + count - 1);
  return buf;
}


/* Return a pointer to a string containing a file time as string.
   Note that the pointer points to static memory; do not use
   format_time() more than once in one expression! */

static const char *format_time (unsigned t)
{
  static char buf[20];

  sprintf (buf, "%.2d:%.2d:%.2d",
           (t >> 11) & 31, (t >> 5) & 63, (t & 31) << 1);
  return buf;
}


/* Return a pointer to a string containing a file date as string.
   Note that the pointer points to static memory; do not use
   format_date() more than once in one expression! */

static const char *format_date (unsigned d)
{
  static char buf[20];

  sprintf (buf, "%d-%.2d-%.2d",
           ((d >> 9) & 127) + 1980, (d >> 5) & 15, d & 31);
  return buf;
}


/* Return the number of days in month M of year Y. */

static int days (int y, int m)
{
  static int month_len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  if (m < 1 || m > 12)
    return 0;
  else if (m != 2)
    return month_len[m-1];
  else
    return y % 4 != 0 ? 28 : y % 100 != 0 ? 29 : y % 400 != 0 ? 28 : 29;
}


/* Types of clusters. */

#define USE_EMPTY       0
#define USE_FILE        1
#define USE_DIR         2
#define USE_LOST        3


/* Return a pointer to a string containing a description of a cluster
   type. */

static const char *cluster_usage (BYTE what)
{
  switch (what)
    {
    case USE_EMPTY:
      return "empty";
    case USE_DIR:
      return "directory";
    case USE_FILE:
      return "file";
    default:
      return "INTERNAL_ERROR";
    }
}


/* Use cluster CLUSTER for WHAT.  PATH points to a string naming the
   object for which the cluster is used.  PATH points to the path name
   chain for the file or directory.  Return FALSE if a cycle has been
   detected. */

static int use_cluster (ULONG cluster, BYTE what, const path_chain *path)
{
  BYTE old;

  if (cluster >= total_clusters)
    abort ();
  old = usage_vector[cluster];
  if (old != USE_EMPTY)
    {
      warning (1, "Cluster %lu usage conflict: %s vs. %s",
               cluster, cluster_usage (usage_vector[cluster]),
               cluster_usage (what));
      if (path_vector != NULL && path_vector[cluster] != NULL)
        warning_cont ("File 1: \"%s\"",
                      format_path_chain (path_vector[cluster], NULL));
      if (path != NULL)
        warning_cont ("File 2: \"%s\"",
                      format_path_chain (path, NULL));
      return !(path != NULL && path == path_vector[cluster]);
    }
  else
    {
      usage_vector[cluster] = what;
      if (path_vector != NULL)
        path_vector[cluster] = path;
      return TRUE;
    }
}


static void dirent_warning (int level, const char *fmt, ULONG secno,
                            const path_chain *path,
                            const BYTE *name, ...) ATTR_PRINTF (2, 6);


#define ALLOCATED16(x)  (fat16[x] != 0 && fat16[x] != 0xfff7)
#define ALLOCATED32(x)  (fat32[x] != 0 && fat32[x] != 0x0ffffff7)
#define ALLOCATED(x)    (fat32_flag ? ALLOCATED32 (x) : ALLOCATED16 (x))

#define BADSECTOR16(x)  (fat16[x] == 0xfff7)
#define BADSECTOR32(x)  (fat32[x] == 0x0ffffff7)
#define BADSECTOR(x)    (fat32_flag ? BADSECTOR32 (x) : BADSECTOR16 (x))

#define LASTCLUSTER16(x) (fat16[x] >= 0xfff8)
#define LASTCLUSTER32(x) (fat32[x] >= 0x0ffffff8)
#define LASTCLUSTER(x)   (fat32_flag ? LASTCLUSTER32 (x) : LASTCLUSTER16 (x))

#define UNUSED16(x)      (fat16[x] == 0)
#define UNUSED32(x)      (fat32[x] == 0)
#define UNUSED(x)        (fat32_flag ? UNUSED32 (x) : UNUSED16 (x))


static USHORT *read_fat16 (DISKIO *d, ULONG secno)
{
  USHORT *fat;
  ULONG sectors, clusters, i;

  clusters = total_clusters;
  sectors = DIVIDE_UP (clusters * 2, sector_size);
  if (sectors != sectors_per_fat)
    warning (1, "Incorrect FAT size: %lu vs. %lu", sectors, sectors_per_fat);
  fat = xmalloc (sectors * sector_size);
  read_sec (d, fat, secno, sectors, TRUE);
  memcpy (raw_fat_start, fat, 2*2);
  for (i = 0; i < clusters; ++i)
    fat[i] = READ_USHORT (&fat[i]);
  return fat;
}


static USHORT *read_fat12 (DISKIO *d, ULONG secno)
{
  USHORT *fat;
  ULONG clusters, sectors, i, s, t;
  USHORT c1, c2;
  BYTE *raw;

  clusters = total_clusters;
  sectors = DIVIDE_UP (clusters * 3, sector_size * 2);
  if (sectors != sectors_per_fat)
    warning (1, "Incorrect FAT size: %lu vs. %lu", sectors, sectors_per_fat);
  raw = xmalloc (sectors * sector_size + 2);
  read_sec (d, raw, secno, sectors, TRUE);
  memcpy (raw_fat_start, raw, 3);
  fat = xmalloc (clusters * 2 + 1);
  s = 0;
  for (i = 0; i < clusters; i += 2)
    {
      t = raw[s+0] | (raw[s+1] << 8) | (raw[s+2] << 16);
      c1 = t & 0xfff;
      if (c1 >= 0xff7)
        c1 |= 0xf000;
      c2 = (t >> 12) & 0xfff;
      if (c2 >= 0xff7)
        c2 |= 0xf000;
      fat[i+0] = c1;
      fat[i+1] = c2;
      s += 3;
    }
  free (raw);
  return fat;
}


static ULONG *read_fat32 (DISKIO *d, ULONG secno)
{
  ULONG *fat;
  ULONG sectors, clusters, i;

  clusters = total_clusters;
  sectors = DIVIDE_UP (clusters * 4, sector_size);
  if (sectors != sectors_per_fat)
    warning (1, "Incorrect FAT size: %lu vs. %lu", sectors, sectors_per_fat);
  fat = xmalloc (sectors * sector_size);
  read_sec (d, fat, secno, sectors, TRUE);
  memcpy (raw_fat_start, fat, 2*4);
  for (i = 0; i < clusters; ++i)
    fat[i] = READ_ULONG (&fat[i]);
  return fat;
}


static USHORT *read_fat (DISKIO *d, ULONG secno, ULONG fatno)
{
  if (a_what)
    {
      if (!what_cluster_flag && IN_RANGE (what_sector, secno, sectors_per_fat))
        info ("Sector #%lu: FAT %lu (+%lu)\n", what_sector, fatno + 1,
              what_sector - secno);
    }
  if (total_clusters - 2 > 4085)
    return read_fat16 (d, secno);
  else
    return read_fat12 (d, secno);
}

static void create_fat32 (BYTE *buf)
{
  /** @todo media descriptor */
  ULONG *dst = (ULONG *)buf;
  ULONG i;
  for (i = 0; i < total_clusters; ++i)
    {
      WRITE_ULONG (dst, fat32[i]);
      ++dst;
    }
  memcpy (buf, raw_fat_start, 2*4);
}

static void create_fat16 (BYTE *buf)
{
  /** @todo media descriptor */
  USHORT *dst = (USHORT *)buf;
  ULONG i;
  for (i = 0; i < total_clusters; ++i)
    {
      WRITE_USHORT (dst, fat16[i]);
      ++dst;
    }
  memcpy (buf, raw_fat_start, 2*2);
}

static void create_fat12 (BYTE *buf)
{
  /** @todo media descriptor */
  ULONG i = 0;
  while (i < total_clusters)
    {
      unsigned c1 = fat16[i+0];
      unsigned c2 = (i + 1 < total_clusters) ? fat16[i+1] : 0;
      ULONG c = ((c2 & 0xfff) << 12) | (c1 & 0xfff);
      buf[0] = (BYTE)((c >>  0)& 0xff);
      buf[1] = (BYTE)((c >>  8) & 0xff);
      buf[2] = (BYTE)((c >> 16) & 0xff);
      buf += 3;
      i += 2;
    }
  memcpy (buf, raw_fat_start, 3);
}

static void write_fats (DISKIO *d)
{
  ULONG bytes = sectors_per_fat * sector_size;
  BYTE *buf = xmalloc (bytes);
  ULONG secno = first_sector;
  ULONG i;

  memset (buf, 0, sizeof (buf));
  if (fat32_flag)
    create_fat32 (buf);
  else if (total_clusters - 2 > 4085)
    create_fat16 (buf);
  else
    create_fat12 (buf);

  for (i = 0; i < number_of_fats; ++i)
    {
      write_sec (d, buf, secno, sectors_per_fat);
      secno += sectors_per_fat;
    }
  free (buf);
}

static void info_fat (ULONG x)
{
  if (x == 0)
    info ("unused");
  else if (x == (fat32_flag ? 0x0ffffff7 : 0xfff7))
    info ("bad");
  else if (x == (fat32_flag ? 0x0ffffff8 : 0xfff8))
    info ("last");
  else
    info ("%lu", x);
}

static void fat_difference (ULONG where, ULONG first, ULONG second)
{
  info ("  %lu: ", where);
  info_fat (first);
  info (" ");
  info_fat (second);
  info ("\n");
}

static void show_free_init ()
{
  free_start = 0;
  free_length = 0;
}

static void show_free (ULONG cluster)
{
  if (cluster != 0 && cluster == free_start + free_length)
    ++free_length;
  else
    {
      if (free_length != 0)
        info ("Free: %s\n", format_cluster_range (free_start, free_length));
      free_start = cluster;
      free_length = 1;
    }
}

static void show_free_done ()
{
  show_free (0);
}

static void do_fats16 (DISKIO *d)
{
  ULONG secno, i, j, k, free, bad;

  fats16 = xmalloc (number_of_fats * sizeof (*fats16));
  secno = first_sector;
  for (i = 0; i < number_of_fats; ++i)
    {
      if (a_info)
        info ("FAT %lu:                      %s\n",
              i + 1, format_sector_range (secno, sectors_per_fat));
      fats16[i] = read_fat (d, secno, i);
      secno += sectors_per_fat;
    }
  for (i = 0; i < number_of_fats; ++i)
    for (j = i + 1; j < number_of_fats; ++j)
      if (memcmp (fats16[i], fats16[j], total_clusters * sizeof (USHORT)) != 0)
        {
          warning (1, "FATs %lu and %lu differ", i + 1, j + 1);
          info ("Differing clusters:\n");
          for (k = 0; k < total_clusters; ++k)
            if (fats16[i][k] != fats16[j][k])
              fat_difference (k, fats16[i][k], fats16[j][k]);
        }
  fat16 = fats16[use_fat == 0 ? 0 : use_fat - 1];
  free = bad = 0;
  for (i = 2; i < total_clusters; ++i)
    if (fat16[i] == 0)
      ++free;
    else if (fat16[i] == 0xfff7)
      ++bad;
  if (a_info)
    {
      if (show_unused)
        {
          show_free_init ();
          for (i = 2; i < total_clusters; ++i)
            if (fat16[i] == 0)
              show_free (i);
          show_free_done ();
        }
      info ("Number of free clusters:    %lu\n", free);
      info ("Number of bad clusters:     %lu\n", bad);
    }
}

static void do_fats32 (DISKIO *d)
{
  ULONG secno, i, j, k, free, bad;

  fats32 = xmalloc (number_of_fats * sizeof (*fats32));
  secno = first_sector;
  for (i = 0; i < number_of_fats; ++i)
    {
      if (a_info)
        info ("FAT %lu:                      %s\n",
              i + 1, format_sector_range (secno, sectors_per_fat));
      fats32[i] = read_fat32 (d, secno);
      secno += sectors_per_fat;
    }
  for (i = 0; i < number_of_fats; ++i)
    for (j = i + 1; j < number_of_fats; ++j)
      if (memcmp (fats32[i], fats32[j], total_clusters * sizeof (ULONG)) != 0)
        {
          warning (1, "FATs %lu and %lu differ", i + 1, j + 1);
          info ("Differing clusters:\n");
          for (k = 0; k < total_clusters; ++k)
            if (fats32[i][k] != fats32[j][k])
              fat_difference (k, fats32[i][k], fats32[j][k]);
        }
  fat32 = fats32[use_fat == 0 ? 0 : use_fat - 1];
  free = bad = 0;
  for (i = 2; i < total_clusters; ++i)
    if (fat32[i] == 0)
      ++free;
    else if (fat32[i] == 0x0ffffff7)
      ++bad;
  if (a_info)
    {
      if (show_unused)
        {
          show_free_init ();
          for (i = 2; i < total_clusters; ++i)
            if (fat32[i] == 0)
              show_free (i);
          show_free_done ();
        }
      info ("Number of free clusters:    %lu\n", free);
      info ("Number of bad clusters:     %lu\n", bad);
    }
}


#ifdef OS2
static void read_ea_data (DISKIO *d)
{
  FAT_SECTOR ea1;
  ULONG i, min_cluster, cluster, pos, size2;
  char *tab2, *cluster_buffer;

  if (ea_data_start == 0xffff)
    return;
  if (ea_data_start < 2 || ea_data_start >= total_clusters)
    {
      warning (1, "\"EA DATA. SF\": Start cluster (%lu) is invalid",
               ea_data_start);
      return;
    }

  /* Read the first table and copy it to `ea_table1'. */

  read_sec (d, &ea1, CLUSTER_TO_SECTOR (ea_data_start), 1, FALSE);
  if (memcmp (ea1.ea1.magic, "ED", 2) != 0)
    {
      warning (1, "\"EA DATA. SF\": Incorrect signature");
      return;
    }
  memcpy (ea_table1, ea1.ea1.table, sizeof (ea_table1));

  /* Find the smallest cluster number referenced by the first table.
     This gives us the maximum size of the second table. */

  min_cluster = ea_table1[0];
  for (i = 1; i < 240; ++i)
    if (ea_table1[i] < min_cluster)
      min_cluster = ea_table1[i];

  if (min_cluster < 1)
    {
      warning (1, "\"EA DATA. SF\": First table contains a zero entry");
      return;
    }
  if (min_cluster >= total_clusters)
    {
      warning (1, "\"EA DATA. SF\": Second table is too big (%lu clusters)",
               min_cluster);
      return;
    }

  /* We make the table too big by one sector to simplify reading the
     table. */

  size2 = min_cluster * bytes_per_cluster;
  if (size2 > ea_data_size)
    {
      warning (1, "\"EA DATA. SF\": Beyond end of file");
      return;
    }
  tab2 = xmalloc (size2);
  cluster_buffer = xmalloc (bytes_per_cluster);

  cluster = ea_data_start;
  for (pos = 0; pos < ea_data_size; pos += bytes_per_cluster)
    {
      if (cluster < 2 || cluster >= total_clusters)
        {
          warning (1, "\"EA DATA. SF\": Invalid FAT chain");
          free (cluster_buffer);
          return;
        }
      read_sec (d, cluster_buffer, CLUSTER_TO_SECTOR (cluster),
                sectors_per_cluster, TRUE);
      if (pos < size2)
        memcpy (tab2 + pos, cluster_buffer, 
                MIN (bytes_per_cluster, size2 - pos));
      cluster = FAT_ENTRY (cluster);
    }
  free (cluster_buffer);

  /* Skip the first 512 bytes (table 1). */

  ea_table2 = (USHORT *)(tab2 + 512);
  ea_table2_entries = (size2 - 512) / 2;

  ea_usage = xmalloc (ea_data_clusters);
  memset (ea_usage, FALSE, ea_data_clusters);
  ea_ok = TRUE;
}


static void do_ea (DISKIO *d, const path_chain *path, ULONG ea_index,
                   int show)
{
  ULONG rel_cluster, cluster, size, size2, i, n;
  ULONG pos, need_eas, value_size, secno;
  FAT_SECTOR ea3;
  char *buf;
  const char *name, *value;
  const FEA *pfea;

  if ((ea_index >> 7) >= 240 || ea_index >= ea_table2_entries)
    {
      warning (1, "\"%s\": Invalid EA index (%lu)",
               format_path_chain (path, NULL), ea_index);
      return;
    }
  if (ea_table2[ea_index] == 0xffff)
    {
      warning (1, "\"%s\": EA index (%lu) points to unused slot",
               format_path_chain (path, NULL), ea_index);
      return;
    }
  rel_cluster = ea_table1[ea_index >> 7] + ea_table2[ea_index];
  if (show)
    info ("Rel. EA cluster:    %lu\n", rel_cluster);
  if ((rel_cluster + 1) * bytes_per_cluster > ea_data_size)
    {
      warning (1, "\"%s\": Invalid relative EA cluster (%lu)",
               format_path_chain (path, NULL), rel_cluster);
      return;
    }

  cluster = ea_data_start;
  for (i = 0; i < rel_cluster; ++i)
    {
      if (cluster < 2 || cluster >= total_clusters)
        abort ();               /* Already checked */
      cluster = FAT_ENTRY (cluster);
    }

  secno = CLUSTER_TO_SECTOR (cluster);
  read_sec (d, &ea3, secno, 1, FALSE);
  if (memcmp (ea3.ea3.magic, "EA", 2) != 0)
    {
      warning (1, "\"%s\": Incorrect signature for EA (sector #%lu)",
               format_path_chain (path, NULL), secno);
      return;
    }

  if (READ_USHORT (&ea3.ea3.rel_cluster) != ea_index)
    warning (1, "\"%s\": Incorrect EA index in \"EA DATA. SF\" "
             "(sector #%lu)", format_path_chain (path, NULL), secno);

  if (memchr (ea3.ea3.name, 0, 13) == NULL)
    warning (1, "\"%s\": Name in \"EA DATA. SF\" not null-terminated "
             "(sector #%lu)", format_path_chain (path, NULL), secno);
  /* OS/2 does not update "EA DATA. SF" when renaming a file!
     Therefore we check this only if -p is given. */
  else if (check_pedantic
           && strcmp ((const char *)ea3.ea3.name, path->name) != 0)
    warning (0, "\"%s\": Name in \"EA DATA. SF\" does not match "
             "(sector #%lu)", format_path_chain (path, NULL), secno);

  size = READ_ULONG (&ea3.ea3.fealist.cbList);
  if (size >= 0x40000000
      || (rel_cluster * bytes_per_cluster
          + offsetof (FAT_SECTOR, ea3.fealist)) > ea_data_size)
    {
      warning (1, "\"%s\": EAs too big (sector #%lu, %lu bytes)",
               format_path_chain (path, NULL), secno, size);
      return;
    }

  if (show)
    info ("Size of EAs:        %lu\n", size);

  size2 = size + offsetof (FAT_SECTOR, ea3.fealist);

  if (a_check)
    {
      n = DIVIDE_UP (size2, bytes_per_cluster);
      for (i = 0; i < n; ++i)
        {
          if (ea_usage[rel_cluster+i] != FALSE)
            {
              warning (1, "Relative cluster %lu of \"EA DATA. SF\" "
                       "multiply used", rel_cluster + i);
              warning_cont ("File 2: \"%s\"", format_path_chain (path, NULL));
            }
          else
            ea_usage[rel_cluster+i] = TRUE;
        }
    }

  if (a_where)
    {
      ULONG c;

      n = DIVIDE_UP (size2, bytes_per_cluster);
      c = cluster;
      for (i = 0; i < n; ++i)
        {
          if (c < 2 || c >= total_clusters)
            abort ();           /* Already checked */
          info ("Extended attributes in cluster %lu\n", c);
          c = FAT_ENTRY (c);
        }
    }

  if (a_what)
    {
      for (pos = 0; pos < size2; pos += bytes_per_cluster)
        {
          if (cluster < 2 || cluster >= total_clusters)
            abort ();           /* Already checked */
          if (what_cluster_flag && cluster == what_cluster)
            info ("Cluster %lu: Extended attributes for \"%s\"\n",
                  cluster, format_path_chain (path, NULL));
          else if (!what_cluster_flag)
            {
              secno = CLUSTER_TO_SECTOR (cluster);
              if (IN_RANGE (what_sector, secno,
                            MIN (sectors_per_cluster,
                                 DIVIDE_UP (size2 - pos, 512))))
                info ("Sector #%lu: Extended attributes for \"%s\"\n",
                      what_sector, format_path_chain (path, NULL));
            }
          cluster = FAT_ENTRY (cluster);
        }
    }
  else if (size2 <= 0x100000 && (a_check || a_where))
    {
      buf = xmalloc (ROUND_UP (size2, 512));
      /* TODO: This reads the first sector twice. */
      for (pos = 0; pos < size2; pos += bytes_per_cluster)
        {
          if (cluster < 2 || cluster >= total_clusters)
            abort ();           /* Already checked */
          read_sec (d, buf + pos, CLUSTER_TO_SECTOR (cluster),
                    MIN (sectors_per_cluster, DIVIDE_UP (size2 - pos, 512)),
                    FALSE);
          cluster = FAT_ENTRY (cluster);
        }

      pos = offsetof (FEALIST, list); need_eas = 0;
      while (pos < size)
        {
          if (pos + sizeof (FEA) > size)
            {
              warning (1, "\"%s\": Truncated FEA structure",
                       format_path_chain (path, NULL));
              break;
            }
          pfea = (const FEA *)(buf + pos + offsetof (FAT_SECTOR, ea3.fealist));
          if (pfea->fEA & FEA_NEEDEA)
            ++need_eas;
          value_size = READ_USHORT (&pfea->cbValue);
          if (pos + sizeof (FEA) + pfea->cbName + 1 + value_size > size)
            {
              warning (1, "\"%s\": Incorrect EA size",
                       format_path_chain (path, NULL));
              break;
            }
          name = (const char *)pfea + sizeof (FEA);
          value = name + 1 + pfea->cbName;
          if (name[pfea->cbName] != 0)
            warning (1, "\"%s\": EA name not null-terminated",
                     format_path_chain (path, NULL));
          if (show_eas >= 1)
            info ("Extended attribute %s (%lu bytes)\n",
                  format_ea_name (pfea), value_size);
          pos += sizeof (FEA) + pfea->cbName + 1 + value_size;
        }
      if (need_eas != READ_ULONG (&ea3.ea3.need_eas))
        warning (1, "\"%s\": Incorrect number of `need' EAs",
                 format_path_chain (path, NULL));
      free (buf);
    }
}
#endif

enum dir_end_state
{
  des_no_zero,			/* initial state, no zero found */
  des_done,			/* checking only, zero found */
  des_zero_seen,		/* fixing, zero found */
  des_fixed,			/* fixing, zero found, error fixed */
  des_fixed_write,		/* fixing, zero found, write pending */
  des_not_fixed			/* fixing, zero found, error not fixed */
};

static void do_dir (DISKIO *d, ULONG secno, ULONG entries,
                    const path_chain *path, struct vfat *pv,
                    ULONG parent_cluster, ULONG start_cluster,
                    ULONG this_cluster, ULONG dirent_index, int list,
		    enum dir_end_state *dir_end_flag, int root_flag);

static void dirent_warning (int level, const char *fmt, ULONG secno,
                            const path_chain *path, const BYTE *name, ...)
{
  va_list arg_ptr;

  warning_prolog (level);
  my_fprintf (diag_file, "Directory sector #%lu (\"%s\"): \"%s\": ",
              secno, format_path_chain (path, NULL), name);
  va_start (arg_ptr, name);
  my_vfprintf (diag_file, fmt, arg_ptr);
  va_end (arg_ptr);
  fputc ('\n', diag_file);
  warning_epilog ();
}


static void do_enddir (DISKIO *d, const path_chain *path, struct vfat *pv,
                       int found)
{
  if (pv->flag)
    warning (1, "\"%s\": No real directory entry after VFAT name",
             format_path_chain (path, NULL));
  if (a_find)
    {
      if (found)
        quit (0, FALSE);
      else
        error ("\"%s\" not found in \"%s\"",
               find_comp, format_path_chain (path, NULL));
    }
}


static void do_file (DISKIO *d, ULONG start_cluster, int dir_flag,
                     const path_chain *path, ULONG parent_cluster,
                     ULONG file_size, int ignore_size, ULONG ea_index,
                     int list, int root_flag)
{
  ULONG count, cluster, dirent_index, extents, ext_start, ext_length;
  int show, found;
  enum dir_end_state dir_end_flag;
  char *copy_buf = NULL;
  struct vfat v;

  found = (a_find && *find_path == 0);
  show = (a_where && found);
  if (found && a_copy)
    {
      if (dir_flag)
        error ("Directories cannot be copied");
      copy_buf = xmalloc (bytes_per_cluster);
    }
  count = 0; cluster = start_cluster; dirent_index = 0; v.flag = FALSE;
  extents = 0; ext_start = 0; ext_length = 0;
  dir_end_flag = des_no_zero;
  if (cluster != 0)
    while (cluster < (fat32_flag ? 0x0ffffff8 : 0xfff8))
      {
        if (ext_length == 0)
          {
            ++extents; ext_start = cluster; ext_length = 1;
          }
        else if (cluster == ext_start + ext_length)
          ++ext_length;
        else
          {
            if (show)
              info ("File data in %s\n",
                    format_cluster_range (ext_start, ext_length));
            ++extents; ext_start = cluster; ext_length = 1;
          }
        if (cluster == 0)
          {
            warning (1, "\"%s\": References unused cluster",
                     format_path_chain (path, NULL));
            break;
          }
        else if (cluster == (fat32_flag ? 0x0ffffff7 : 0xfff7))
          {
            warning (1, "\"%s\": References bad cluster",
                     format_path_chain (path, NULL));
            break;
          }
        else if (cluster < (fat32_flag ? 0x0ffffff8 : 0xfff8)
                 && (cluster < 2 || cluster >= total_clusters))
          {
            warning (1, "\"%s\": %lu: Invalid cluster number",
                     format_path_chain (path, NULL), cluster);
            break;
          }
        else
          {
            if (!use_cluster (cluster, dir_flag ? USE_DIR : USE_FILE, path))
              {
                warning (1, "\"%s\": Cycle after %lu clusters",
                         format_path_chain (path, NULL), count);
                break;
              }
            if (a_what)
              {
                if (what_cluster_flag && what_cluster == cluster)
                  info ("Cluster %lu: Relative cluster %lu of \"%s\"\n",
                        what_cluster, count, format_path_chain (path, NULL));
                else if (!what_cluster_flag
                         && IN_RANGE (what_sector, CLUSTER_TO_SECTOR (cluster),
                                      sectors_per_cluster))
                  info ("Sector #%lu: Relative sector %lu of \"%s\"\n",
                        what_sector,
                        (count * sectors_per_cluster
                         + what_sector - CLUSTER_TO_SECTOR (cluster)),
                        format_path_chain (path, NULL));
              }
            if (dir_flag && (!found || !list) && dir_end_flag != des_done)
              {
                do_dir (d, CLUSTER_TO_SECTOR (cluster),
                        bytes_per_cluster/32, path, &v,
                        parent_cluster, start_cluster, cluster, dirent_index,
                        found && a_dir, &dir_end_flag, root_flag);
                dirent_index += bytes_per_cluster/32;
              }
            if (a_copy && found
                && (ignore_size || count * bytes_per_cluster < file_size))
              {
                size_t n;
                read_sec (d, copy_buf, CLUSTER_TO_SECTOR (cluster),
                          sectors_per_cluster, FALSE);
                n = bytes_per_cluster;
                if (!ignore_size)
                  n = MIN (file_size - count * bytes_per_cluster,
                           bytes_per_cluster);
                fwrite (copy_buf, n, 1, save_file);
                if (ferror (save_file))
                  save_error ();
              }
            cluster = FAT_ENTRY (cluster);
            ++count;
          }
      }

  if (dir_flag && !found)
    do_enddir (d, path, &v, FALSE);

  if (show)
    {
      if (ext_length != 0)
        info ("File data in %s\n",
              format_cluster_range (ext_start, ext_length));
      info ("Number of clusters: %lu\n", count);
      info ("Number of extents:  %lu\n", extents);
    }

#ifdef OS2
  if (ea_index != 0)
    do_ea (d, path, ea_index, show);
#endif

  if (a_check)
    {
      if (!dir_flag && !ignore_size)
        {
          if (count * bytes_per_cluster < file_size)
            warning (1, "\"%s\": Not enough clusters allocated",
                     format_path_chain (path, NULL));
          if (count > DIVIDE_UP (file_size, bytes_per_cluster))
            warning (1, "\"%s\": Too many clusters allocated",
                     format_path_chain (path, NULL));
        }
    }
  if (found)
    {
      if (a_copy)
        save_close ();
      if (!a_dir)
        quit (0, FALSE);
    }
}

static int ask_fix (void)
{
  char buf[10];
  for (;;)
    {
      printf ("Fix [y/n/q]? "); fflush (stdout);
      if (fix_yes)
	{
	  puts ("y"); fflush (stdout);
	  return TRUE;
	}
      if (fgets (buf, sizeof (buf), stdin) == NULL)
	quit (1, FALSE);
      if (strcmp (buf, "y\n") == 0)
	return TRUE;
      if (strcmp (buf, "n\n") == 0)
	return FALSE;
      if (strcmp (buf, "q\n") == 0)
	quit (2, FALSE);
    }
}

static ULONG get_head (const FAT_DIRENT *p)
{
  if (fat32_flag)
    return READ_USHORT (&p->cluster16) | ((ULONG)READ_USHORT (&p->ea) << 16);
  else
    return READ_USHORT (&p->cluster16);
}

static void set_head (FAT_DIRENT *p, ULONG head)
{
  if (fat32_flag)
    {
      WRITE_USHORT (&p->cluster16, (unsigned short)(head & 0xffff));
      WRITE_USHORT (&p->ea, (unsigned short)(head >> 16));
    }
  else
    WRITE_USHORT (&p->cluster16, (unsigned short)head);
}

static void set_fat_entry (ULONG cluster, ULONG next)
{
  if (fat32_flag)
    fat32[cluster] = next;
  else
    fat16[cluster] = next;
}

static void do_set_data_1 (const struct change *chg, ULONG cluster,
                           char *seen, ULONG *head, ULONG *tail,
                           ULONG *count, int really)
{
  if (cluster < 2 || cluster >= total_clusters)
    error ("Invalid cluster number %lu", cluster);
  if (ALLOCATED (cluster))
    error ("Cluster %lu is not free", cluster);
  if (seen[cluster])
    error ("Cluster %lu is added more than once", cluster);
  seen[cluster] = TRUE;
  if (really)
    {
      if (chg->type == CT_APPEND)
        {
#if 0
          info ("Appending cluster %lu, head=%lu, tail=%lu\n",
                cluster, *head, *tail);
#endif
          if (*tail == 0)
            *head = cluster;
          else
            set_fat_entry (*tail, cluster);
          set_fat_entry (cluster, fat32_flag ? 0x0ffffff8 : 0xfff8);
          *tail = cluster;
          *count += 1;
        }
      else
        abort ();
    }
}

static void do_set_data (DISKIO *d, const BYTE *sec_buf, ULONG secno,
                         FAT_DIRENT *p, int really)
{
  char *seen = xmalloc (total_clusters);
  const struct change *chg = changes;
  ULONG count = 0;
  ULONG head0 = get_head (p);
  ULONG head = head0;
  ULONG cl = head0;
  ULONG tail = head0;
  ULONG count0;

  memset (seen, FALSE, total_clusters);
  if (cl != 0)
    while (cl < (fat32_flag ? 0x0ffffff8 : 0xfff8))
      {
        if (cl < 2 || cl >= total_clusters || !ALLOCATED (cl) || seen[cl])
          error ("Invalid cluster chain");
        seen[cl] = TRUE;
        tail = cl;
        cl = FAT_ENTRY (cl);
        ++count;
      }
  memset (seen, FALSE, total_clusters);
  count0 = count;

  while (chg != NULL)
    {
      const struct source *src = chg->source;
      ULONG i;
      while (src != NULL)
        {
          cl = src->cluster;
          if (src->type == ST_SUCCESSIVE)
            for (i = 0; i < src->count; ++i)
              do_set_data_1 (chg, cl++, seen, &head, &tail, &count, really);
          else if (src->type == ST_UNUSED || src->type == ST_ALL_UNUSED)
            {
              i = 0;
              for (;;)
                {
                  if (src->type == ST_UNUSED && i >= src->count)
                    break;
                  if (!ALLOCATED (cl))
                    {
                      do_set_data_1 (chg, cl, seen, &head, &tail, &count, really);
                      ++i;
                    }
                  if (++cl == total_clusters)
                    cl = 2;
                  if (cl == src->cluster)
                    break;
                }
              if (src->type == ST_UNUSED && i < src->count)
                error ("Not enough unused clusters available");
            }
          else
            abort ();
          src = src->next;
        }
      chg = chg->next;
    }
  free (seen);

  write_fats (d);

  /* Update dirent */
  if (count != count0 || head != head0)
    {
      if (count != count0)
        WRITE_ULONG (&p->size, count * bytes_per_cluster);
      if (head != head0)
        set_head (p, head);
      write_sec (d, sec_buf, secno, 1);
    }
}

static void do_dirent (DISKIO *d, const BYTE *sec_buf, ULONG secno,
                       FAT_DIRENT *p,
                       const path_chain *path, struct vfat *pv,
                       ULONG parent_cluster, ULONG start_cluster,
                       ULONG dirent_index, int *label_flag, int show, int list,
		       enum dir_end_state *dir_end_flag, int root_flag)
{
  BYTE name[13], cs;
  int i, n, name_len;
  char found;
  unsigned date, time;
  ULONG cluster;
  const VFAT_DIRENT *v;
  USHORT vname[13];

  if (p->name[0] == 0xe5 || p->name[0] == 0x00)
    {
      if (p->name[0] == 0x00 && *dir_end_flag == des_no_zero)
	*dir_end_flag = des_zero_seen;
      if (pv->flag)
        {
          warning (1, "\"%s\": Unused directory entry after VFAT name "
                   "(sector #%lu)", format_path_chain (path, NULL), secno);
          pv->flag = FALSE;
        }
      return;
    }

  if (*dir_end_flag == des_zero_seen && fix_zero_ends_dir)
    {
      warning (1, "\"%s\": non-empty directory entry after 0x00 (sector #%lu)",
	       format_path_chain (path, NULL), secno);
      if (ask_fix ())
	{
	  *dir_end_flag = des_fixed_write;
	  p->name[0] = 0xe5;
	}
      else
	*dir_end_flag = des_done;
      return;
    }
  else if (*dir_end_flag == des_fixed_write)
    {
      p->name[0] = 0xe5;
      return;
    }
  else if (*dir_end_flag == des_fixed)
    {
      *dir_end_flag = des_fixed_write;
      p->name[0] = 0xe5;
      return;
    }
  else if (*dir_end_flag == des_not_fixed || *dir_end_flag == des_done)
    return;

  if (p->attr == 0x0f)
    {
      v = (const VFAT_DIRENT *)p;
      for (i = 0; i < 5; ++i)
        vname[i+0] = READ_USHORT (&v->name1[i]);
      for (i = 0; i < 6; ++i)
        vname[i+5] = READ_USHORT (&v->name2[i]);
      for (i = 0; i < 2; ++i)
        vname[i+11] = READ_USHORT (&v->name3[i]);
      n = 13;
      while (n > 0 && vname[n-1] == 0xffff)
        --n;

      if (show)
        {
          info ("Directory entry %lu of \"%s\":\n", dirent_index,
                format_path_chain (path, NULL));
          info ("  VFAT name frag:   \"");
          for (i = 0; i < n; ++i)
            if (vname[i] >= 0x20 && vname[i] <= 0xff)
              info ("%c", vname[i]);
            else
              info ("<0x%x>", vname[i]);
          info ("\"\n");
        }

      if (v->flag > 0x7f)
        {
          warning (1, "\"%s\": Invalid VFAT name (sector #%lu)",
                   format_path_chain (path, NULL), secno);
          pv->flag = FALSE;
          return;
        }
      else if (v->flag & 0x40)
        {
          if (pv->flag)
            warning (1, "\"%s\": No real directory entry after VFAT name "
                     "(sector #%lu)", format_path_chain (path, NULL), secno);
          else if (n == 0 || (n != 13 && vname[n-1] != 0))
            {
              warning (1, "\"%s\": VFAT name not null-terminated "
                       "(sector #%lu)", format_path_chain (path, NULL), secno);
              return;
            }
          else
            {
              --n;
              pv->flag = TRUE;
              pv->unprintable = FALSE;
              pv->name[256] = 0;
              pv->start = 256;
              pv->total = v->flag & 0x3f;
              pv->index = v->flag & 0x3f;
              pv->checksum = v->checksum;
            }
        }
      if ((v->flag & 0x3f) != pv->index || pv->index == 0)
        {
          warning (1, "\"%s\": Incorrect VFAT name index (sector #%lu)",
                   format_path_chain (path, NULL), secno);
          pv->flag = FALSE;
          return;
        }
      if (v->checksum != pv->checksum)
        warning (1, "\"%s\": Incorrect VFAT checksum (sector #%lu)",
                 format_path_chain (path, NULL), secno);
      pv->index -= 1;
      if (pv->start < n)
        {
          warning (1, "\"%s\": VFAT name too long (sector #%lu)",
                   format_path_chain (path, NULL), secno);
          pv->flag = FALSE;
          return;
        }
      for (i = n-1; i >= 0; --i)
        {
          if (vname[i] < 0x20 || vname[i] > 0xff)
            pv->unprintable = TRUE;
          pv->name[--pv->start] = (BYTE)vname[i];
        }
      return;
    }

  cluster = get_head (p);
  found = FALSE;

  if (p->name[0] == '.')
    {
      i = 1;
      if (p->name[1] == '.')
        ++i;
      memcpy (name, p->name, i);
    }
  else if (p->attr & ATTR_LABEL)
    {
      memcpy (name, p->name, 11);
      i = 11;
      while (i > 0 && name[i-1] == ' ')
        --i;
    }
  else
    {
      memcpy (name, p->name + 0, 8);
      i = 8;
      while (i > 0 && name[i-1] == ' ')
        --i;
      if (memcmp (p->name + 8, "   ", 3) != 0)
        {
          name[i++] = '.';
          memcpy (name + i, p->name + 8, 3);
          i += 3;
          while (name[i-1] == ' ')
            --i;
        }
    }
  name[i] = 0;
  name_len = i;
  if (name[0] == 0x05)
    name[0] = 0xe5;

  if (pv->flag)
    {
      if (pv->index != 0)
        {
          warning (1, "\"%s\": Incomplete VFAT name for \"%s\" (sector #%lu)",
                   format_path_chain (path, NULL), name, secno);
          pv->flag = FALSE;
        }
      /* TODO: Compare VFAT name to regular name */

      /* Compute and check the checksum. */

      cs = 0;
      for (i = 0; i < 8+3; ++i)
        cs = rorb1 (cs) + p->name[i];
      if (cs != pv->checksum)
        warning (1, "\"%s\": Checksum mismatch for \"%s\" (sector #%lu): "
                 "0x%.2x vs. 0x%.2x", format_path_chain (path, NULL), name,
                 secno, pv->checksum, cs);
    }

  if (a_find && !show && !list)
    {
      if (compare_fname (name, find_comp) != 0)
        goto done;
      if (*find_path == 0)
        {
          found = TRUE;
          if (a_where)
            {
              info ("Directory entry in sector #%lu\n", secno);
              show = TRUE;
            }
          if (a_dir)
            show = TRUE;
        }
    }

  date = READ_USHORT (&p->date);
  time = READ_USHORT (&p->time);

  if (list || (a_dir && show && !(p->attr & ATTR_DIR)))
    {
      info ("%s %s ", format_date (date), format_time (time));
      if (p->attr & ATTR_DIR)
        info ("     <DIR>      ");
      else
        info ("%10lu %c%c%c%c%c",
              READ_ULONG (&p->size),
              (p->attr & ATTR_READONLY) ? 'R' : '-',
              (p->attr & ATTR_HIDDEN)   ? 'H' : '-',
              (p->attr & ATTR_SYSTEM)   ? 'S' : '-',
              (p->attr & ATTR_LABEL)    ? 'V' : '-',
              (p->attr & ATTR_ARCHIVED) ? 'A' : '-');
      info (" \"%s\"", name);
      if (pv->flag)
	{
	  info ("%*s", 13 - name_len, "");
	  if (pv->unprintable)
	    info ("(not printable)");
	  else
	    info ("\"%s\"", pv->name + pv->start);
	}
      info ("\n");
    }

  if (show && !a_dir)
    {
      ULONG size = READ_ULONG (&p->size);
      ULONG clusters = DIVIDE_UP (size, bytes_per_cluster);
      info ("Directory entry %lu of \"%s\":\n", dirent_index,
            format_path_chain (path, NULL));
      info ("  Name:             \"%s\"\n", name);
      info ("  Attributes:       0x%.2x", p->attr);
      if (p->attr & ATTR_DIR)
        info (" dir");
      if (p->attr & ATTR_READONLY)
        info (" r/o");
      if (p->attr & ATTR_HIDDEN)
        info (" hidden");
      if (p->attr & ATTR_SYSTEM)
        info (" system");
      if (p->attr & ATTR_LABEL)
        info (" label");
      if (p->attr & ATTR_ARCHIVED)
        info (" arch");
      info ("\n");
      info ("  Cluster:          %lu\n", cluster);
      info ("  Time:             0x%.4x (%s)\n", time, format_time (time));
      info ("  Date:             0x%.4x (%s)\n", date, format_date (date));
      info ("  Size:             %lu (%lu cluster%s)\n",
            size, clusters, (clusters == 1) ? "" : "s");
      info ("  EA pointer:       %u\n", READ_USHORT (&p->ea));
      if (pv->flag)
        {
          if (pv->unprintable)
            info ("  VFAT name:        (not printable)\n");
          else
            info ("  VFAT name:        \"%s\"\n", pv->name + pv->start);
        }
    }

  if (a_check)
    {
      unsigned y, m, d, h, s;
      int i;

      y = ((date >> 9) & 127) + 1980;
      m = (date >> 5) & 15;
      d = date & 31;
      if (m < 1 || m > 12 || d < 1 || d > days (y, m))
        dirent_warning (0, "Invalid date (0x%.4x)", secno, path, name, date);

      h = (time >> 11) & 31;
      m = (time >> 5) & 63;
      s = (time & 31) << 1;
      if (h > 23 || m > 59 || s > 59)
        dirent_warning (0, "Invalid time (0x%.4x)", secno, path, name, time);
      if (p->attr & ~0x3f)
        dirent_warning (0, "Undefined attribute bit is set",
                        secno, path, name);

      /* Check the file name.  Note that file names starting with `.'
         are checked further down. */

      if (p->name[0] != '.')
        {
          for (i = 0; i < 11; ++i)
            if (p->name[i] != 0x05
                && (p->name[i] < 0x20
                    || strchr ("\"*+,./;:<=>?[\\]|", p->name[i]) != NULL))
              break;
          if (i < 11)
            dirent_warning (1, "Invalid character in file name",
                            secno, path, name);
        }
    }

  if (p->name[0] == '.')
    {
      if (pv->flag)
        {
          dirent_warning (1, "Must not have a VFAT name", secno, path, name);
          pv->flag = FALSE;
        }
      if (!a_check)
        return;
      if (memcmp (p->name + i, "          ", 11 - i) != 0)
        dirent_warning (1, "File name starting with \".\"",
                        secno, path, name);
      else if (!(p->attr & ATTR_DIR))
        dirent_warning (1, "Not a directory", secno, path, name);
      else if (cluster != (i == 1 ? start_cluster : parent_cluster))
        dirent_warning (1, "Incorrect cluster (%lu vs. %lu)",
                        secno, path, name,
                        cluster, (i == 1 ? start_cluster : parent_cluster));
      return;
    }

  if (verbose)
    {
      if (!pv->flag)
	my_fprintf (prog_file, "%s\n", format_path_chain (path, (char *)name));
      else if (pv->unprintable)
	my_fprintf (prog_file, "%s (not printable)\n",
		    format_path_chain (path, (char *)name));
      else
	my_fprintf (prog_file, "%s (\"%s\")\n",
		    format_path_chain (path, (char *)name),
		    pv->name + pv->start);
    }

  if (a_check)
    {
      if (p->attr & ATTR_LABEL)
        {
          if (path->parent != NULL)
            dirent_warning (1, "Unexpected volume label",
                            secno, path, name);
          else if (*label_flag)
            dirent_warning (1, "More than one volume label",
                            secno, path, name);
          else
            *label_flag = TRUE;
        }
    }

  if (found && a_get)
    {
      if (strcmp (get_name, "size") == 0)
        printf (get_format, READ_ULONG (&p->size));
      else if (strcmp (get_name, "head") == 0)
        printf (get_format, start_cluster);
      else
        error ("Oops, unknown name");
    }

  if (found && a_set)
    {
      if (strcmp (get_name, "size") == 0)
        WRITE_ULONG (&p->size, set_ulong);
      else if (strcmp (get_name, "head") == 0)
        set_head (p, set_ulong);
      else
        error ("Oops, unknown name");
      write_sec (d, sec_buf, secno, 1);
      info ("Sector #%lu modified\n", secno);
    }

  if (found && a_set_data)
    {
      do_set_data (d, sec_buf, secno, p, FALSE);
      do_set_data (d, sec_buf, secno, p, TRUE);
    }

  if (!(p->attr & ATTR_LABEL)
      && !list
      && !(a_what && !what_cluster_flag && what_sector < data_sector))
    {
      path_chain link, *plink;

      plink = PATH_CHAIN_NEW (&link, path, (char *)name);
      do_file (d, cluster, p->attr & ATTR_DIR, plink,
               root_flag ? 0 : start_cluster,
               READ_ULONG (&p->size), FALSE, READ_USHORT (&p->ea), list, FALSE);
    }
  if (found && !list)
    quit (0, FALSE);
done:
  pv->flag = FALSE;
}


static void do_dir (DISKIO *d, ULONG secno, ULONG entries,
                    const path_chain *path, struct vfat *pv,
                    ULONG parent_cluster, ULONG start_cluster,
                    ULONG this_cluster, ULONG dirent_index, int list,
		    enum dir_end_state *dir_end_flag, int root_flag)
{
  FAT_DIRENT *dir;
  ULONG i, n, last_secno;
  int show, label_flag;

  dir = (FAT_DIRENT*)xmalloc (sector_size);
  if (a_find && dirent_index == 0)
    {
      const char *pdelim;
      size_t len;

      pdelim = strpbrk (find_path, SEPS);
      if (pdelim == NULL)
        len = strlen (find_path);
      else
        len = pdelim - find_path;
      if (len > 255)
        error ("Path name component too long");
      memcpy (find_comp, find_path, len);
      find_comp[len] = 0;
      find_path += len;
      if (ISSEP (*find_path))
        {
          ++find_path;
          if (*find_path == 0)
            error ("Trailing directory separator");
        }
    }

  label_flag = FALSE; last_secno = 0;
  while (entries != 0)
    {
      show = FALSE;
      if (a_what)
        {
          if (what_cluster_flag && what_cluster == this_cluster)
            {
              info ("Cluster %lu: Directory \"%s\"\n", what_cluster,
                    format_path_chain (path, NULL));
              show = TRUE;
            }
          else if (!what_cluster_flag && what_sector == secno)
            {
              info ("Sector #%lu: Directory \"%s\"\n", what_sector,
                    format_path_chain (path, NULL));
              show = TRUE;
            }
        }
      read_sec (d, dir, secno, 1, TRUE);
      last_secno = secno;
      n = MIN (sector_size / 32, entries);
      for (i = 0; i < n; ++i)
        {
          if (dir[i].name[0] == 0 && zero_ends_dir && !fix_zero_ends_dir)
	    {
	      *dir_end_flag = des_done;
	      goto done;
	    }
          do_dirent (d, (const BYTE *)dir, secno, &dir[i],
                     path, pv, parent_cluster,
                     start_cluster, dirent_index, &label_flag,
                     show, list, dir_end_flag, root_flag);
          ++dirent_index;
        }
      if (*dir_end_flag == des_fixed_write)
	{
	  if (!write_sec (d, dir, secno, 1))
	    {
	      diskio_close (d);
	      quit (2, FALSE);
	    }
	  *dir_end_flag = des_fixed;
	}
      ++secno; entries -= sector_size / 32;
    }
done:;
  free (dir);
}


#ifdef OS2
static void find_ea_data (DISKIO *d, ULONG secno, ULONG entries)
{
  ULONG i, n;
  FAT_DIRENT dir[512/32];

  ea_data_start = 0xffff; ea_data_size = 0;
  while (entries != 0)
    {
      read_sec (d, dir, secno, 1, FALSE);
      n = MIN (sector_size / 32, entries);
      for (i = 0; i < n; ++i)
        {
          if (dir[i].name[0] == 0)
            return;
          if (memcmp (dir[i].name, "EA DATA  SF", 8+3) == 0
              && !(dir[i].attr & (ATTR_LABEL|ATTR_DIR)))
            {
              ea_data_start = READ_USHORT (&dir[i].cluster16);
              ea_data_size = READ_ULONG (&dir[i].size);
              ea_data_clusters = DIVIDE_UP (ea_data_size, bytes_per_cluster);
              if (a_info)
                {
                  info ("\"EA DATA. SF\" 1st cluster:  %lu\n", ea_data_start);
                  info ("\"EA DATA. SF\" size:         %lu\n", ea_data_size);
                }
              return;
            }
        }
      ++secno; entries -= sector_size / 32;
    }
}
#endif

static void do_root_dir (DISKIO *d)
{
  ULONG secno;
  int list;

  secno = first_sector + number_of_fats * sectors_per_fat;

  list = FALSE;

  if (a_find && *find_path == 0)
    {
      if (a_where)
        info ("Root directory in %s\n",
              format_sector_range (secno, root_sectors));
      if (a_dir)
        list = TRUE;
      else
        quit (0, FALSE);
    }
  if (a_info)
    info ("Root directory:             %s\n",
          format_sector_range (secno, root_sectors));
  if (a_what)
    {
      if (!what_cluster_flag && IN_RANGE (what_sector, secno, root_sectors))
        info ("Sector #%lu: Root directory (+%lu)\n", what_sector,
              what_sector - secno);
    }

#ifdef OS2
  find_ea_data (d, secno, root_entries);
  read_ea_data (d);
#endif

  if (a_save || a_check || a_what || a_find)
    {
      path_chain link, *plink;
      struct vfat v;
      enum dir_end_state dir_end_flag = des_no_zero;

      v.flag = FALSE;
      plink = PATH_CHAIN_NEW (&link, NULL, "");
      do_dir (d, secno, root_entries, plink, &v, 0, 0, 0, 0, list,
	      &dir_end_flag, TRUE);
      do_enddir (d, plink, &v, list);
    }
}

static void do_root_dir_fat32 (DISKIO *d, ULONG root_cluster)
{
  int list = FALSE;

  if (a_save || a_check || a_what || a_find)
    {
      path_chain link, *plink;

      plink = PATH_CHAIN_NEW (&link, NULL, "");
      do_file (d, root_cluster, TRUE, plink, 0, 0, TRUE, 0, list, TRUE);
    }
}


static void check_alloc (void)
{
  ULONG i, start, count;
  BYTE *head_vector;
  int indent;

  i = 2; count = 0;
  while (i < total_clusters)
    {
      if (usage_vector[i] == USE_EMPTY && ALLOCATED (i))
        {
          start = i;
          do
            {
              ++i;
            } while (i < total_clusters
                     && usage_vector[i] == USE_EMPTY && ALLOCATED (i));
          if (check_unused)
            warning (0, "Unused but marked as allocated: %s",
                     format_cluster_range (start, i - start));
          count += i - start;
        }
      else
        ++i;
    }
  if (count == 1)
    warning (0, "The file system has 1 lost cluster");
  else if (count > 1)
    warning (0, "The file system has %lu lost clusters", count);

  head_vector = (BYTE *)xmalloc (total_clusters);
  memset (head_vector, 1, total_clusters);
  for (i = 2; i < total_clusters; ++i)
    if (ALLOCATED (i) && !LASTCLUSTER (i))
      head_vector[FAT_ENTRY(i)] = 0;
  for (i = 2; i < total_clusters; ++i)
    if (usage_vector[i] == USE_EMPTY && head_vector[i] && ALLOCATED (i))
      {
        start = 0; count = 0; indent = 0;
        info ("Lost chain: ");
        while (ALLOCATED (i))
          {
            if (count == 0)
              {
                start = i; count = 1;
              }
            else if (i == start + count)
              ++count;
            else
              {
                if (indent)
                  info ("            ");
                info ("%s\n", format_cluster_range (start, count));
                start = i; count = 1; indent = 1;
              }
            if (usage_vector[i] != USE_EMPTY)
              break;
            usage_vector[i] = USE_LOST;
            if (LASTCLUSTER (i))
              break;
            i = FAT_ENTRY (i);
          }
        if (count != 0)
          {
            if (indent)
              info ("            ");
            info ("%s", format_cluster_range (start, count));
            if (!LASTCLUSTER (i))
              info (" (loop or conflict)");
            info ("\n");
          }
      }
  free (head_vector);
}


/* Process a FAT volume. */

void do_fat (DISKIO *d, const FAT_SECTOR *pboot)
{
  ULONG i;
  ULONG root_cluster = 0;

  fat32_flag = FALSE;
  if (READ_USHORT (&pboot->boot.sectors_per_fat) == 0
          && READ_ULONG (&pboot->boot.r.fat32.sectors_per_fat) != 0)
    fat32_flag = TRUE;
  plenty_memory = TRUE;
  sector_size = READ_USHORT (&pboot->boot.bytes_per_sector);
  if (sector_size != 128 && sector_size != 256 && sector_size != 512
      && sector_size != 1024 && sector_size != 2048)
    error ("Sector size %u is not supported", (unsigned)sector_size);
  if (sector_size != 512 && a_save)
    error ("Sector size %u not supported for that operation",
           (unsigned)sector_size);
  diskio_set_sector_size (d, (unsigned)sector_size);
  if (pboot->boot.sectors_per_cluster == 0)
    error ("Cluster size is zero");
  if (pboot->boot.fats == 0)
    error ("Number of FATs is zero");
  first_sector = READ_USHORT (&pboot->boot.reserved_sectors);
  sectors_per_cluster = pboot->boot.sectors_per_cluster;
  bytes_per_cluster = sectors_per_cluster * sector_size;
  if (fat32_flag)
    sectors_per_fat = READ_ULONG (&pboot->boot.r.fat32.sectors_per_fat);
  else
    sectors_per_fat = READ_USHORT (&pboot->boot.sectors_per_fat);
  number_of_fats = pboot->boot.fats;

  if ((ULONG)use_fat > number_of_fats)
    error ("FAT specified on command line does not exist");

  if (READ_USHORT (&pboot->boot.sectors) != 0)
    total_sectors = READ_USHORT (&pboot->boot.sectors);
  else
    total_sectors = READ_ULONG (&pboot->boot.large_sectors);
  if (total_sectors < READ_USHORT (&pboot->boot.reserved_sectors))
    error ("Number of reserved sectors exceeds total number of sectors");
  total_sectors -= READ_USHORT (&pboot->boot.reserved_sectors);

  if (fat32_flag)
    {
      root_cluster = READ_ULONG (&pboot->boot.r.fat32.root_cluster);
      root_entries = 0;
      root_sectors = 0;
    }
  else
    {
      root_entries = READ_USHORT (&pboot->boot.root_entries);
      root_sectors = DIVIDE_UP (root_entries, sector_size / 32);
    }

  if (total_sectors < number_of_fats * sectors_per_fat + root_sectors)
    error ("Disk too small for FATs and root directory");
  total_clusters = ((total_sectors - number_of_fats * sectors_per_fat
                     - root_sectors)
                    / sectors_per_cluster);
  total_clusters += 2;
  if (total_clusters < 2)
    error ("Disk too small, no data clusters");
  if (!fat32_flag && total_clusters > 0xffff)
    warning (0, "Too many clusters");

  data_sector = (first_sector + number_of_fats * sectors_per_fat
                 + root_sectors);

  if (a_set && strcmp (get_name, "head") == 0)
    {
      if (set_ulong < 2 || set_ulong >= total_clusters)
        error ("Invalid value for `head'");
    }

  if (a_info)
    {
      unsigned bits;
      if (fat32_flag)
        bits = 32;
      else if (total_clusters - 2 > 4085)
        bits = 16;
      else
        bits = 12;
      info ("Number of clusters:         %lu\n", total_clusters - 2);
      info ("First data sector:          #%lu\n", data_sector);
      info ("Bits per FAT entry:         %u\n", bits);
    }

  if (a_what && what_cluster_flag)
    {
      if (what_sector < 2 || what_sector >= total_clusters)
        error ("Invalid cluster number");
      what_cluster = what_sector;
      what_sector = CLUSTER_TO_SECTOR (what_sector);
    }

  if (a_what)
    {
      if (!what_cluster_flag && what_sector == 0)
        info ("Sector #%lu: Boot sector\n", what_sector);
    }

  if (a_copy_fat)
    {
      ULONG bytes;
      BYTE *buf;
      if ((src_fat_path == NULL && src_fat_number > number_of_fats)
          || (dst_fat_path == NULL && dst_fat_number > number_of_fats))
        error ("Specified FAT number is out of range");
      if (sectors_per_fat > UINT_MAX / sector_size)
        error ("FAT too big");
      bytes = sectors_per_fat * sector_size;
      buf = xmalloc (bytes);
      if (src_fat_path == NULL)
        read_sec (d, buf, first_sector + (src_fat_number - 1) * sectors_per_fat,
                  sectors_per_fat, FALSE);
      else
        {
          size_t n;
          FILE *f = fopen (src_fat_path, "rb");
          if (f == NULL)
            error ("%s: %s", src_fat_path, strerror (errno));
          n = fread (buf, 1, bytes, f);
          if (ferror (f) || fclose (f) != 0)
            error ("%s: %s", src_fat_path, strerror (errno));
          if (n != bytes)
            error ("%s: file too short", src_fat_path);
        }
      if (dst_fat_path == NULL)
        {
          ULONG secno;
          secno = first_sector + (dst_fat_number - 1) * sectors_per_fat;
          write_sec (d, buf, secno, sectors_per_fat);
        }
      else
        {
          size_t n;
          FILE *f = fopen (dst_fat_path, "wb");
          if (f == NULL)
            error ("%s: %s", dst_fat_path, strerror (errno));
          n = fwrite (buf, 1, bytes, f);
          if (ferror (f) || fflush (f) != 0 || fclose (f) != 0)
            error ("%s: %s", dst_fat_path, strerror (errno));
          if (n != bytes)
            error ("%s: short write", dst_fat_path);
        }
      free (buf);
      return;
    }

  /* Allocate usage vector.  Initially, all clusters are unused. */

  usage_vector = (BYTE *)xmalloc (total_clusters);
  memset (usage_vector, USE_EMPTY, total_clusters);

  path_vector = xmalloc (total_clusters * sizeof (*path_vector));
  for (i = 0; i < total_clusters; ++i)
    path_vector[i] = NULL;

  if (fat32_flag)
    do_fats32 (d);
  else
    do_fats16 (d);

  if (a_what)
    {
      if (!what_cluster_flag
          && what_sector >= data_sector && what_sector < total_sectors)
        {
          i = SECTOR_TO_CLUSTER (what_sector);
          if (i >= 2 && i < total_clusters)
            {
              info ("Sector #%lu: Cluster %lu\n", what_sector, i);
              if (BADSECTOR (i))
                info ("Sector #%lu: Cluster contains bad sector\n",
                      what_sector);
              else if (LASTCLUSTER (i))
                info ("Sector #%lu: In last cluster of a file or directory\n",
                      what_sector);
              else if (UNUSED (i))
                info ("Sector #%lu: In an unused cluster\n", what_sector);
              else
                info ("Sector #%lu: In a used cluster\n", what_sector);
            }
        }
      else if (what_cluster_flag)
        {
          info ("Cluster %lu: %s\n", what_cluster,
                format_sector_range (CLUSTER_TO_SECTOR (what_cluster),
                                     sectors_per_cluster));
          if (BADSECTOR (what_cluster))
            info ("Cluster %lu: Cluster contains bad sector\n", what_cluster);
          else if (LASTCLUSTER (what_cluster))
            info ("Cluster %lu: Last cluster of a file or directory\n",
                  what_cluster);
          else if (UNUSED (what_cluster))
            info ("Cluster %lu: Unused\n", what_cluster);
          else
            info ("Cluster %lu: Used\n", what_cluster);
        }
    }

  if (fat32_flag)
    do_root_dir_fat32 (d, root_cluster);
  else
    do_root_dir (d);

  if (a_check)
    {
      check_alloc ();
    }
}
