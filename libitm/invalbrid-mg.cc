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


// Invalbrids static initialization.
pthread_mutex_t invalbrid_mg::commit_lock
    __attribute__((aligned(HW_CACHELINE_SIZE)));
atomic<uint32_t> invalbrid_mg::sw_cnt;

invalbrid_mg::invalbrid_mg()
{
  pthread_mutex_init(&commit_lock, NULL);
  sw_cnt.store(0);
  commit_sequence.store(0);
  hw_post_commit = 0;
  committing_tx.store(0);
}

// Decides as which type of transaction this transaction should run, starts
// the transaction and returns the appropriate _ITM_actions code.
uint32_t 
invalbrid_mg::begin(uint32_t prop, const gtm_jmpbuf *jb) 
{
  gtm_thread *tx = gtm_thr();
  uint32_t ret = 0;
  
  // ??? pr_undoLogCode is not properly defined in the ABI. Are barriers
  // omitted because they are not necessary (e.g., a transaction on thread-
  // local data) or because the compiler thinks that some kind of global
  // synchronization might perform better?
  if (unlikely(prop & pr_undoLogCode))
    GTM_fatal("pr_undoLogCode not supported");
  
  if (tx == NULL)
    {
      tx = new gtm_thread();
      set_gtm_thr(tx);
    }
  if(tx->nesting > 0)
    {
      // This is a nested transaction.
      if (prop & pr_hasNoAbort)
	{
	  // If there is no instrumented codepath and disp can not run 
	  // uninstrumented, we need to restart with a dispatch that can run 
	  // uninstrumented or in the case of an irrevocable transaction upgrade
	  // to an irrevocable transaction, that can run uninstrumented.
	  if (!(prop & pr_instrumentedCode)) 
	    if (!abi_disp()->can_run_uninstrumented_code())
	      {
		if (!(tx->state & gtm_thread::STATE_IRREVOCABLE))
		  restart(RESTART_UNINSTRUMENTED_CODEPATH);
		else
		  {
		    // Set the dispatch to sglsw, because this is the only
		    // irrevocable transaction type, that can run uninstrumented.
		    set_abi_disp(dispatch_invalbrid_sglsw());
		    // Let the dispatch setup the transaction.
		    abi_disp()->begin();
		    // Since we are now running uninstrumented we must clear the
		    // transactional data because the uninstrumented commit won't.
		    // Remove the Software flag, which is set, whenever a
		    // transaction uses the transactional_data.
		    tx->shared_state &= ~gtm_thread::STATE_SOFTWARE;
		    // Clear the transaction data.
		    gtm_transaction_data *data = tx->tx_data.load();
		    if (data != NULL)
		      data->clear();
		    tx->state &= ~gtm_thread::STATE_SOFTWARE;
		  }
		}
	  // If this nested transaction goes irrevocable, then restart.
	  if ((prop & pr_doesGoIrrevocable) && !(tx->state & gtm_thread::STATE_IRREVOCABLE))
	    restart(RESTART_SERIAL_IRR);
	  // This is a nested transaction that will be flattend.
	  tx->nesting++;
	  return ((prop & pr_uninstrumentedCode) && 
		  (abi_disp()->can_run_uninstrumented_code())) ? 
	    a_runUninstrumentedCode : a_runInstrumentedCode;
	}
      else
	{
	  // This is a closed nested transaction.
	  assert(prop & pr_instrumentedCode);
	  gtm_transaction_cp *cp = tx->parent_txns.push();
	  cp->save(tx);
	  new (&tx->alloc_actions) aa_tree<uintptr_t, gtm_alloc_action>();
	}
    }
  else
    {
      if (prop & pr_hasNoAbort)
      // If no abort is present, any of the Invalbrid transactions can be choosen. 
	{
	  if (prop & pr_doesGoIrrevocable)
	  // Only irrevocsw or sglsw can be used at this point. 
	    {
	      uint32_t software_count = sw_cnt.load();
	      if (((prop & pr_instrumentedCode) || (prop & pr_readOnly)) 
		&& (software_count != 0))
	      // Use irrevocsw if software transactions(specsws) are present and
	      // there is an instrumented codepath or this transaction is read only.
		set_abi_disp(dispatch_invalbrid_irrevocsw());
	      else
	      // Use sglsw instead.
		set_abi_disp(dispatch_invalbrid_sglsw());
	    }
	  else
	    {
	      /*#ifdef HAVE_AS_RTM
		if ((prop & pr_instrumentedCode) && (sw_cnt.load() != 0))
		  set_abi_disp(dispatch_invalbrid_specsw()); //TODO BFHW
		else
		  set_abi_disp(dispatch_invalbrid_sglsw()); // TODO LITEHW
	      #else*/
		if (prop & pr_instrumentedCode)
		  set_abi_disp(dispatch_invalbrid_specsw());
		else
		  set_abi_disp(dispatch_invalbrid_sglsw());
	      //#endif
	    }
	}
      else
	{
	  assert(prop & pr_instrumentedCode);
	  set_abi_disp(dispatch_invalbrid_specsw());
	}
    }
  
  // Initialization that is common for outermost and closed nested transactions.
  tx->prop = prop;
  tx->jb = *jb;
  tx->nesting++;
  // As long as we have not exhausted a previously allocated block of TIDs,
  // we can avoid an atomic operation on a shared cacheline.
  if (tx->local_tid & (tid_block_size - 1))
    tx->id = tx->local_tid++;
  else
    {
      #ifdef HAVE_64BIT_SYNC_BUILTINS
      // We don't really care which block of TIDs we get but only that we
      // acquire one atomically; therefore, relaxed memory order is
      // sufficient.
      tx->id = global_tid.fetch_add(tid_block_size, memory_order_relaxed);
      tx->local_tid = tx->id + 1;
      #else
      pthread_mutex_lock (&global_tid_lock);
      global_tid += tid_block_size;
      tx->id = global_tid;
      tx->local_tid = tx->id + 1;
      pthread_mutex_unlock (&global_tid_lock);
      #endif
    }
  
  // Call disp->begin if this is the outer transaction. Nested transactions 
  // shouldn't call the begin routine.  
  if (tx->nesting == 1) 
    {
      abi_disp()->begin();
    }
  
  if (abi_disp()->can_restart()) ret |= a_saveLiveVariables;
  ret |= ((prop & pr_uninstrumentedCode) && 
	  (abi_disp()->can_run_uninstrumented_code())) ? 
    a_runUninstrumentedCode : a_runInstrumentedCode;
  return ret;
}

void 
invalbrid_mg::abort(_ITM_abortReason reason) 
{
  gtm_thread *tx = gtm_thr();

  assert (reason == userAbort || reason == (userAbort | outerAbort));
  assert ((tx->prop & pr_hasNoAbort) == 0);

  if (tx->state & gtm_thread::STATE_IRREVOCABLE)
    GTM_fatal("Irrevocable transactions can not abort");

  // Roll back to innermost transaction.
  if (tx->parent_txns.size() > 0 && !(reason & outerAbort))
    {
      // The innermost transaction is a closed nested transaction.
      gtm_transaction_cp *cp = tx->parent_txns.pop();
      uint32_t longjmp_prop = tx->prop;
      gtm_jmpbuf longjmp_jb = tx->jb;
      tx->rollback (cp, true);
      // Jump to nested transaction (use the saved jump buffer).
      GTM_longjmp (a_abortTransaction | a_restoreLiveVariables,
		   &longjmp_jb, longjmp_prop);
    }
  else
    {
      // There is no nested transaction or an abort of the outermost
      // transaction was requested, so roll back to the outermost transaction.
      tx->rollback (0, true);
      tx->undolog.rollback(tx);
      
      GTM_longjmp (a_abortTransaction | a_restoreLiveVariables,
		   &tx->jb, tx->prop);
    }
}

void 
invalbrid_mg::commit() 
{
  commit_EH(0);
}

void 
invalbrid_mg::commit_EH(void *exc_ptr)
{
  gtm_thread *tx = gtm_thr();
  tx->nesting--;
  if (tx->parent_txns.size() > 0)
    {
      gtm_transaction_cp *cp = tx->parent_txns.pop();
      tx->commit_allocations(false, &cp->alloc_actions);
      cp->commit(tx);
    }
  if (tx->nesting == 0)
    {
      gtm_restart_reason rr;
      rr = abi_disp()->trycommit();
      if (rr != NO_RESTART)
	{
	  if (exc_ptr != NULL)
	    tx->eh_in_flight = exc_ptr;
	  restart(rr);
	}
      tx->restart_total = 0;
      tx->cxa_catch_count = 0;
      tx->undolog.commit();
      tx->commit_user_actions();
      tx->commit_allocations (false,0);
    }
}

_ITM_howExecuting 
invalbrid_mg::in_transaction()
{
  gtm_thread *tx = gtm_thr();
  if (tx == NULL || tx->state == 0) 
    return outsideTransaction;
  else
    return (tx->state & gtm_thread::STATE_IRREVOCABLE) ? 
      inIrrevocableTransaction : inRetryableTransaction;
}

_ITM_transactionId_t 
invalbrid_mg::get_transaction_id()
{
  gtm_thread *tx = gtm_thr();
  return (tx == NULL || tx->state == 0) ? _ITM_noTransactionId : tx->id;
}

void 
invalbrid_mg::change_transaction_mode(_ITM_transactionState state)
{
  assert (state == modeSerialIrrevocable);
  if (!(gtm_thr()->state & gtm_thread::STATE_IRREVOCABLE))
    restart(RESTART_SERIAL_IRR);
}

void 
invalbrid_mg::acquire_serial_access()
{
  gtm_thread *tx = gtm_thr();
  if (!(tx && (tx->state & gtm_thread::STATE_SERIAL)))
    {
      pthread_mutex_lock(&commit_lock);
    }
}

void 
invalbrid_mg::release_serial_access()
{
  gtm_thread *tx = gtm_thr();
  if (!(tx && (tx->state & gtm_thread::STATE_SERIAL)))
  {
    pthread_mutex_unlock(&commit_lock);
  }
}

void 
invalbrid_mg::restart(gtm_restart_reason rr) 
{
  gtm_thread* tx = gtm_thr();
  assert(rr != NO_RESTART);
  // Only non irrevocable transactions my restart.
  assert(!(tx->state & gtm_thread::STATE_IRREVOCABLE));
  tx->restart_total++;
  tx->restart_reason[rr]++;
  tx->rollback();
  tx->undolog.rollback(tx);
  uint32_t ret = 0;
  if (rr == RESTART_UNINSTRUMENTED_CODEPATH || rr == RESTART_SERIAL_IRR)
    {
      assert(tx->prop & pr_hasNoAbort);
      set_abi_disp(dispatch_invalbrid_sglsw());
      ret = a_runUninstrumentedCode;
    }
  else 
    {
      if (tx->restart_total < SW_RESTARTS + HW_RESTARTS)
	{
	  set_abi_disp(dispatch_invalbrid_specsw());
	  ret = a_runInstrumentedCode;
	}
      // If we have restartet to many times, switch to serial mode.
      else
	{
	  // If the transaction has no abort, we can use sglsw oder irrevocsw
	  // transactionen.
	  if (tx->prop & pr_hasNoAbort)
	  {
	    if (sw_cnt.load() == 0)
	      {
		set_abi_disp(dispatch_invalbrid_sglsw());
		ret = a_runUninstrumentedCode;
	      }
	    else
	      {
		set_abi_disp(dispatch_invalbrid_irrevocsw()); 
		ret = a_runUninstrumentedCode;
	      }
	  }
	  // If the transaction may abort, we have to use specsw in serial mode.
	  else
	    {
	      set_abi_disp(dispatch_invalbrid_specsw());
	      ret = a_runInstrumentedCode;
	      tx->state |= gtm_thread::STATE_SERIAL;
	    }
	}
    }
  abi_disp()->begin();
  GTM_longjmp (ret | a_restoreLiveVariables, &(tx->jb), tx->prop);
}

void 
invalbrid_mg::invalidate()
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
      // Invalidate other software transactions in this case other SpecSWs.
      // IrrevocSWs also have the state software, because they carry a
      // writeset, but an IrrevocSW can not be active at this point, because
      // invalidation is only done when holding the commit lock.
      if((*prev)->shared_state.load() & gtm_thread::STATE_SOFTWARE)
      {
	invalbrid_tx_data *prev_data = (invalbrid_tx_data*) (*prev)->tx_data.load();
	assert(prev_data != NULL);
	bloomfilter *w_bf = prev_data->writeset.load();
	bloomfilter *r_bf = prev_data->readset.load();
	if (bf->intersects(w_bf))
	  {
	    prev_data->invalid_reason.store(RESTART_LOCKED_WRITE, memory_order_release);
	  }
	if (bf->intersects(r_bf))
	  {
	    prev_data->invalid_reason.store(RESTART_LOCKED_READ, memory_order_release);
	  }
      }
    }
  tx->thread_lock.reader_unlock();
}


// Bloomfilter implementation.

namespace
{
  union hash_bit_accessor 
  {
    void *ptr;	//TODO hash the pointer
  #ifdef __x86_64__
    uint8_t blocks[8];
  #else
    uint8_t blocks[4];
  #endif
  };
} // Anon namespace

void
bloomfilter::add_address(const void *ptr, size_t len)
{
  uint32_t tmp_bf[BLOOMFILTER_LENGTH] = {};
  hash_bit_accessor hb;
  for (size_t j = 0; j<len; j++)
  {
    hb.ptr = (uint8_t*)ptr + j;
    for (int i=0; i<4; i++)
    {
      uint32_t block;
    #ifdef __x86_64__
      block = hb.blocks[i*2];
    #else
      block = hb.blocks[i];
    #endif
      tmp_bf[i*8+block/32] |= 0x01 << block%32;
    }
  }
  for (int i=0; i<BLOOMFILTER_LENGTH; i++)
    if (tmp_bf[i] != 0)
      bf[i] |= tmp_bf[i];
}

void
bloomfilter::set(const bloomfilter* bfilter)
{
  const atomic<uint32_t> *data = bfilter->bf;
  for (int i=0; i<32; i++)
    bf[i].store(data[i].load()); 
}

bool
bloomfilter::intersects(const bloomfilter* bfilter)
{
  bool result = true;
  const atomic<uint32_t> *data = bfilter->bf;
  for (int i=0; i<4; i++)
  {
    uint32_t block_result = 0;
    for (int j=0; j<8; j++)
      block_result |= bf[i*8+j].load() & data[i*8+j].load();
    if (block_result == 0)
      result = false;
  }
  return result;
}

bool
bloomfilter::empty ()
{
  bool ret = true;
  for (int i=0; i<32; i++)
    if (bf[i].load() == 0) ret = false;
  return ret;
}

void
bloomfilter::clear()
{
  for (int i=0; i<32; i++) bf[i].store(0);
}

// Allocate a bloomfilter structure.
void *
bloomfilter::operator new (size_t s)
{
  void *bf;

  assert(s == sizeof(bloomfilter));

  bf = xmalloc (sizeof (bloomfilter), true);

  return bf;
}

// Free the given bloomfilter.
void
bloomfilter::operator delete(void *bf)
{
  free(bf);
}

//Constructor and destructor for invalbrid transactional data.
invalbrid_tx_data::invalbrid_tx_data()
{
  log_size = 0;
  local_commit_sequence = 0;
  invalid_reason.store(NO_RESTART);
  readset.store(new bloomfilter());
  writeset.store(new bloomfilter());
}

invalbrid_tx_data::~invalbrid_tx_data()
{
  delete writeset.load();
  delete readset.load();
  if (write_log != NULL)
    delete write_log;
}

// Allocate a transaction data structure.
void *
invalbrid_tx_data::operator new (size_t s)
{
  void *tx_data;

  assert(s == sizeof(invalbrid_tx_data));

  tx_data = xmalloc (sizeof (invalbrid_tx_data), true);

  return tx_data;
}

// Free the given transaction data.
void
invalbrid_tx_data::operator delete(void *tx_data)
{
  free(tx_data);
}

void
invalbrid_tx_data::clear()
{
  log_size = 0;
  if (write_log != NULL)
    write_log->rollback();
  local_commit_sequence = 0;
  bloomfilter *ws = writeset.load();
  bloomfilter *rs = readset.load();
  ws->clear();
  rs->clear();
  invalid_reason.store(NO_RESTART, memory_order_release);
}

gtm_transaction_data*
invalbrid_tx_data::save()
{
  invalbrid_tx_data *ret = new invalbrid_tx_data();
  // Get the pointers to the bloomfilters of this tx data and the checkpoint's
  // tx data.
  bloomfilter *ws = writeset.load();
  bloomfilter *rs = readset.load();
  bloomfilter *ret_ws = ret->writeset.load();
  bloomfilter *ret_rs = ret->readset.load();
  // Set the bloomfilters of the checkpoint to the value of this tx datas
  // bloomfilters.
  ret_ws->set(ws);
  ret_rs->set(rs);
  ret->log_size = log_size;
  ret->local_commit_sequence = local_commit_sequence;
  // The invalid flag and reason are not save, because they are not restored by
  // load(), see load().
  return (gtm_transaction_data*) ret;
}

void
invalbrid_tx_data::load(gtm_transaction_data* tx_data)
{
  invalbrid_tx_data *data = (invalbrid_tx_data*) tx_data;
  bloomfilter *ws = writeset.load();
  bloomfilter *rs = readset.load();
  bloomfilter *others_ws = data->writeset.load();
  bloomfilter *others_rs = data->readset.load();
  ws->set(others_ws);
  rs->set(others_rs);
  log_size = data->log_size;
  write_log->rollback(data->log_size);
  local_commit_sequence = data->local_commit_sequence;
  // The invalid flag and reason are not restored to prevent lost updates on them.
  // The data object is no longer needed after the containing information has
  // been loaded, so it's deleted or we would have a memory leak.
  delete data;
}

// Invalbrid method group as a singleton.
static invalbrid_mg o_invalbrid_mg;

// Getter for the invalbrid method group.
method_group *
GTM::method_group_invalbrid()
{
  return &o_invalbrid_mg;
}
