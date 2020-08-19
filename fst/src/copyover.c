/* copyover.c -- Copy without truncating or extending target file
   Copyright (c) 2009 by Eberhard Mattes

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char banner[] =
"copyover 0.5b -- Copyright (c) 2008-2009 by Eberhard Mattes\n";

static void usage (void)
{
  puts (banner);
  puts ("copyover comes with ABSOLUTELY NO WARRANTY. For details see file\n"
        "`COPYING' that should have come with this program.\n"
        "fst is free software, and you are welcome to redistribute it\n"
        "under certain conditions. See the file `COPYING' for details.\n");
  puts ("Usage:\n"
        "  copyover <source_file> <target_file>");
  exit (1);
}

static void fail (const char *path)
{
  perror (path);
  exit (2);
}

static FILE *open_file (const char *path, const char *mode)
{
  FILE *f = fopen (path, mode);
  if (f == NULL)
    fail (path);
  return f;
}

static void write_file (FILE *f, const void *buf, int size, const char *path)
{
  int n = fwrite (buf, 1, size, f);
  if (ferror (f))
    fail (path);
  if (n < size)
    {
      fputs ("Short write\n", stderr);
      exit (2);
    }
}

static long get_size (FILE *f, const char *path)
{
  long size;
  if (fseek (f, 0, SEEK_END)!= 0)
    fail (path);
  size = ftell (f);
  if (size == -1)
    fail (path);
  if (fseek (f, 0, SEEK_SET)!= 0)
    fail (path);
  return size;
}

static const char *bytes (long n)
{
  return (n == 1) ? "byte" : "bytes";
}

int main (int argc, char *argv[])
{
  const char *src_path, *dst_path;
  FILE *src, *dst;
  long pos, src_size, dst_size;
  char buf[4096];

  if (argc != 3)
    usage ();
  src_path = argv[1];
  dst_path = argv[2];
  src = open_file (src_path, "rb");
  dst = open_file (dst_path, "r+b");
  src_size = get_size (src, src_path);
  dst_size = get_size (dst, dst_path);
  pos = 0;
  while (pos < dst_size)
    {
      int size, n;
      size = sizeof (buf);
      if (size > dst_size - pos)
        size = (int)(dst_size - pos);
      n = fread (buf, 1, size, src);
      if (ferror (src))
        fail (src_path);
      if (n == 0)
        break;
      write_file (dst, buf, n, dst_path);
      pos += n;
    }
  if (src_size > pos)
    printf ("Omitting %ld %s of source file\n",
            src_size - pos, bytes (src_size - pos));
  if (dst_size > pos)
    {
      printf ("Filling %ld %s of target file...\n",
              dst_size - pos, bytes (dst_size - pos));
      memset (buf, 0, sizeof (buf));
      while (pos < dst_size)
        {
          int n = sizeof (buf);
          if (n > dst_size - pos)
            n = (int)(dst_size - pos);
          write_file (dst, buf, n, dst_path);
          pos += n;
        }
    }
  if (fflush (dst) != 0 || fclose (dst) != 0)
    fail (dst_path);
  if (fclose (src) != 0)
    fail (src_path);
  puts ("Done.");
  return 0;
}
