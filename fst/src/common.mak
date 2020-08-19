#
# Copyright (c) 1995-2009 by Eberhard Mattes
#
# This file is part of fst.
#
# fst is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# fst is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with fst; see the file COPYING.  If not, write to the
# the Free Software Foundation, 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.
#

default: $(DIR)/fst$(EXE) $(DIR)/copyover$(EXE)

FST_OBJS=$(DIR)/fst$(OBJ) $(DO_HPFS_OBJ) $(DIR)/do_fat$(OBJ) \
	$(DIR)/$(DISKIO_OBJ) $(DIR)/crc$(OBJ) $(DEF_FILE)

$(DIR)/fst$(EXE): $(FST_OBJS)
	$(CC) $(OUTEXE)$(DIR)/fst$(EXE) $(FST_OBJS)

$(DIR)/fst$(OBJ): fst.c fst.h do_hpfs.h do_fat.h crc.h diskio.h fat.h
	$(CC) -c $(OUTOBJ)$(DIR)/fst$(OBJ) fst.c

$(DIR)/do_hpfs$(OBJ): do_hpfs.c fst.h do_hpfs.h crc.h diskio.h hpfs.h
	$(CC) -c $(OUTOBJ)$(DIR)/do_hpfs$(OBJ) do_hpfs.c

$(DIR)/do_fat$(OBJ): do_fat.c fst.h do_fat.h fat.h crc.h diskio.h
	$(CC) -c $(OUTOBJ)$(DIR)/do_fat$(OBJ) do_fat.c

$(DIR)/diskio_2$(OBJ): diskio_2.c fst.h crc.h diskio.h
	$(CC) -c $(OUTOBJ)$(DIR)/diskio_2$(OBJ) diskio_2.c

$(DIR)/diskio_u$(OBJ): diskio_u.c fst.h crc.h diskio.h
	$(CC) -c $(OUTOBJ)$(DIR)/diskio_u$(OBJ) diskio_u.c

$(DIR)/crc$(OBJ): crc.c crc.h
	$(CC) -c $(OUTOBJ)$(DIR)/crc$(OBJ) crc.c

$(DIR)/copyover$(EXE): $(DIR)/copyover$(OBJ)
	$(CC) $(OUTEXE)$(DIR)/copyover$(EXE) $(DIR)/copyover$(OBJ)

$(DIR)/copyover$(OBJ): copyover.c
	$(CC) -c $(OUTOBJ)$(DIR)/copyover$(OBJ) copyover.c

$(DIR)/fst.sis: epocemx/fst.exe fst.doc fst.pkg
	emxsis fst.pkg $(DIR)/fst.sis

fst.zip: epocemx/fst.sis os2/fst.exe windows/fst.exe windows/copyover.exe
	mkdir src
	cp EM *.c *.h *.mak fst.def fst.pkg src
	mkdir src/{emx,os2,windows}
	zip -9rp fst COPYING epocemx/fst.sis fst.doc src epocemx/fst.exe windows/fst.exe windows/copyover.exe os2/fst.exe
	rm -rf src

clean:
	rm -f $(DIR)/*$(OBJ) $(DIR)/fst$(EXE) $(DIR)/copyover$(EXE) epocemx/fst.sis
