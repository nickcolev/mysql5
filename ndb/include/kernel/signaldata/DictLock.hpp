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

#ifndef DICT_LOCK_HPP
#define DICT_LOCK_HPP

#include "SignalData.hpp"

// see comments in Dbdict.hpp

class DictLockReq {
  friend class Dbdict;
  friend class Dbdih;
public:
  STATIC_CONST( SignalLength = 3 );
  enum LockType {
    NoLock = 0,
    NodeRestartLock = 1
  };
private:
  Uint32 userPtr;
  Uint32 lockType;
  Uint32 userRef;
};

class DictLockConf {
  friend class Dbdict;
  friend class Dbdih;
public:
  STATIC_CONST( SignalLength = 3 );
private:
  Uint32 userPtr;
  Uint32 lockType;
  Uint32 lockPtr;
};

class DictLockRef {
  friend class Dbdict;
  friend class Dbdih;
public:
  STATIC_CONST( SignalLength = 3 );
  enum ErrorCode {
    NotMaster = 1,
    InvalidLockType = 2,
    BadUserRef = 3,
    TooLate = 4,
    TooManyRequests = 5
  };
private:
  Uint32 userPtr;
  Uint32 lockType;
  Uint32 errorCode;
};

class DictUnlockOrd {
  friend class Dbdict;
  friend class Dbdih;
public:
  STATIC_CONST( SignalLength = 2 );
private:
  Uint32 lockPtr;
  Uint32 lockType;
};

#endif
