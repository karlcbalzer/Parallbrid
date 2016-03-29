/* Copyright (C) 2012-2016 Free Software Foundation, Inc.
   Contributed by Karl Balzer <Salamahachy@googlemail.com>.

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
#include "invalbrid-mg.h" 

using namespace GTM;

namespace {
  
class sglsw_dispatch : public abi_dispatch
{
public:
  sglsw_dispatch(): abi_dispatch(method_group_invalbrid()) { }

protected:
  template <typename V> static V load(const V* addr, ls_modifier mod)
  {
    return *addr;
  }
  template <typename V> static void store(V* addr, const V value,
      ls_modifier mod)
  {
    *addr = value;
  }

public:
  static void memtransfer_static(void *dst, const void* src, size_t size,
      bool may_overlap, ls_modifier dst_mod, ls_modifier src_mod)
  {
    if (!may_overlap)
      ::memcpy(dst, src, size);
    else
      ::memmove(dst, src, size);
  }

  static void memset_static(void *dst, int c, size_t size, ls_modifier mod)
  {
    ::memset(dst, c, size);
  }
  
  CREATE_DISPATCH_METHODS(virtual, )
  CREATE_DISPATCH_METHODS_MEM()
  
  void begin() 
  { 
    gtm_thread *tx;
    pthread_mutex_lock(&invalbrid_mg::commit_lock);
    tx = gtm_thr();
    tx->state = gtm_thread::STATE_SERIAL | gtm_thread::STATE_IRREVOCABLE; 
     
  }
  
  gtm_restart_reason validate() { return NO_RESTART; }
  
  gtm_restart_reason trycommit() 
  {
    pthread_mutex_unlock(&invalbrid_mg::commit_lock);
    return NO_RESTART; 
  }
  
}; // sglsw_dispatch

static const sglsw_dispatch o_sglsw_dispatch;

} // anon

GTM::abi_dispatch *
GTM::dispatch_invalbrid_sglsw()
{
  return const_cast<sglsw_dispatch *>(&o_sglsw_dispatch);
}
