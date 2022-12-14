/* Copyright (C) 2003 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef BACKUP_CONTINUEB_H
#define BACKUP_CONTINUEB_H

#include "SignalData.hpp"

class BackupContinueB {
  /**
   * Sender(s)/Reciver(s)
   */
  friend class Backup;
  friend bool printCONTINUEB_BACKUP(FILE * output, const Uint32 * theData, Uint32 len);
private:
  enum {
    START_FILE_THREAD = 0,
    BUFFER_UNDERFLOW  = 1,
    BUFFER_FULL_SCAN  = 2,
    BUFFER_FULL_FRAG_COMPLETE = 3,
    BUFFER_FULL_META  = 4,
    BACKUP_FRAGMENT_INFO = 5
  };
};

#endif
