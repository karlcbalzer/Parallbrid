/* Copyright (C) 2012-2016 Free Software Foundation, Inc.
   Contributed by Karl Balzer <Karl.C.Balzer@gmail.com>.

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
#include <stdio.h>

using namespace GTM;

namespace {

class irrevocsw_dispatch : public abi_dispatch
{
public:
  irrevocsw_dispatch(): abi_dispatch(method_group_invalbrid(), false, false) { }

protected:
  template <typename V> static V load(const V* addr, ls_modifier mod)
  {
    return *addr;
  }
  template <typename V> static void store(V* addr, const V value,
      ls_modifier mod)
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load();
    // The addresses will be added to the writeset.
    bloomfilter *bf = spec_data->writeset.load();
    bf->add_address((void*) addr, sizeof(V));
    *addr = value;
  }

public:
  static void memtransfer_static(void *dst, const void* src, size_t size,
      bool may_overlap, ls_modifier dst_mod, ls_modifier src_mod)
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load();
    // The addresses will be added to the writeset.
    bloomfilter *bf = spec_data->writeset.load();
    bf->add_address(dst, size);
    if (!may_overlap)
      ::memcpy(dst, src, size);
    else
      ::memmove(dst, src, size);
  }

  static void memset_static(void *dst, int c, size_t size, ls_modifier mod)
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load();
    // The addresses will be added to the writeset.
    bloomfilter *bf = spec_data->writeset.load();
    bf->add_address(dst, size);
    ::memset(dst, c, size);
  }

  CREATE_DISPATCH_METHODS(virtual, )
  CREATE_DISPATCH_METHODS_MEM()

  void
  begin()
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_mg* mg = (invalbrid_mg*)m_method_group;
    // Acquire the commit lock.
    pthread_mutex_lock(&invalbrid_mg::commit_lock);
    invalbrid_mg::commit_lock_available = false;
    mg->committing_tx.store(tx);
    tx->state = gtm_thread::STATE_SERIAL | gtm_thread::STATE_IRREVOCABLE
          | gtm_thread::STATE_SOFTWARE;
    tx->shared_state.store( gtm_thread::STATE_SERIAL
              | gtm_thread::STATE_IRREVOCABLE
              | gtm_thread::STATE_SOFTWARE, std::memory_order_release);
    if (unlikely(tx->tx_data.load() == NULL))
    {
      invalbrid_tx_data *spec_data = new invalbrid_tx_data();
      spec_data->write_log = new gtm_log();
      tx->tx_data.store((gtm_transaction_data*)spec_data);
    }
    #ifdef DEBUG_INVALBRID
      tx->tx_types_started[IRREVOC_SW]++;
    #endif
  }

  gtm_restart_reason
  trycommit()
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_mg* mg = (invalbrid_mg*)m_method_group;
    invalbrid_tx_data * tx_data = (invalbrid_tx_data*) tx->tx_data.load();
    invalbrid_mg::invalidate();
    mg->committing_tx.store(0);
    invalbrid_mg::commit_lock_available = true;
    pthread_mutex_unlock(&invalbrid_mg::commit_lock);
    tx->state = 0;
    tx->shared_state.store(0, std::memory_order_release);
    tx_data->clear();
    #ifdef DEBUG_INVALBRID
      tx->tx_types_commited[IRREVOC_SW]++;
    #endif
    return NO_RESTART;
  }

  void
  rollback(gtm_transaction_cp *cp)
  {
    GTM_fatal("Invalbrid-IrrevocSW cannot rollback, because it's serial irrevocable");
  }

}; // sglsw_dispatch

static const irrevocsw_dispatch o_irrevocsw_dispatch;

} // anon

GTM::abi_dispatch *
GTM::dispatch_invalbrid_irrevocsw()
{
  return const_cast<irrevocsw_dispatch *>(&o_irrevocsw_dispatch);
}
