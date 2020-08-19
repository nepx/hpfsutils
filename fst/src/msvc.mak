#
# Copyright (c) 1995-2005 by Eberhard Mattes
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

CC=cl -O -W2 -DWINDOWS
DIR=windows
OUTEXE=-Fe
OUTOBJ=-Fo
OBJ=.obj
EXE=.exe
DO_HPFS_OBJ=
DISKIO_OBJ=diskio_u$(OBJ)
DEF_FILE=

!include common.mak
