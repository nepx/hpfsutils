/* diskio_u.c -- Disk/sector I/O for fst
   Copyright (c) 2001-2008 by Eberhard Mattes

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


#ifdef WINDOWS
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include "fst.h"
#include "crc.h"
#include "diskio.h"

/* Size of the hash table used for speeding up reading snapshot files. */

#define HASH_SIZE       997

/* This value marks the end of a hash chain.  It should be an
   `impossible' sector number. */

#define HASH_END        0xffffffff

/* Method for reading and writing sectors. */

enum disk_io_type
{
  DIOT_FILE,                    /* File or device */
  DIOT_SNAPSHOT,                /* Snapshot file */
  DIOT_CRC                      /* CRC file */
};

/* Data for DIOT_FILE. */

struct diskio_file
{
#ifdef WINDOWS
  HANDLE h;                     /* Handle */
#else
  FILE *f;                      /* Stream */
#endif
};

/* Data for DIOT_SNAPSHOT. */

struct diskio_snapshot
{
  FILE *f;                      /* Stream */
  ULONG sector_count;           /* Total number of sectors */
  ULONG *sector_map;            /* Table containing relative sector numbers */
  ULONG *hash_next;             /* Hash chains */
  ULONG version;                /* Format version number */
  ULONG hash_start[HASH_SIZE];  /* Hash chain heads */
};

/* Data for DIOT_CRC. */

struct diskio_crc
{
  FILE *f;                      /* Stream */
  ULONG version;                /* Format version number */
  crc_t *vec;                   /* See diskio_crc_load() */
};

/* DISKIO structure. */

struct diskio
{
  enum disk_io_type type;       /* Method */
  ULONG sector_size;            /* Bytes per sector */
  ULONG total_sectors;          /* Total number of sectors */
  union
    {
      struct diskio_file file;
      struct diskio_snapshot snapshot;
      struct diskio_crc crc;
    } x;                        /* Method-specific data */
};

/* This variable selects the method of direct disk I/O to use.
   ACCESS_DASD selects DosRead and DosWrite, ACCESS_LOG_TRACK selects
   DSK_READTRACK and DSK_WRITETRACK for logical disks.  (Ignored under
   Unix.) */

enum access_type diskio_access;

/* Non-zero if write access is required. */

char write_enable;

/* Non-zero if removable disks are allowed.  (Ignored under Unix.) */

char removable_allowed;

/* Non-zero to ignore failure to lock the disk.  (Ignore under Unix.) */

char ignore_lock_error;

/* Non-zero to disable locking (FOR TESTING ONLY!). */

char dont_lock;

/* Type of the save file. */

enum save_type save_type;

/* This is the save file. */

FILE *save_file;

/* Name of the save file.  There is no save file if this variable is
   NULL. */

const char *save_fname;

/* Number of sectors written to the save file. */

ULONG save_sector_count;

/* Number of elements allocated for save_sector_map. */

ULONG save_sector_alloc;

/* This table maps logical sector numbers to relative sector numbers
   in the snap shot file, under construction. */

ULONG *save_sector_map;


/* Obtain access to a device, file, snapshot file, or CRC file.  FNAME
   is the name of the disk or file to open.  FLAGS defines what types
   of files are allowed; FLAGS is the inclusive OR of one or more of
   DIO_DISK, DIO_SNAPSHOT, and DIO_CRC.  Open for writing if FOR_WRITE
   is non-zero. */

DISKIO *diskio_open (PCSZ fname, unsigned flags, ULONG sector_size,
                     int for_write)
{
  FILE *f = NULL;
  DISKIO *d;
  header hdr;
  size_t n;
  int r;
  ULONG i, hash, *map;
  const char *llfn = fname;

  /* Writing required the -w option.  On the other hand, -w should not
     be used unless writing is requested. */

  if (!for_write && write_enable)
    error ("Do not use the -w option for actions that don't write sectors");
  if (for_write && !write_enable)
    error ("Use the -w option for actions that write sectors");

  /* Allocate a DISKIO structure. */

  d = xmalloc (sizeof (*d));

#ifdef __EPOC32__
  /* Translate drive names for user convenience. */
  if (strlen (fname) == 2 && fname[1] == ':')
    {
      static char raw[] = "/dev/raw/x";
      raw[9] = fname[0];
      llfn = raw;
    }
#endif
#ifdef WINDOWS
  /* Translate drive names for user convenience. */
  if (strlen (fname) == 2 && fname[1] == ':')
    {
      if ((fname[0] >= 'a' && fname[0] <= 'z') ||
          (fname[0] >= 'A' && fname[0] <= 'Z'))
        {
          static char raw[] = "//./x:";
          raw[4] = fname[0];
          llfn = raw;
        }
      else if (fname[0] >= '0' && fname[0] <= '9')
        {
          static char raw[] = "//./physicaldrive#";
          raw[17] = fname[0];
          llfn = raw;
        }
    }
#endif

  d->sector_size = sector_size;

  /* Open the file. */

  if (for_write)
    f = fopen (llfn, "r+b");
  else
    f = fopen (llfn, "rb");
  if (f == NULL)
    error ("Cannot open %s (%s)", fname, strerror (errno));

  /* Read the header and check the magic number. */

  n = fread (&hdr, 1, sizeof (hdr), f);
  if (ferror (f))
    error ("Cannot read %s (%s)", fname, strerror (errno));
  if (n != 512
      || !(((flags & DIO_SNAPSHOT)
	    && READ_ULONG (&hdr.magic) == SNAPSHOT_MAGIC)
	   || ((flags & DIO_CRC)
	       && READ_ULONG (&hdr.magic) == CRC_MAGIC)
	   || (flags & DIO_DISK)))
    {
      switch (flags & (DIO_SNAPSHOT | DIO_CRC))
	{
	case DIO_SNAPSHOT:
	  error ("%s is not a snapshot file", fname);
	case DIO_CRC:
	  error ("%s is not a CRC file", fname);
	case DIO_SNAPSHOT|DIO_CRC:
	  error ("%s is neither a snapshot file nor a CRC file", fname);
	}
    }

  /* Further processing depends on the magic number. */

  switch (READ_ULONG (&hdr.magic))
    {
    case SNAPSHOT_MAGIC:

      /* Check the header of a snapshot file and remember the values
	 of the header. */

      if (READ_ULONG (&hdr.s.version) > 1)
	error ("Format of %s too new -- please upgrade this program",
	       fname);
      if (sector_size != 512)
        error ("Unsupported sector size");
      d->x.snapshot.f = f;
      d->x.snapshot.sector_count = READ_ULONG (&hdr.s.sector_count);
      d->x.snapshot.version = READ_ULONG (&hdr.s.version);
      r = fseek (f, READ_ULONG (&hdr.s.map_pos), SEEK_SET);
      if (r != 0)
	error ("Cannot read %s (%s)", fname, strerror (errno));

      /* Load the sector map. */

      map = xmalloc (d->x.snapshot.sector_count * sizeof (ULONG));
      d->x.snapshot.sector_map = map;
      d->x.snapshot.hash_next = xmalloc (d->x.snapshot.sector_count * sizeof (ULONG));
      r = fread (map, sizeof (ULONG), d->x.snapshot.sector_count, f);
      if (ferror (f))
	error ("Cannot read %s (%s)", fname, strerror (errno));
      if (r != d->x.snapshot.sector_count)
	error ("Cannot read %s", fname);

      for (i = 0; i < d->x.snapshot.sector_count; ++i)
	map[i] = READ_ULONG (&map[i]);

      /* Initialize hashing. */

      for (i = 0; i < HASH_SIZE; ++i)
	d->x.snapshot.hash_start[i] = HASH_END;
      for (i = 0; i < d->x.snapshot.sector_count; ++i)
	d->x.snapshot.hash_next[i] = HASH_END;
      for (i = 0; i < d->x.snapshot.sector_count; ++i)
	{
	  hash = map[i] % HASH_SIZE;
	  d->x.snapshot.hash_next[i] = d->x.snapshot.hash_start[hash];
	  d->x.snapshot.hash_start[hash] = i;
	}
      d->total_sectors = 0;
      d->type = DIOT_SNAPSHOT;
      break;

    case CRC_MAGIC:

      /* Check the header of a CRC file and remember the values of the
	 header. */

      if (READ_ULONG (&hdr.c.version) > 1)
	error ("Format of %s too new -- please upgrade this program",
	       fname);
      if (sector_size != 512)
        error ("Unsupported sector size");

      /* Build a C stream from the file handle. */

#ifdef OS2
#ifdef __EMX__
      h = _imphandle ((int)hf);
      if (h == -1)
	error ("%s: %s", (const char *)fname, strerror (errno));
#else
      h = (int)hf;
      if (setmode (h, O_BINARY) == -1)
	error ("%s: %s", (const char *)fname, strerror (errno));
#endif
      d->x.crc.f = fdopen (h, (for_write ? "r+b" : "rb"));
#else
      d->x.crc.f = f;
#endif

      /* Remember values of the header. */

      d->total_sectors = READ_ULONG (&hdr.c.sector_count);
      d->x.crc.version = READ_ULONG (&hdr.c.version);
      d->x.crc.vec = NULL;  /* CRCs not read into memory */

      /* Seek to the first CRC. */

      fseek (d->x.crc.f, 512, SEEK_SET);
      d->type = DIOT_CRC;
      break;

    default:
      d->type = DIOT_FILE;
#ifdef WINDOWS
      {
        HANDLE h;
        DISK_GEOMETRY geom;
        DWORD nread;
        DWORD acc = FILE_READ_DATA | FILE_READ_ATTRIBUTES;
        if (for_write)
          acc |= FILE_WRITE_DATA;
        fclose (f);
        h = CreateFile (llfn, acc, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
          error ("Cannot open %s (error %ld)", fname, (long)GetLastError ());
        d->x.file.h = h;
        memset (&geom, 0, sizeof (geom));
        if (DeviceIoControl (d->x.file.h, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                             NULL, 0,
                             &geom, sizeof (geom), &nread, NULL))
          {
            if (geom.BytesPerSector != sector_size)
              error ("Please use -ss=%u", (unsigned)geom.BytesPerSector);
          }
        else
          {
            long e = GetLastError ();
            if (e != ERROR_INVALID_PARAMETER)
              error ("Cannot get disk geometry, error %ld", e);
            /* It's a file */
          }
      }
#else
      d->x.file.f = f;
#endif
      diskio_set_sector_size (d, sector_size);
      break;
    }
  return d;
}

void diskio_set_sector_size (DISKIO *d, ULONG sector_size)
{
  d->sector_size = sector_size;
  if (d->type == DIOT_FILE)
    {
#ifdef WINDOWS
      LARGE_INTEGER size;
      if (!GetFileSizeEx (d->x.file.h, &size))
        {
          PARTITION_INFORMATION part;
          DWORD nread;
          memset (&part, 0, sizeof (part));
          if (!DeviceIoControl (d->x.file.h, IOCTL_DISK_GET_PARTITION_INFO,
                                NULL, 0,
                                &part, sizeof (part), &nread, NULL))
            error ("Cannot get partition information, error %ld", (long)GetLastError ());
          size = part.PartitionLength;
        }
      size.QuadPart /= sector_size;
      if ((ULONGLONG)size.QuadPart > ULONG_MAX)
        error ("Too many sectors");
      d->total_sectors = (ULONG)size.QuadPart;
#elif defined (_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS >= 32 + 9
      {
        off_t size;
        fseeko (d->x.file.f, 0L, SEEK_END);
        size = ftello (d->x.file.f);
        size /= sector_size;
        if (size > ULONG_MAX)
          error ("Too many sectors");
        d->total_sectors = (ULONG)size;
      }
#else
      fseek (d->x.file.f, 0L, SEEK_END);
      d->total_sectors = ftell (d->x.file.f) / sector_size;
#endif
    }
}

/* Close a DISKIO returned by diskio_open(). */

void diskio_close (DISKIO *d)
{
  int r;
  switch (d->type)
    {
    case DIOT_FILE:
#ifdef WINDOWS
      if (!CloseHandle (d->x.file.h))
        error ("CloseHandle failed, error %ld\n", (long)GetLastError ());
      free (d);
      return;
#else
      r = fclose (d->x.file.f);
#endif
      break;
    case DIOT_SNAPSHOT:
      r = fclose (d->x.snapshot.f);
      free (d->x.snapshot.sector_map);
      free (d->x.snapshot.hash_next);
      break;
    case DIOT_CRC:
      r = fclose (d->x.crc.f);
      break;
    default:
      abort ();
    }
  if (r != 0)
    error ("fclose(): %s", strerror (errno));
  free (d);
}


/* Return the type of a DISKIO. */

unsigned diskio_type (DISKIO *d)
{
  switch (d->type)
    {
    case DIOT_FILE:
      return DIO_DISK;
    case DIOT_SNAPSHOT:
      return DIO_SNAPSHOT;
    case DIOT_CRC:
      return DIO_CRC;
    default:
      abort ();
    }
}


/* Return the number of sectors covered by a DISKIO. */

ULONG diskio_total_sectors (DISKIO *d)
{
  return d->total_sectors;
}


/* TODO: Move to separate file (code copied from diskio_2.c) */

/* Return the number of snapshot sectors. */

ULONG diskio_snapshot_sectors (DISKIO *d)
{
  if (d->type != DIOT_SNAPSHOT)
    abort ();
  return d->x.snapshot.sector_count;
}


/* Compare two sector numbers, for qsort(). */

static int snapshot_sort_comp (const void *x1, const void *x2)
{
  const ULONG *p1 = (const ULONG *)x1;
  const ULONG *p2 = (const ULONG *)x2;
  if (*p1 < *p2)
    return -1;
  else if (*p1 > *p2)
    return 1;
  else
    return 0;
}


/* Return a sorted array of all sector numbers of a snapshot file.  If
   DISKIO is not associated with a snapshot file, return NULL. */

ULONG *diskio_snapshot_sort (DISKIO *d)
{
  ULONG *p;
  size_t n;

  if (diskio_type (d) != DIO_SNAPSHOT)
    return NULL;
  n = (size_t)d->x.snapshot.sector_count;
  p = xmalloc (n * sizeof (ULONG));
  memcpy (p, d->x.snapshot.sector_map, n * sizeof (ULONG));
  qsort (p, n, sizeof (ULONG), snapshot_sort_comp);
  return p;
}


/* Read all CRCs of the CRC file associated with D into memory, unless
   there are too many CRCs.  This is used for speeding up
   processing. */

void diskio_crc_load (DISKIO *d)
{
  ULONG i;

  if (d->type != DIOT_CRC || d->x.crc.vec != NULL)
    abort ();
  if (d->total_sectors * sizeof (crc_t) >= 8*1024*1024)
    return;
  d->x.crc.vec = xmalloc (d->total_sectors * sizeof (crc_t));
  fseek (d->x.crc.f, 512, SEEK_SET);
  if (fread (d->x.crc.vec, sizeof (crc_t), d->total_sectors,
             d->x.crc.f) != d->total_sectors)
    error ("Cannot read CRC file");
  for (i = 0; i < d->total_sectors; ++i)
    d->x.crc.vec[i] = READ_ULONG (&d->x.crc.vec[i]);
}


/* Write the sector with number SEC and data SRC to the save file. */

static void save_one_sec (const void *src, ULONG sec)
{
  ULONG i, *sig;
  BYTE raw[512];

  for (i = 0; i < save_sector_count; ++i)
    if (save_sector_map[i] == sec)
      return;
  if (save_sector_count >= save_sector_alloc)
    {
      save_sector_alloc += 1024;
      save_sector_map = realloc (save_sector_map,
                                 save_sector_alloc * sizeof (ULONG));
      if (save_sector_map == NULL)
        error ("Out of memory");
    }
  save_sector_map[save_sector_count++] = sec;
  memcpy (raw, src, 512);

  /* Scramble the signature so that there are no sectors with the
     original HPFS sector signatures.  This simplifies recovering HPFS
     file systems and undeleting files. */

  sig = (ULONG *)raw;
  WRITE_ULONG (sig, READ_ULONG (sig) ^ SNAPSHOT_SCRAMBLE);
  if (fwrite (raw, 512, 1, save_file) != 1)
    save_error ();
}


/* Write COUNT sectors starting at number SEC to the save file. */

void save_sec (const void *src, ULONG sec, ULONG count)
{
  const char *p;

  p = (const char *)src;
  while (count != 0)
    {
      save_one_sec (p, sec);
      p += 512; ++sec; --count;
    }
}


/* Create a save file of type TYPE.  The file name is passed in the
   global variable `save_fname'.  Complain if the file would be on the
   drive AVOID_FNAME. */

void save_create (const char *avoid_fname, enum save_type type)
{
  header hdr;

  save_file = fopen (save_fname, "wb");
  if (save_file == NULL)
    save_error ();
  save_type = type;
  switch (save_type)
    {
    case SAVE_SNAPSHOT:
      save_sector_count = 0;
      save_sector_alloc = 0;
      save_sector_map = NULL;
      memset (&hdr, 0, sizeof (hdr));
      fwrite (&hdr, sizeof (hdr), 1, save_file);
      break;

    case SAVE_CRC:
      save_sector_count = 0;
      memset (&hdr, 0, sizeof (hdr));
      fwrite (&hdr, sizeof (hdr), 1, save_file);
      break;

    default:
      break;
    }
}


/* Error while writing to the save file. */

void save_error (void)
{
  error ("%s: %s", save_fname, strerror (errno));
}


/* Close the save file and update the header. */

void save_close (void)
{
  header hdr;
  ULONG i;

  switch (save_type)
    {
    case SAVE_SNAPSHOT:
      memset (&hdr, 0, sizeof (hdr));
      WRITE_ULONG (&hdr.s.magic, SNAPSHOT_MAGIC);
      WRITE_ULONG (&hdr.s.sector_count, save_sector_count);
      WRITE_ULONG (&hdr.s.map_pos, ftell (save_file));
      WRITE_ULONG (&hdr.s.version, 1); /* Scrambled */
      for (i = 0; i < save_sector_count; ++i)
        WRITE_ULONG (&save_sector_map[i], save_sector_map[i]);
      if (fwrite (save_sector_map, sizeof (ULONG), save_sector_count,
                  save_file)
          != save_sector_count || fseek (save_file, 0L, SEEK_SET) != 0)
        save_error ();
      fwrite (&hdr, sizeof (hdr), 1, save_file);
      for (i = 0; i < save_sector_count; ++i)
        save_sector_map[i] = READ_ULONG (&save_sector_map[i]);
      break;

    case SAVE_CRC:
      memset (&hdr, 0, sizeof (hdr));
      WRITE_ULONG (&hdr.c.magic, CRC_MAGIC);
      WRITE_ULONG (&hdr.c.sector_count, save_sector_count);
      WRITE_ULONG (&hdr.c.version, 1);
      if (fseek (save_file, 0L, SEEK_SET) != 0)
        save_error ();
      fwrite (&hdr, sizeof (hdr), 1, save_file);
      break;

    default:
      break;
    }

  if (fclose (save_file) != 0)
    save_error ();
  save_file = NULL;
}


/* Convert the logical sector number SECNO to cylinder number
   (0-based), head number (0-based), and sector number (1-based).
   Return TRUE iff successful. */

int diskio_cyl_head_sec (DISKIO *d, cyl_head_sec *dst, ULONG secno)
{
  return FALSE;
}


/* Return the relative sector number of sector N in the snapshot file
   associated with D.  Return 0 if there is no such sector (relative
   sector number 0 is the header of the snapshot file). */

ULONG find_sec_in_snapshot (DISKIO *d, ULONG n)
{
  ULONG j;

  for (j = d->x.snapshot.hash_start[n % HASH_SIZE]; j != HASH_END;
       j = d->x.snapshot.hash_next[j])
    if (d->x.snapshot.sector_map[j] == n)
      return j + 1;
  return 0;
}


/* Seek to sector SEC in file F. There are SIZE bytes per sector. */

static void seek_sec (FILE *f, ULONG sec, ULONG size)
{
  int r;
#if defined (_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS >= 32 + 9
  off_t off;
  if (sec > (1ULL << (_FILE_OFFSET_BITS - 1)) / size)
    error ("Sector number #%lu out of range", sec);
  off = sec;
  off *= size;
  r = fseeko (f, off, SEEK_SET);
#else
  if (sec < LONG_MAX / size)
    r = fseek (f, sec * size, SEEK_SET);
  else
    {
      ULONG rest = sec;

      r = fseek (f, 0L, SEEK_SET);
      while (r == 0 && rest > 0)
	{
	  long n = (rest < LONG_MAX / size) ? rest : LONG_MAX / size;
	  r = fseek (f, n * size, SEEK_CUR);
	  rest -= n;
	}
    }
#endif
  if (r != 0)
    error ("Cannot seek to sector #%lu (%s)", sec, strerror (errno));
}


/* Read COUNT sectors from F.  There are SIZE bytes per sector. */

static void read_sec_file (FILE *f, void *dst, ULONG sec, ULONG size,
                           ULONG count)
{
  int r;

  seek_sec (f, sec, size);
  r = fread (dst, size, count, f);
  if (ferror (f))
    error ("Cannot read sector #%lu (%s)", sec, strerror (errno));
  if (r != count)
    error ("EOF reached while reading sector #%lu", sec);
}


/* Read COUNT sectors from D to DST.  SEC is the starting sector
   number.  Copy the sector to the save file if SAVE is non-zero. */

void read_sec (DISKIO *d, void *dst, ULONG sec, ULONG count, int save)
{
  ULONG i, j, n;
  char *p;

  switch (d->type)
    {
    case DIOT_FILE:
#ifdef WINDOWS
      {
        DWORD nread;
        LARGE_INTEGER pos;
        pos.QuadPart = sec;
        pos.QuadPart *= d->sector_size;
        if (!SetFilePointerEx (d->x.file.h, pos, NULL, FILE_BEGIN))
          error ("Cannot seek to sector #%lu (error %ld)", sec, (long)GetLastError ());
        if (!ReadFile (d->x.file.h, dst, count * d->sector_size, &nread, NULL))
          error ("Cannot read sector #%lu (error %ld)", sec, (long)GetLastError ());
        if (nread != count * d->sector_size)
          error ("EOF reached while reading sector #%lu", sec);
      }
#else
      read_sec_file (d->x.file.f, dst, sec, d->sector_size, count);
#endif
      break;
    case DIOT_SNAPSHOT:
      p = (char *)dst; n = sec;
      for (i = 0; i < count; ++i)
        {
          j = find_sec_in_snapshot (d, n);
          if (j == 0)
            error ("Sector #%lu not found in snapshot file", n);
          read_sec_file (d->x.snapshot.f, p, j, d->sector_size, 1);
          if (d->x.snapshot.version >= 1)
	    {
	      ULONG *sig = (ULONG *)p;
	      WRITE_ULONG (sig, READ_ULONG (sig) ^ SNAPSHOT_SCRAMBLE);
	    }
          p += 512; ++n;
        }
      break;
    default:
      abort ();
    }
  if (a_save && save)
    save_sec (dst, sec, count);
}


/* Store the CRC of sector SECNO to the object pointed to by PCRC. */

int crc_sec (DISKIO *d, crc_t *pcrc, ULONG secno)
{
  if (d->type == DIOT_CRC)
    {
      if (secno >= d->total_sectors)
        return FALSE;
      if (d->x.crc.vec != NULL)
        {
          *pcrc = d->x.crc.vec[secno];
          return TRUE;
        }
      fseek (d->x.crc.f, 512 + secno * sizeof (crc_t), SEEK_SET);
      if (fread (pcrc, sizeof (crc_t), 1, d->x.crc.f) != 1)
        error ("CRC file: %s", strerror (errno));
      *pcrc = READ_ULONG (pcrc);
      return TRUE;
    }
  else
    {
      BYTE data[512];

      read_sec (d, data, secno, 1, FALSE);
      *pcrc = crc_compute (data, 512);
      return TRUE;
    }
}


/* Write COUNT sectors at SEC to F.  There are SIZE bytes per sector. */

static int write_sec_file (FILE *f, const void *src, ULONG sec, ULONG size,
                           ULONG count)
{
  int r;

  seek_sec (f, sec, size);
  r = fwrite (src, size, count, f);
  if (ferror (f))
    {
      warning (1, "Cannot write sector #%lu (%s)", sec, strerror (errno));
      return FALSE;
    }
  if (r != count)
    {
      warning (1, "Incomplete write for sector #%lu", sec);
      return FALSE;
    }
  return TRUE;
}


/* Replace the sector SEC in the snapshot file associated with D.
   Return FALSE on failure. */

static int write_sec_snapshot (DISKIO *d, const void *src, ULONG sec)
{
  BYTE raw[512];
  ULONG j;

  j = find_sec_in_snapshot (d, sec);
  if (j == 0)
    {
      warning (1, "Sector #%lu not found in snapshot file", sec);
      return FALSE;
    }

  memcpy (raw, src, 512);
  /* Scramble the signature so that there are no sectors with the
     original HPFS sector signatures.  This simplifies recovering HPFS
     file systems and undeleting files. */
  if (d->x.snapshot.version >= 1)
    {
      ULONG *sig = (ULONG *)raw;
      WRITE_ULONG (sig, READ_ULONG (sig) ^ SNAPSHOT_SCRAMBLE);
    }

  return write_sec_file (d->x.snapshot.f, raw, j, d->sector_size, 1);
}


/* Write COUNT sectors from SRC to SEC at D  */

int write_sec (DISKIO *d, const void *src, ULONG sec, ULONG count)
{
  switch (d->type)
    {
    case DIOT_FILE:
#ifdef WINDOWS
      {
        DWORD nwritten;
        LARGE_INTEGER pos;
        pos.QuadPart = sec;
        pos.QuadPart *= d->sector_size;
        if (!SetFilePointerEx (d->x.file.h, pos, NULL, FILE_BEGIN))
          error ("Cannot seek to sector #%lu (error %ld)", sec, (long)GetLastError ());
        if (!WriteFile (d->x.file.h, src, count * d->sector_size, &nwritten, NULL))
          error ("Cannot write sector #%lu (error %ld)", sec, (long)GetLastError ());
        if (nwritten != count * d->sector_size)
          error ("Short write for sector #%lu", sec);
        return TRUE;
      }
#else
      return write_sec_file (d->x.file.f, src, sec, d->sector_size, count);
#endif
    case DIOT_SNAPSHOT:
      {
        const unsigned char *s = (const unsigned char *)src;
        while (count > 0)
          {
            if (!write_sec_snapshot (d, s, sec))
              return FALSE;
            sec += 1; s += 512; count -= 1;
          }
      }
    default:
      abort ();
    }
  return FALSE;
}
