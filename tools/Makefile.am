# Copyright (C) 2004 MySQL AB
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
# Process this file with automake to create Makefile.in

if HAVE_YASSL
  yassl_dummy_link_fix= $(top_srcdir)/extra/yassl/src/dummy.cpp
else
  yassl_dummy_link_fix=
endif
INCLUDES=	-I$(top_builddir)/include -I$(top_srcdir)/include \
		$(openssl_includes)
LDADD=		@CLIENT_EXTRA_LDFLAGS@ \
		$(top_builddir)/libmysql_r/libmysqlclient_r.la \
		@openssl_libs@ @yassl_libs@ @ZLIB_LIBS@ 
bin_PROGRAMS=		mysqltestmanager
mysqltestmanager_SOURCES=	mysqlmanager.c $(yassl_dummy_link_fix)
mysqltestmanager_DEPENDENCIES=	$(LIBRARIES) $(pkglib_LTLIBRARIES)
DEF=			-DUNDEF_THREADS_HACK

# Don't update the files from bitkeeper
%::SCCS/s.%
