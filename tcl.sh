#!/bin/sh
# Build for TCL
# (make DESTDIR=... doesn't work, so ./configure --prefix is used
. /etc/init.d/tc-functions

T='/tmp/mysql'
P='mysql-5.0.27.tcz'

echo "${BLUE}dependencies${NORMAL}"
tce-load -i bison ncurses-dev glibc_apps

echo "${BLUE}configure${NORMAL}"
./configure --prefix="/usr/local/mysql" --without-docs --without-bench --without-man --without-innodb

echo "${BLUE}make${NORMAL}"
make

echo "${BLUE}install${NORMAL}"
[ -d "${T}" ] && rm -rf "${T}"
mkdir "${T}"
make DESTDIR="${T}" install-strip

echo "${BLUE}build TCZ${NORMAL}"
cd /tmp
# Remove prev TCZs
[ -f "${P}" ] && rm "${P}"
mksquashfs mysql/ "${P}"
md5sum "${P}" > "${P}".md5.txt
printf "bison.tcz\nncurses.tcz\nglibc_apps.tcz\n" > "${P}".dep
mv -f "${P}"* /etc/sysconfig/tcedir/optional/

echo "${BLUE}please, setup my.cnf${NORMAL}"
