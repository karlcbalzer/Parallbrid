/* Copyright (C) 2008-2016 Free Software Foundation, Inc.
   Contributed by Richard Henderson <rth@redhat.com>.

   This file is part of the GNU Transactional Memory Library (libitm).

   Libitm is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   Libitm is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
   more details.

   Under Section 7 of GPL version 3, you are granted additional
   permissions described in the GCC Runtime Library Exception, version
   3.1, as published by the Free Software Foundation.

   You should have received a copy of the GNU General Public License and
   a copy of the GCC Runtime Library Exception along with this program;
   see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
   <http://www.gnu.org/licenses/>.  */

#include "libitm_i.h"

using namespace GTM;

int ITM_REGPARM
_ITM_versionCompatible (int version)
{
  return version == _ITM_VERSION_NO;
}


const char * ITM_REGPARM
_ITM_libraryVersion (void)
{
  return "GNU libitm " _ITM_VERSION;
}


_ITM_howExecuting ITM_REGPARM
_ITM_inTransaction (void)
{
  // We let the method group determin if this execution is transactional,
  // because a hardware transaction might not have a thread object.
  if(unlikely(method_group::method_gr == 0))
    set_default_method_group();
  return method_group::method_gr->in_transaction();
}


_ITM_transactionId_t ITM_REGPARM
_ITM_getTransactionId (void)
{
  // We let the method group determin the transaction id. The reason is the same
  // as with inTransaction.
  if(unlikely(method_group::method_gr == 0))
    set_default_method_group();
  return method_group::method_gr->get_transaction_id();
}


void ITM_REGPARM ITM_NORETURN
_ITM_error (const _ITM_srcLocation * loc UNUSED, int errorCode UNUSED)
{
  abort ();
}
