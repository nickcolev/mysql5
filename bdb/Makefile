# Copyright (C) 2000 MySQL AB & MySQL Finland AB & TCX DataKonsult AB
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Adaptor makefile to translate between what automake expects and what
# BDB provides (or vice versa).

srcdir = .
top_srcdir = ..

# distdir and top_distdir are set by the calling Makefile

bdb_build = build_unix
files = LICENSE Makefile Makefile.in README CMakeLists.txt
subdirs = btree build_vxworks build_win32 clib common cxx db dbinc \
  dbinc_auto db185 db_archive db_checkpoint db_deadlock db_dump \
  db_dump185 db_load db_printlog db_recover db_stat db_upgrade \
  db_verify dbm dbreg dist env examples_c examples_cxx fileops hash \
	hsearch hmac include java libdb_java lock log mp mutex os os_vxworks \
	os_win32 perl.BerkeleyDB perl.DB_File qam rep rpc_client rpc_server tcl \
	test txn xa



all:
	cd $(bdb_build) && $(MAKE) all

clean:
	cd $(bdb_build) && $(MAKE) clean

distclean:
	cd $(bdb_build) && $(MAKE) distclean

# May want to fix this, and MYSQL/configure, to install things
install dvi check installcheck:

distdir:
	for s in $(subdirs); do \
	  cp -pr $(srcdir)/$$s $(distdir)/$$s; \
	done
	for f in $(files); do \
	  test -f $(distdir)/$$f || cp -p $(srcdir)/$$f $(distdir)/$$f; \
	done
	mkdir $(distdir)/$(bdb_build)
	cp -p $(srcdir)/$(bdb_build)/.IGNORE_ME $(distdir)/$(bdb_build)
