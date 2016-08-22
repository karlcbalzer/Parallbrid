
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

class bfhw_dispatch : public abi_dispatch
{
public:
  bfhw_dispatch(): abi_dispatch(method_group_invalbrid(), false, true) { }

protected:
  template <typename V> static V load(const V* addr, ls_modifier mod)
  {
    return *addr;
  }
  template <typename V> static void store(V* addr, const V value,
      ls_modifier mod)
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_hw_tx_data * tx_data = (invalbrid_hw_tx_data*) tx->hw_tx_data;
    tx_data->writeset->add_address((void*) addr, sizeof(V));
    *addr = value;
  }

public:
  static void memtransfer_static(void *dst, const void* src, size_t size,
      bool may_overlap, ls_modifier dst_mod, ls_modifier src_mod)
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_hw_tx_data * tx_data = (invalbrid_hw_tx_data*) tx->hw_tx_data;
    tx_data->writeset->add_address(dst, size);
    if (!may_overlap)
      ::memcpy(dst, src, size);
    else
      ::memmove(dst, src, size);
  }

  static void memset_static(void *dst, int c, size_t size, ls_modifier mod)
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_hw_tx_data * tx_data = (invalbrid_hw_tx_data*) tx->hw_tx_data;
    tx_data->writeset->add_address(dst, size);
    ::memset(dst, c, size);
  }

  CREATE_DISPATCH_METHODS(virtual, )
  CREATE_DISPATCH_METHODS_MEM()

  void
  begin()
  {
    gtm_thread *tx = gtm_thr();
    tx->state = gtm_thread::STATE_HARDWARE;
    if (tx->hw_tx_data == 0)
    {
      tx->hw_tx_data = (gtm_transaction_data*) new invalbrid_hw_tx_data();
    }
  }

  gtm_restart_reason
  trycommit()
  {
    invalbrid_mg::hw_post_commit++;
    htm_commit();

    // The htm execution has ended successfully. Now the post commit phase starts.
    // Invalidate software transactions
    gtm_thread *tx = gtm_thr();
    tx->thread_lock.reader_lock();
    gtm_thread **prev = &(tx->list_of_threads);
    invalbrid_hw_tx_data * tx_data = (invalbrid_hw_tx_data*) tx->hw_tx_data;
    hw_bloomfilter *bf = tx_data->writeset;
    for (; *prev; prev = &(*prev)->next_thread)
    {
      if (*prev == tx)
        continue;
      // Invalidate other software transactions in this case other SpecSWs.
      // IrrevocSWs also have the state software, because they carry a
      // writeset, but an IrrevocSW can not be active at this point, because
      // invalidation is only done when holding the commit lock.
      if((*prev)->shared_state.load(std::memory_order_acquire) & gtm_thread::STATE_SOFTWARE)
      {
        invalbrid_tx_data *prev_data = (invalbrid_tx_data*) (*prev)->tx_data.load(std::memory_order_acquire);
        assert(prev_data != NULL);
//        bloomfilter *w_bf = prev_data->writeset.load(std::memory_order_acquire);
        bloomfilter *r_bf = prev_data->readset.load(std::memory_order_acquire);
//        if (bf->intersects(w_bf))
//        {
//          prev_data->invalid_reason.store(RESTART_LOCKED_WRITE, memory_order_release);
//        }
        if (bf->intersects(r_bf))
        {
          prev_data->invalid_reason.store(RESTART_LOCKED_READ, memory_order_release);
        }
      }
    }
    tx->thread_lock.reader_unlock();
    // Finish post commit phase
    bool finished = false;
    while (!finished) {
      uint32_t htm_return = htm_begin();
      if (htm_begin_success(htm_return)) {
        invalbrid_mg::hw_post_commit--;
        htm_commit();
        finished = true;
      }
    }
    bf->clear();
    tx->nesting = 0;
    tx->state = 0;

    #ifdef DEBUG_INVALBRID
      tx->tx_types_commited[BFHW]++;
    #endif
    return NO_RESTART;
  }

  void
  rollback(gtm_transaction_cp *cp)
  {
    GTM_fatal("Hardware transactions don't need to rollback because of their isolation properties");
  }

}; // bfhw_dispatch

static const bfhw_dispatch o_bfhw_dispatch;

} // anon

GTM::abi_dispatch *
GTM::dispatch_invalbrid_bfhw()
{
  return const_cast<bfhw_dispatch *>(&o_bfhw_dispatch);
}

