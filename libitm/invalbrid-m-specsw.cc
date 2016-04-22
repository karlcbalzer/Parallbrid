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
  
class specsw_dispatch : public abi_dispatch
{
public:
  specsw_dispatch(): abi_dispatch(method_group_invalbrid(), false, true) { }

protected:
  static gtm_restart_reason 
  validate() 
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_mg* mg = (invalbrid_mg*)method_group::method_gr;
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data;
    // If commit_sequenze has changed since this transaction started, then a
    // sglsw transaction is or was running. So this transaction has to restart,
    // because there is no conflict detection with sglsw transactions.
    if (spec_data->local_commit_sequence != mg->commit_sequence.load())
      return RESTART_TRY_AGAIN;
    // If there is a transaction which holds the commit_lock, we have to
    // validate the read and write set against its write set.
    gtm_thread* c_tx = mg->committing_tx.load();
    // Take the thread_lock as reader to garantee that the thread won't be
    // destroyed will we're working on it.
    tx->thread_lock.reader_lock();
    bool rconflict = false, wconflict = false;
    if (c_tx !=0 && c_tx != tx)
      {
	c_tx->shared_data_lock.reader_lock();
	if(tx->shared_state.load() != gtm_thread::STATE_HARDWARE)
	{
	  invalbrid_tx_data* c_tx_data = (invalbrid_tx_data*)tx->tx_data;
	  rconflict = spec_data->readset.intersects(&c_tx_data->writeset);
	  wconflict = spec_data->writeset.intersects(&c_tx_data->writeset);
	}
	c_tx->shared_data_lock.reader_unlock();
      }
    tx->thread_lock.reader_unlock();
    if (rconflict)
      return RESTART_VALIDATE_READ;
    if (wconflict)
      return RESTART_VALIDATE_WRITE;
    // Wait while hardware transactions are in their post commit phase.
    while (mg->hw_post_commit != 0) { cpu_relax(); }
    // If the INVALID flag is set, this transaction has to be restartet.
    tx->shared_data_lock.reader_lock();
    bool invalid = spec_data->invalid.load();
    gtm_restart_reason reason = spec_data->invalid_reason.load();
    tx->shared_data_lock.reader_unlock();
    if (invalid == true) 
      return reason;
    
    return NO_RESTART;    
  }
  
  static void 
  invalidate()
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data;
    tx->thread_lock.reader_lock();
    gtm_thread **prev = &(tx->list_of_threads);
    for (; *prev; prev = &(*prev)->next_thread)
      {
	if (*prev == tx)
	  continue;
	(*prev)->shared_data_lock.writer_lock(); // ?TODO? use reader and upgrade to writer if necessary.
	if(tx->shared_state.load() != gtm_thread::STATE_HARDWARE)
	{
	  invalbrid_tx_data* prev_data = (invalbrid_tx_data*) (*prev)->tx_data;
	  if (spec_data->writeset.intersects(&prev_data->writeset))
	    {
	      prev_data->invalid_reason = RESTART_VALIDATE_WRITE;
	      prev_data->invalid = true;
	    }
	  if (spec_data->writeset.intersects(&prev_data->readset))
	    {
	      prev_data->invalid_reason = RESTART_VALIDATE_READ;
	      prev_data->invalid = true;
	    }
	}
	(*prev)->shared_data_lock.writer_lock();
      }
    tx->thread_lock.reader_unlock();
  }
  
  template <typename V> static V load(const V* addr, ls_modifier mod)
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_tx_data * spec_data = (invalbrid_tx_data*) tx->tx_data;
    // Search in the hash_map for previous writes to this address.
    std::unordered_map<const void*, const gtm_word*>::const_iterator found =
      spec_data->write_hash.find((void*) addr);
    if (found != spec_data->write_hash.end())
      return *(found->second);
    // If there was no previous write, the addr will be added to the readset.
    spec_data->readset.add_address((void*) addr);
    V val = *addr;
    // Before the value can be returned, we have to do validation for opacity.
    gtm_restart_reason rr = validate();
    if (rr != NO_RESTART)
      method_group::method_gr->restart(rr);
    return val;
  }
  
  template <typename V> static void store(V* addr, const V value,
      ls_modifier mod)
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_tx_data * spec_data = (invalbrid_tx_data*) tx->tx_data;
    // If this transaction has been invalidated, it must restart.
    tx->shared_data_lock.reader_lock();
    bool invalid = spec_data->invalid.load();
    gtm_restart_reason rr = spec_data->invalid_reason;
    tx->shared_data_lock.reader_unlock();
    if (invalid)
      method_group::method_gr->restart(rr);
    // Adding the address to the writeset bloomfilter.
    spec_data->writeset.add_address((void*)addr);
    // Adding the addr, value pair to the writelog.
    gtm_word *entry = spec_data->write_log->log((void*)&value, sizeof(V));
    // Also adding the address and a pointer to the writelog to a hashmap for
    // quick access on a load.
    spec_data->write_hash.insert(
      std::make_pair<const void *, const gtm_word *>((void*)addr, entry));
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
  
  void 
  begin() 
  { 
    invalbrid_mg::sw_cnt++; 
    gtm_thread *tx = gtm_thr();
    invalbrid_mg* mg = (invalbrid_mg*)m_method_group;
    uint32_t local_cs = mg->commit_sequence.load();
    while (local_cs & 1)
      {
	cpu_relax();
	local_cs = mg->commit_sequence.load();
      }
    tx->state = gtm_thread::STATE_SOFTWARE; 
    // Setting shared_state and possibly tx_data must be protected by 
    // shared_data_lock as writer.
    tx->shared_data_lock.writer_lock();
    tx->shared_state.store(gtm_thread::STATE_SOFTWARE);
    if (unlikely(tx->tx_data == 0))
      {
	invalbrid_tx_data *spec_data = new invalbrid_tx_data();
	spec_data->write_log = new gtm_log();
	spec_data->local_commit_sequence = local_cs;
	tx->tx_data = (gtm_transaction_data*)spec_data;
      }
    tx->shared_data_lock.writer_unlock();
  }
  
  gtm_restart_reason 
  trycommit() 
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_mg* mg = (invalbrid_mg*)m_method_group;
    invalbrid_tx_data * spec_data = (invalbrid_tx_data*) tx->tx_data;
    // If this is a read only transaction, no commit lock or validation is required.
    if (spec_data->writeset.empty() == true)
      {
	invalbrid_mg::sw_cnt--;
	return NO_RESTART;
      }
    pthread_mutex_lock(&invalbrid_mg::commit_lock);
    mg->committing_tx.store(tx);
    gtm_restart_reason rr = validate();
    if (rr != NO_RESTART)
      {
	pthread_mutex_unlock(&invalbrid_mg::commit_lock);
	return rr;
      }
    invalbrid_mg::sw_cnt--;
    // Commit all speculative writes to memory.
    spec_data->write_log->commit(tx,0);
    // Invalidate other conflicting specsw transactions.
    invalidate();
    // Restore committing_tx_data.
    mg->committing_tx.store(0);
    pthread_mutex_unlock(&invalbrid_mg::commit_lock);
    // Clear the tx data.
    tx->shared_data_lock.writer_lock();
    spec_data->clear();
    tx->state = 0;
    tx->shared_state.store(0);
    tx->shared_data_lock.writer_unlock();
    return NO_RESTART;
  }
  
  void
  rollback(gtm_transaction_cp *cp)
  {
    gtm_thread *tx = gtm_thr();
    tx->shared_data_lock.writer_lock();
    if (cp)
      tx->tx_data->load(cp->tx_data);
    else
      {
	tx->tx_data->clear();
	tx->state = 0;
	tx->shared_state.store(0);
      }
    tx->shared_data_lock.writer_unlock();
  }
  
}; // specsw_dispatch

static const specsw_dispatch o_specsw_dispatch;

} // anon

GTM::abi_dispatch *
GTM::dispatch_invalbrid_specsw()
{
  return const_cast<specsw_dispatch *>(&o_specsw_dispatch);
}
