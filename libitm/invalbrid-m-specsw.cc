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
    invalbrid_mg *mg = (invalbrid_mg*)method_group::method_gr;
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load();
    // If commit_sequenze has changed since this transaction started, then a
    // sglsw transaction is or was running. So this transaction has to restart,
    // because there is no conflict detection with sglsw transactions.
    if (spec_data->local_commit_sequence != mg->commit_sequence.load())
      return RESTART_TRY_AGAIN;
    // If there is a transaction which holds the commit_lock, we have to
    // validate the read and write set against its write set.
    gtm_thread *c_tx = mg->committing_tx.load();
    // Take the thread_lock as reader to garantee that the commiting thread
    // won't be destroyed while we're working on it.
    bool r_conflict = false, w_conflict = false;
    tx->thread_lock.reader_lock();
    if (c_tx !=0 && c_tx != tx)
      {
	c_tx->shared_data_lock.reader_lock();
	if(tx->shared_state.load() & gtm_thread::STATE_SOFTWARE)
	{
	  invalbrid_tx_data* c_tx_data = (invalbrid_tx_data*)c_tx->tx_data.load();
	  assert(c_tx_data != NULL);
	  bloomfilter *c_bf = c_tx_data->writeset.load();
	  bloomfilter *r_bf = spec_data->readset.load();
	  bloomfilter *w_bf = spec_data->writeset.load();
	  r_conflict = r_bf->intersects(c_bf); // TODO store the bf in a local object and execute intersection check after lock release. This also would mean only one shared acces to the writeset of the commiting transaction. 
	  w_conflict = w_bf->intersects(c_bf);
	}
	c_tx->shared_data_lock.reader_unlock();
      }
    tx->thread_lock.reader_unlock();
    // Load this threads read- and writeset. If one of them intersects with the
    // writeset of the commiting transaction, then this transaction has to restart.
    if (r_conflict)
      return RESTART_VALIDATE_READ;
    if (w_conflict)
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
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load();
    tx->thread_lock.reader_lock();
    gtm_thread **prev = &(tx->list_of_threads);
    bloomfilter *bf = spec_data->writeset.load();
    for (; *prev; prev = &(*prev)->next_thread)
      {
	if (*prev == tx)
	  continue;
	// Lock prevs shared data for writing. 
	(*prev)->shared_data_lock.writer_lock(); // ?TODO? use reader and upgrade to writer if necessary.
	// Only invalidate software transactions in this case other SpecSWs.
	// IrrevocSWs also have the state software, because they carry read and
	// write sets. But an IrrevocSW can not be active at this point, because
	// invalidation is only done when holding the commit lock.
	if((*prev)->shared_state.load() & gtm_thread::STATE_SOFTWARE)
	{
	  invalbrid_tx_data *prev_data = (invalbrid_tx_data*) (*prev)->tx_data.load();
	  assert(prev_data != NULL);
	  bloomfilter *w_bf = prev_data->writeset.load();
	  bloomfilter *r_bf = prev_data->readset.load();
	  if (bf->intersects(w_bf))
	    {
	      prev_data->invalid_reason.store(RESTART_VALIDATE_WRITE);
	      prev_data->invalid.store(true);
	    }
	  if (bf->intersects(r_bf))
	    {
	      prev_data->invalid_reason.store(RESTART_VALIDATE_READ);
	      prev_data->invalid.store(true);
	    }
	}
	(*prev)->shared_data_lock.writer_unlock();
      }
    tx->thread_lock.reader_unlock();
  }
  
  template <typename V> static V load(const V* addr, ls_modifier mod)
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load();
    // Search in the hash_map for previous writes to this address.
    std::unordered_map<const void*, const gtm_word*>::const_iterator found =
      spec_data->write_hash.find((void*) addr);
    if (found != spec_data->write_hash.end())
      return *(&found->second[2]);
    // If there was no previous write, the addr will be added to the readset.
    bloomfilter *bf = spec_data->readset.load();
    bf->add_address((void*) addr);
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
    invalbrid_tx_data *spec_data = (invalbrid_tx_data*) tx->tx_data.load();
    // If this transaction has been invalidated, it must restart.
    tx->shared_data_lock.reader_lock();
    bool invalid = spec_data->invalid.load();
    gtm_restart_reason rr = spec_data->invalid_reason;
    tx->shared_data_lock.reader_unlock();
    if (invalid)
      method_group::method_gr->restart(rr);
    // Adding the address to the writeset bloomfilter.
    bloomfilter *bf = spec_data->writeset.load();
    bf->add_address((void*)addr);
    // Adding the addr, value pair to the writelog.
    gtm_word *entry = spec_data->write_log->log((void*)addr,(void*)&value, sizeof(V));
    spec_data->log_size = spec_data->write_log->size();
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
    // If this transaction has been restartet as a serial transaction, we have
    // to acquire the commit lock. And set the shared state to serial.
    if (tx->state & gtm_thread::STATE_SERIAL)
      {
	pthread_mutex_lock(&invalbrid_mg::commit_lock);
	tx->shared_data_lock.writer_lock();
	tx->shared_state |= gtm_thread::STATE_SERIAL;
	tx->shared_data_lock.writer_unlock();
      }
    uint32_t local_cs = mg->commit_sequence.load();
    while (local_cs & 1)
      {
	cpu_relax();
	local_cs = mg->commit_sequence.load();
      }
    tx->state |= gtm_thread::STATE_SOFTWARE; 
    // Setting shared_state and possibly tx_data must be protected by 
    // shared_data_lock as writer.
    tx->shared_data_lock.writer_lock();
    tx->shared_state.store(gtm_thread::STATE_SOFTWARE);
    if (unlikely(tx->tx_data.load() == NULL))
      {
	invalbrid_tx_data *spec_data = new invalbrid_tx_data();
	spec_data->write_log = new gtm_log();
	tx->tx_data.store((gtm_transaction_data*)spec_data);
      }
    invalbrid_tx_data* tx_data = (invalbrid_tx_data*) tx->tx_data.load();
    tx_data->local_commit_sequence = local_cs;
    tx->shared_data_lock.writer_unlock();
    #ifdef DEBUG_INVALBRID
      tx->tx_types_started[SPEC_SW]++;
    #endif
  }
  
  gtm_restart_reason 
  trycommit() 
  {
    gtm_thread *tx = gtm_thr();
    invalbrid_mg* mg = (invalbrid_mg*)m_method_group;
    invalbrid_tx_data * spec_data = (invalbrid_tx_data*) tx->tx_data.load();
    // If this is a read only transaction, no commit lock or validation is required.
    bloomfilter *bf = spec_data->writeset.load();
    if (bf->empty() == true)
      {
	// Clear the tx data.
	clear();
	invalbrid_mg::sw_cnt--;
	return NO_RESTART;
      }
    // If this transaction went serial and has already acquired the commit lock,
    // we don't want to take it. This case should be unlikely. The default case
    // should be that the commit lock must be acquired at this point.
    if (likely(!(tx->state & gtm_thread::STATE_SERIAL)))
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
    spec_data->write_log->commit(tx);
    // Invalidate other conflicting specsw transactions.
    invalidate();
    // Restore committing_tx_data.
    mg->committing_tx.store(0);
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
    tx->shared_data_lock.writer_lock();
    tx->tx_data.load()->clear();
    tx->state = 0;
    tx->shared_state.store(0);
    tx->shared_data_lock.writer_unlock();
  }
  
  void
  rollback(gtm_transaction_cp *cp)
  {
    gtm_thread *tx = gtm_thr();
    tx->shared_data_lock.writer_lock();
    if (cp)
      {
	gtm_transaction_data *data = tx->tx_data.load();
	data->load(cp->tx_data);
      }
    else
      {
	// If this transaction has a serial state, then this rollback belongs
	// to an outer abort of an serial mode software transaction, so we have
	// to release the commit lock.
	if (tx->state & gtm_thread::STATE_SERIAL)
	  pthread_mutex_unlock(&invalbrid_mg::commit_lock);
	tx->tx_data.load()->clear();
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
