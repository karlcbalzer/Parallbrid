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

class specsw_dispatch : public abi_dispatch
{
public:
  specsw_dispatch(): abi_dispatch(method_group_invalbrid(), false, true) { }

protected:
  static gtm_restart_reason
  validate()
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_mg *mg = (invalbrid_mg*)method_group::method_gr;
    // We can load tx_data in relaxed mode, because the reference never changes.
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
    // If commit_sequenze has changed since this transaction started, then a
    // sglsw transaction is or was running. So this transaction has to restart,
    // because there is no conflict detection with sglsw transactions.
    if (spec_data->local_commit_sequence != mg->commit_sequence.load(std::memory_order_acquire))
      return RESTART_TRY_AGAIN;
    // If there is a transaction which holds the commit_lock, we have to
    // validate the read and write set against its write set.
    // Take the thread_lock as reader to garantee that the commiting thread, if
    // one exists, won't be destroyed while we're working on it.
    tx->thread_lock.reader_lock();
    gtm_thread *c_tx = mg->committing_tx.load(std::memory_order_acquire);
    bool r_conflict = false, w_conflict = false;
    if (c_tx !=0 && c_tx != tx)
    {
      // We do not have to check the state of the committing tx, since only
      // transactions with a read- and writeset set committing_tx. If the
      // transaction pointed to by committing_tx has moved on and has no
      // longer the software state, then there is no problem either, since it
      // still carrys an read and writeset, even if its not used.
      invalbrid_tx_data* c_tx_data = (invalbrid_tx_data*)c_tx->tx_data.load(std::memory_order_acquire);
      // Load the writeset of the committing transaction.
      bloomfilter *c_bf = c_tx_data->writeset.load(std::memory_order_acquire);
      // Load this threads read- and writeset.
      bloomfilter *r_bf = spec_data->readset.load(std::memory_order_relaxed);
      bloomfilter *w_bf = spec_data->writeset.load(std::memory_order_relaxed);
      // Intersect this transactions read- and writeset with the writeset of the
      // committing transaction.
      r_conflict = r_bf->intersects(c_bf);
      w_conflict = w_bf->intersects(c_bf);
    }
    tx->thread_lock.reader_unlock();
    // If this transactions read- or writeset intersect with the writeset of
    // the committing transaction, then this transaction has to be restarted.
    if (r_conflict)
      return RESTART_VALIDATE_READ;
    if (w_conflict)
      return RESTART_VALIDATE_WRITE;
#ifdef USE_HTM_FASTPATH
    // Wait while hardware transactions are in their post commit phase.
    uint32_t hw_pcc;
    bool finished = false;
    while (!finished) {
      uint32_t htm_return = htm_begin();
      if (htm_begin_success(htm_return)) {
        hw_pcc = invalbrid_mg::hw_post_commit;
        htm_commit();
        if (hw_pcc == 0){
          finished = true;
        }
        else {
          cpu_relax();
        }
      }
    }
#endif
    // If this transaction has been invalidated, it has to be restartet. This is
    // handled by the caller.
    gtm_restart_reason rr = spec_data->invalid_reason.load(memory_order_acquire);
    return rr;
  }

  static void
  pre_write(void *dst, size_t size)
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
    // If this transaction has been invalidated, it has to be restarted.
    gtm_restart_reason rr = spec_data->invalid_reason.load(memory_order_acquire);
    if (rr != NO_RESTART)
      {
    method_group::method_gr->restart(rr);
      }
    // Adding the address to the writeset bloomfilter.
    bloomfilter *bf = spec_data->writeset.load(std::memory_order_relaxed);
    bf->add_address(dst, size);
  }

  template <typename V> static V load(const V* addr, ls_modifier mod)
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
    // The addr will be added to the readset.
    bloomfilter *bf = spec_data->readset.load(std::memory_order_relaxed);
    bf->add_address((void*) addr, sizeof(V));
    V val = *addr;
    spec_data->write_log->load_value((void*)&val,(void*)addr, sizeof(V));
    // Before the value can be returned, we have to do validation for opacity.
    // But only if this transaction hasn't been upgraded to serial status.
    if (!(tx->state & gtm_thread::STATE_SERIAL))
      {
    gtm_restart_reason rr = validate();
    if (rr != NO_RESTART)
      method_group::method_gr->restart(rr);
      }
    return val;
  }

  template <typename V> static void store(V* addr, const V value,
      ls_modifier mod)
  {
    pre_write(addr, sizeof(V));
    gtm_thread *tx = gtm_thr();
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
    // Adding the addr, value pair to the writelog.
    spec_data->write_log->log((void*)addr,(void*)&value, sizeof(V));
    spec_data->log_size = spec_data->write_log->size();
  }

public:
  static void memtransfer_static(void *dst, const void* src, size_t size,
      bool may_overlap, ls_modifier dst_mod, ls_modifier src_mod)
  {
    // read phase
    gtm_thread *tx = gtm_thr();
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
    // The src addresses will be added to the readset.
    bloomfilter *bf = spec_data->readset.load(std::memory_order_relaxed);
    bf->add_address(src, size);
    void *tmp = xmalloc (size, true);
    ::memcpy(tmp, src, size);
    spec_data->write_log->load_value(tmp,src, size);
    // Before the value can be added to the write log, we have to do validation
    // for opacity. But only if this is not an serial transaction.
    if (!(tx->state & gtm_thread::STATE_SERIAL))
    {
      gtm_restart_reason rr = validate();
      if (rr != NO_RESTART)
        method_group::method_gr->restart(rr);
    }
    // write phase
    bf = spec_data->writeset.load(std::memory_order_relaxed);
    bf->add_address(dst, size);
    // Adding the addr, value pair to the writelog.
    spec_data->write_log->log(dst,tmp, size);
    spec_data->log_size = spec_data->write_log->size();
  }

  static void memset_static(void *dst, int c, size_t size, ls_modifier mod)
  {
    pre_write(dst, size);
    gtm_thread *tx = gtm_thr();
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
    // Adding the addr, value pair to the writelog.
    spec_data->write_log->log_memset(dst,c, size);
    spec_data->log_size = spec_data->write_log->size();
  }

  CREATE_DISPATCH_METHODS(virtual, )
  CREATE_DISPATCH_METHODS_MEM()

  void
  begin()
  {
    invalbrid_mg::sw_cnt.fetch_add(1, std::memory_order_release);
    gtm_thread *tx = gtm_thr();
    invalbrid_mg* mg = (invalbrid_mg*)m_method_group;
    uint32_t local_cs = mg->commit_sequence.load(std::memory_order_acquire);
    while (local_cs & 1)
    {
        cpu_relax();
        local_cs = mg->commit_sequence.load(std::memory_order_acquire);
    }
    tx->state |= gtm_thread::STATE_SOFTWARE;
    if (unlikely(tx->tx_data.load(std::memory_order_relaxed) == NULL))
    {
        invalbrid_tx_data *spec_data = new invalbrid_tx_data();
        spec_data->write_log = new gtm_log();
        tx->tx_data.store((gtm_transaction_data*)spec_data, std::memory_order_release);
    }
    // Set the shared state to STATE_SOFTWARE, so invalidating transactions
    // know that this is a transaction that carrys a read and write set. We do
    // not need to take the shared_data_lock, since shared_state is set
    // atomically and the memory order garantees that tx_data is published
    // before shared_state is.
    tx->shared_state.fetch_or(gtm_thread::STATE_SOFTWARE, std::memory_order_release);
    invalbrid_tx_data* tx_data = (invalbrid_tx_data*) tx->tx_data.load();
    // Save the commit_sequence so we can restart if a sglsw transaction was
    // starteted.
    tx_data->local_commit_sequence = local_cs;
    #ifdef DEBUG_INVALBRID
      tx->tx_types_started[SPEC_SW]++;
    #endif
  }

  gtm_restart_reason
  trycommit()
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_mg* mg = (invalbrid_mg*)m_method_group;
    invalbrid_tx_data * spec_data = (invalbrid_tx_data*) tx->tx_data.load(std::memory_order_relaxed);
    // If this is a read only transaction, no commit lock or validation is required.
    bloomfilter *bf = spec_data->writeset.load(std::memory_order_relaxed);
    if (bf->empty() == true)
    {
        // The writeset is empty, so we can commit without synchronization. If we hold
        // the commit lock, release it.
        if (tx->state & gtm_thread::STATE_SERIAL)
        {
          invalbrid_mg::commit_lock_available = true;
          pthread_mutex_unlock(&invalbrid_mg::commit_lock);
        }
        // Clear the tx data.
        clear();
        invalbrid_mg::sw_cnt.fetch_sub(1,std::memory_order_release);
        return NO_RESTART;
    }
    // If this transaction went serial and has already acquired the commit lock,
    // we don't want to take it. This case should be unlikely. The default case
    // should be that the commit lock must be acquired at this point.
    if (likely(!(tx->state & gtm_thread::STATE_SERIAL)))
    {
      pthread_mutex_lock(&invalbrid_mg::commit_lock);
      invalbrid_mg::commit_lock_available = false;
    }
    mg->committing_tx.store(tx, std::memory_order_release);
    gtm_restart_reason rr = validate();
    // If validation failes, restart.
    if (rr != NO_RESTART)
    {
      mg->committing_tx.store(0, std::memory_order_release);
      invalbrid_mg::commit_lock_available = true;
      pthread_mutex_unlock(&invalbrid_mg::commit_lock);
      return rr;
    }
    invalbrid_mg::sw_cnt.fetch_sub(1,std::memory_order_release);
    // Commit all speculative writes to memory.
    spec_data->write_log->commit(tx);
    // Invalidate other conflicting specsw transactions.
    invalbrid_mg::invalidate();
    // Restore committing_tx_data.
    mg->committing_tx.store(0, std::memory_order_release);
    invalbrid_mg::commit_lock_available = true;
    pthread_mutex_unlock(&invalbrid_mg::commit_lock);
    // Clear the tx data.
    clear();
    #ifdef DEBUG_INVALBRID
      tx->tx_types_commited[SPEC_SW]++;
    #endif
    return NO_RESTART;
  }

  // Clear the tx data.
  void
  clear()
  {
    gtm_thread *tx = gtm_thr();
    tx->shared_state.store(0, std::memory_order_release);
    tx->tx_data.load(std::memory_order_relaxed)->clear();
    tx->state = 0;
  }

  void
  rollback(gtm_transaction_cp *cp)
  {
    gtm_thread *tx = gtm_thr();
    if (cp)
    {
        gtm_transaction_data *data = tx->tx_data.load(std::memory_order_relaxed);
        data->load(cp->tx_data);
    }
    else
    {
        // If this transaction has a serial state, then this rollback belongs
        // to an outer abort of an serial mode software transaction, so we have
        // to release the commit lock.
        if (tx->state & gtm_thread::STATE_SERIAL)
        {
        invalbrid_mg::commit_lock_available = true;
          pthread_mutex_unlock(&invalbrid_mg::commit_lock);
        }
        invalbrid_mg::sw_cnt.fetch_sub(1, std::memory_order_release);
        clear();
    }
  }

}; // specsw_dispatch

static const specsw_dispatch o_specsw_dispatch;

} // anon

GTM::abi_dispatch *
GTM::dispatch_invalbrid_specsw()
{
  return const_cast<specsw_dispatch *>(&o_specsw_dispatch);
}
