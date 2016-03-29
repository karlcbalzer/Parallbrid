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

// Invalbrid synchronization locks.
pthread_mutex_t invalbrid_mg::commit_lock
  __attribute__((aligned(HW_CACHELINE_SIZE)));;
std::atomic<uint32_t> invalbrid_mg::commit_sequence;


// Initializes locks mutexes and counter for software transactions.
void invalbrid_mg::init() 
{
  pthread_mutex_init(&commit_lock, NULL);
  commit_sequence.store(0);
}

// Decides as which type of transaction this transaction should run, starts
// the transaction and returns the appropriate _ITM_actions code.
uint32_t invalbrid_mg::begin(uint32_t prop, const gtm_jmpbuf *jb) 
{
  gtm_thread *tx = gtm_thr();
  if (tx == NULL)
  {
    tx = new gtm_thread();
    set_gtm_thr(tx);
  }
  
  if(tx->nesting > 0)
      {
	// This is a nested transaction.
	tx->nesting++;
	return (prop & pr_uninstrumentedCode) ? 
	  a_runUninstrumentedCode : a_runInstrumentedCode;
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
  
  set_abi_disp(dispatch_invalbrid_sglsw());
  abi_disp()->begin();
  return (prop & pr_uninstrumentedCode) ? 
    a_runUninstrumentedCode : a_runInstrumentedCode;
}

void invalbrid_mg::abort(_ITM_abortReason) 
{
  GTM_fatal("abort is not yet supported");
}

void invalbrid_mg::commit() 
{
  gtm_thread *tx = gtm_thr();
  tx->nesting--;
  if (!(tx->nesting > 0))
    {
      gtm_restart_reason rr;
      rr = abi_disp()->trycommit();
      if (rr != NO_RESTART) restart(rr);
    }
}

void invalbrid_mg::commit_EH(void *exc_ptr)
{
  gtm_thread *tx = gtm_thr();
  tx->nesting--;
  gtm_restart_reason rr;
  if (!(tx->nesting >0))
    {
      rr = abi_disp()->trycommit();
      if (rr != NO_RESTART)
      {
	restart(rr);
	tx->eh_in_flight = exc_ptr;
      }
    }
}

_ITM_howExecuting invalbrid_mg::in_transaction()
{
  gtm_thread *tx = gtm_thr();
  if (tx == NULL) 
    return outsideTransaction;
  else
    return (tx->state & gtm_thread::STATE_IRREVOCABLE) ? 
      inIrrevocableTransaction : inRetryableTransaction;
}

_ITM_transactionId_t invalbrid_mg::get_transaction_id()
{
  gtm_thread *tx = gtm_thr();
  return (tx != NULL) ? tx->id : _ITM_noTransactionId;
}

void invalbrid_mg::change_transaction_mode(_ITM_transactionState state)
{
  assert (state == modeSerialIrrevocable);
  if (!(gtm_thr()->state & gtm_thread::STATE_IRREVOCABLE))
    restart(RESTART_SERIAL_IRR);
}

void invalbrid_mg::acquire_serial_access()
{
  gtm_thread *tx = gtm_thr();
  if (!(tx && (tx->state & gtm_thread::STATE_SERIAL)))
    {
      pthread_mutex_lock(&commit_lock);
      abi_dispatch *disp = abi_disp();
      if (tx && disp) 
	{
	  gtm_restart_reason rr = disp->validate();
	  if (!(rr == NO_RESTART))
	    {
	      pthread_mutex_unlock(&commit_lock);
	      restart(rr);
	    }
	}
    }
}

void invalbrid_mg::release_serial_access()
{
  gtm_thread *tx = gtm_thr();
  if (!(tx && (tx->state & gtm_thread::STATE_SERIAL)))
  {
    pthread_mutex_unlock(&commit_lock);
  }
}

void invalbrid_mg::restart(gtm_restart_reason rr) {}

static invalbrid_mg o_invalbrid_mg;



method_group *
GTM::method_group_invalbrid()
{
  return &o_invalbrid_mg;
}
