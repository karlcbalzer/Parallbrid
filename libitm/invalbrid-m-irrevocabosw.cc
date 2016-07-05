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

using namespace GTM;

namespace {

class irrevocabosw_dispatch : public abi_dispatch
{
public:
  irrevocabosw_dispatch(): abi_dispatch(method_group_invalbrid(), false, false) { }

protected:
  template <typename V> static V load(const V* addr, ls_modifier mod)
  {
    return *addr;
  }

  template <typename V> static void store(V* addr, const V value, ls_modifier mod)
  {
    gtm_thread *tx = gtm_thr();
    // We can load tx_data in relaxed mode, because the reference never changes.
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
    // The addresses will be added to the writeset.
    bloomfilter *bf = spec_data->writeset.load(std::memory_order_relaxed);
    bf->add_address((void*) addr, sizeof(V));
    // Adding the addr, previous value pair to the writelog.
    spec_data->undo_log->log((void*)addr, sizeof(V));
    spec_data->log_size = spec_data->undo_log->size();
    *addr = value;
  }

public:
  static void memtransfer_static(void *dst, const void* src, size_t size,
                                 bool may_overlap, ls_modifier dst_mod, ls_modifier src_mod)
  {
    gtm_thread *tx = gtm_thr();
    // We can load tx_data in relaxed mode, because the reference never changes.
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
    // The addresses will be added to the writeset.
    bloomfilter *bf = spec_data->writeset.load(std::memory_order_relaxed);
    bf->add_address(dst, size);
    // Adding the addr, previous value pair to the writelog.
    spec_data->undo_log->log(dst, size);
    spec_data->log_size = spec_data->undo_log->size();
    if (!may_overlap)
      ::memcpy(dst, src, size);
    else
      ::memmove(dst, src, size);
  }

  static void memset_static(void *dst, int c, size_t size, ls_modifier mod)
  {
    gtm_thread *tx = gtm_thr();
    // We can load tx_data in relaxed mode, because the reference never changes.
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
    // The addresses will be added to the writeset.
    bloomfilter *bf = spec_data->writeset.load(std::memory_order_relaxed);
    bf->add_address(dst, size);
    // Adding the addr, previous value pair to the writelog.
    spec_data->undo_log->log(dst, size);
    spec_data->log_size = spec_data->undo_log->size();
    ::memset(dst, c, size);
  }

  CREATE_DISPATCH_METHODS(virtual, )
  CREATE_DISPATCH_METHODS_MEM()

  void
  begin()
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_mg* mg = (invalbrid_mg*)m_method_group;
    if (unlikely(tx->tx_data.load(std::memory_order_relaxed) == NULL))
    {
      invalbrid_tx_data *spec_data = new invalbrid_tx_data();
      spec_data->undo_log = new gtm_undolog();
      tx->tx_data.store((gtm_transaction_data*)spec_data, std::memory_order_release);
    }
    else
    {
      invalbrid_tx_data *data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
      if (data->undo_log == NULL)
      {
        data->undo_log = new gtm_undolog();
      }
    }
    // Acquire the commit lock.
    pthread_mutex_lock(&invalbrid_mg::commit_lock);
    invalbrid_mg::commit_lock_available = false;
    mg->committing_tx.store(tx, std::memory_order_release);
    tx->state = gtm_thread::STATE_SERIAL | gtm_thread::STATE_SOFTWARE;
    tx->shared_state.store( gtm_thread::STATE_SERIAL
              | gtm_thread::STATE_SOFTWARE, std::memory_order_release);
    #ifdef DEBUG_INVALBRID
      tx->tx_types_started[IRREVOCABO_SW]++;
    #endif
  }

  gtm_restart_reason
  trycommit()
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_mg* mg = (invalbrid_mg*)m_method_group;
    invalbrid_tx_data * tx_data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
    invalbrid_mg::invalidate();
    mg->committing_tx.store(0, std::memory_order_release);
    invalbrid_mg::commit_lock_available = true;
    pthread_mutex_unlock(&invalbrid_mg::commit_lock);
    tx->state = 0;
    tx->shared_state.store(0, std::memory_order_release);
    tx_data->clear();
    #ifdef DEBUG_INVALBRID
      tx->tx_types_commited[IRREVOCABO_SW]++;
    #endif
    return NO_RESTART;
  }

  void
  rollback(gtm_transaction_cp *cp)
  {
    gtm_thread *tx = gtm_thr();
    if (cp)
    {
      gtm_transaction_data *data = tx->tx_data.load(std::memory_order_relaxed);
      // Now the checkpoint gets loaded to restore the tx data to the state before
      // this nested transaction.
      data->load(cp->tx_data);
      // Rollback the undolog.
      invalbrid_tx_data *inval_data = (invalbrid_tx_data*) data;
      inval_data->undo_log->rollback(tx, inval_data->log_size);
    }
    else
    {
      // This rollback belongs to an outer abort of an serial mode software transaction, so we have
      // to restore the previous memory state by unrolling the undolog and publish the previous data. After that we release the commit lock.
      gtm_thread *tx = gtm_thr();
      invalbrid_tx_data *data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
      data->undo_log->rollback(tx, 0);
      invalbrid_mg::commit_lock_available = true;
      pthread_mutex_unlock(&invalbrid_mg::commit_lock);
      tx->shared_state.store(0, std::memory_order_release);
      tx->tx_data.load(std::memory_order_relaxed)->clear();
      tx->state = 0;
    }
  }

}; // irrevocabosw_dispatch

static const irrevocabosw_dispatch o_irrevocabosw_dispatch;

} // anon

GTM::abi_dispatch *
GTM::dispatch_invalbrid_irrevocabosw()
{
  return const_cast<irrevocabosw_dispatch *>(&o_irrevocabosw_dispatch);
}
