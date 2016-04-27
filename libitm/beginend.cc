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
#include <ctype.h>
#include <iostream>
#include "invalbrid-mg.h"


using namespace GTM;

// Provides a on-thread-exit callback used to release per-thread data.
static pthread_key_t thr_release_key;
static pthread_once_t thr_release_once = PTHREAD_ONCE_INIT;


// Thread linkage initialization
gtm_thread *GTM::gtm_thread::list_of_threads = 0;
unsigned GTM::gtm_thread::number_of_threads = 0;
rw_atomic_lock GTM::gtm_thread::thread_lock;


method_group *GTM::method_group::method_gr = 0;


// Calls the current method_groups specific abort handler. Which in turn may
// call a dispatch specific abort handler or abort the transaction another way.
void ITM_REGPARM
_ITM_abortTransaction (_ITM_abortReason reason)
{
  //method_group::method_gr->abort(reason);
    GTM_fatal("abort is not yet supported");
}

// Calls the commit routine of the current method_group.
void ITM_REGPARM
_ITM_commitTransaction(void)
{
  method_group::method_gr->commit();
}

// Calls the commit routine of the current method_group with exception handling.
void ITM_REGPARM
_ITM_commitTransactionEH(void *exc_ptr)
{
  method_group::method_gr->commit_EH(exc_ptr);
}

void ITM_REGPARM
_ITM_changeTransactionMode (_ITM_transactionState state)
{
  method_group::method_gr->change_transaction_mode(state);
}

void set_default_method_group();

// This function is called from assembler. It calls the method_groups begin 
// function, which in turn may call a dispatch specific begin function.
uint32_t
GTM::method_group::begin_transaction(uint32_t prop, const gtm_jmpbuf *jb)
{
  if(unlikely(method_group::method_gr == 0))
    set_default_method_group();
  return method_group::method_gr->begin(prop, jb);
}

// Allocate a transaction structure.
void *
GTM::gtm_thread::operator new (size_t s)
{
  void *tx;

  assert(s == sizeof(gtm_thread));

  tx = xmalloc (sizeof (gtm_thread), true);
  memset (tx, 0, sizeof (gtm_thread));

  return tx;
}

// Free the given transaction. Raises an error if the transaction is still
// in use. 
void
GTM::gtm_thread::operator delete(void *tx)
{
  free(tx);
}

static void
thread_exit_handler(void *)
{
  gtm_thread *thr = gtm_thr();
  if (thr)
    delete thr;
  set_gtm_thr(0);
}

static void
thread_exit_init()
{
  if (pthread_key_create(&thr_release_key, thread_exit_handler))
    GTM_fatal("Creating thread release TLS key failed.");
}


GTM::gtm_thread::~gtm_thread()
{
  if (nesting > 0)
    GTM_fatal("Thread exit while a transaction is still active.");

  // Deregister this transaction.
  thread_lock.writer_lock();
  gtm_thread **prev = &list_of_threads;
  for (; *prev; prev = &(*prev)->next_thread)
    {
      if (*prev == this)
	{
	  *prev = (*prev)->next_thread;
	  break;
	}
    }
  number_of_threads--;
  // Taking the shared_data_lock should be unnecessary, because every other
  // thread that trys to access tx_data should acquire the thread_lock as a
  // reader. But since thread destruction is hopefully uncommon, this shouldn't
  // provide a big overhead.
  shared_data_lock.writer_lock();
  gtm_transaction_data* data = tx_data.load();
  if (data != 0) 
    delete data;
  shared_data_lock.writer_unlock();
  
  #ifdef DEBUG_INVALBRID
    uint32_t restarts = 0;
    for (int i=0; i<NUM_RESTARTS; i++) restarts += restart_reason[i];
    std::cout << "RESTART_REALLOCATE: " << restart_reason[RESTART_REALLOCATE] << "\n"
	      << "RESTART_LOCKED_READ: " << restart_reason[RESTART_LOCKED_READ] << "\n"
	      << "RESTART_LOCKED_WRITE: " << restart_reason[RESTART_LOCKED_WRITE] << "\n"
	      << "RESTART_VALIDATE_READ: " << restart_reason[RESTART_VALIDATE_READ] << "\n"
	      << "RESTART_VALIDATE_WRITE: " << restart_reason[RESTART_VALIDATE_WRITE] << "\n"
	      << "RESTART_VALIDATE_COMMIT: " << restart_reason[RESTART_VALIDATE_COMMIT] << "\n"
	      << "RESTART_SERIAL_IRR: " << restart_reason[RESTART_SERIAL_IRR] << "\n"
	      << "RESTART_NOT_READONLY: " << restart_reason[RESTART_NOT_READONLY] << "\n"
	      << "RESTART_CLOSED_NESTING: " << restart_reason[RESTART_CLOSED_NESTING] << "\n"
	      << "RESTART_INIT_METHOD_GROUP: " << restart_reason[RESTART_INIT_METHOD_GROUP] << "\n"
	      << "RESTART_UNINSTRUMENTED_CODEPATH: " << restart_reason[RESTART_UNINSTRUMENTED_CODEPATH] << "\n"
	      << "RESTART_TRY_AGAIN: " << restart_reason[RESTART_TRY_AGAIN] << "\n"
	      << "total: "<< restarts << "\n";
    std::cout << "SpecSW started:" << tx_types_started[SPEC_SW] << " SpecSW commited:" << tx_types_commited[SPEC_SW] << "\n";
    std::cout << "SglSW started:" << tx_types_started[SGL_SW] << " SglSW commited:" << tx_types_commited[SGL_SW] << "\n";
  #endif
  
  thread_lock.writer_unlock();
}

GTM::gtm_thread::gtm_thread ()
{
  // Register this transaction with the list of all threads' transactions.
  thread_lock.writer_lock();
  next_thread = list_of_threads;
  list_of_threads = this;
  number_of_threads++;
  thread_lock.writer_unlock();

  init_cpp_exceptions ();

  if (pthread_once(&thr_release_once, thread_exit_init))
    GTM_fatal("Initializing thread release TLS key failed.");
  // Any non-null value is sufficient to trigger destruction of this
  // transaction when the current thread terminates.
  if (pthread_setspecific(thr_release_key, this))
    GTM_fatal("Setting thread release TLS key failed.");
}

void
GTM::gtm_thread::rollback (gtm_transaction_cp *cp, bool aborting)
{
  // Perform dispatch-specific rollback. If there is checkpointed tx_data,
  // the dispatch is responsible for restoring that.
  abi_disp()->rollback(cp);

  // Roll back all actions that are supposed to happen around the transaction.
  rollback_user_actions (cp ? cp->user_actions_size : 0);
  commit_allocations (true, (cp ? &cp->alloc_actions : 0));
  revert_cpp_exceptions (cp);

  if (cp)
    {
      // We do not yet handle restarts of nested transactions. To do that, we
      // would have to restore some state (jb, id, prop, nesting) not to the
      // checkpoint but to the transaction that was started from this
      // checkpoint (e.g., nesting = cp->nesting + 1);
      assert(aborting);
      // Roll back the rest of the state to the checkpoint.
      jb = cp->jb;
      id = cp->id;
      prop = cp->prop;
      assert(cp->disp == abi_disp());
      memcpy(&alloc_actions, &cp->alloc_actions, sizeof(alloc_actions));
      nesting = cp->nesting;
      
    }
  else
    {
      // Roll back to the outermost transaction.
      // Restore the jump buffer and transaction properties, which we will
      // need for the longjmp used to restart or abort the transaction.
      if (parent_txns.size() > 0)
	{
	  jb = parent_txns[0].jb;
	  id = parent_txns[0].id;
	  prop = parent_txns[0].prop;
	}
      // Reset the transaction. Do not reset this->state, which is handled by
      // the callers. Note that if we are not aborting, we reset the
      // transaction to the point after having executed begin_transaction
      // (we will return from it), so the nesting level must be one, not zero.
      nesting = (aborting ? 0 : 1);
      if (aborting)
	{
	  cxa_catch_count = 0;
	  restart_total = 0;
	}
      parent_txns.clear();
    }

  if (this->eh_in_flight)
    {
      _Unwind_DeleteException ((_Unwind_Exception *) this->eh_in_flight);
      this->eh_in_flight = NULL;
    }
}

void
GTM::gtm_transaction_cp::save(gtm_thread* tx)
{
  // Save everything that we might have to restore on restarts or aborts.
  jb = tx->jb;
  memcpy(&alloc_actions, &tx->alloc_actions, sizeof(alloc_actions));
  user_actions_size = tx->user_actions.size();
  id = tx->id;
  prop = tx->prop;
  cxa_catch_count = tx->cxa_catch_count;
  cxa_uncaught_count = tx->cxa_uncaught_count;
  disp = abi_disp();
  nesting = tx->nesting;
  // The save() method returns a pointer to a tx_data object, that can be used
  // to restore tx_data.
  gtm_transaction_data *data = tx->tx_data.load();
  if (data != NULL)
    tx_data = data->save();
}

void
GTM::gtm_transaction_cp::commit(gtm_thread* tx)
{
  // Restore state that is not persistent across commits. Exception handling,
  // information, nesting level, and any logs do not need to be restored on
  // commits of nested transactions. Allocation actions must be committed
  // before committing the snapshot.
  tx->jb = jb;
  memcpy(&tx->alloc_actions, &alloc_actions, sizeof(alloc_actions));
  tx->id = id;
  tx->prop = prop;
}

// RW atomic lock implementation.
void
rw_atomic_lock::writer_lock()
{
  writer++;
  int32_t r = 0;
  while (!readers.compare_exchange_strong(r, -1))
  { 
    r = 0;
    cpu_relax(); // TODO Improve with pthread condition or futex
  }
  // We locked readers and other writers out by setting readers to -1. 
}

void
rw_atomic_lock::writer_unlock()
{
  readers++;
  writer--;
}

void
rw_atomic_lock::reader_lock()
{
  while (writer.load() != 0) // TODO Improve with pthread condition or futex
    cpu_relax(); 
  int32_t r = readers.load();
  bool succ = false;
  do
    {
      if (r == -1)
	{	
	  // There is still a writer present.
	  cpu_relax(); 
	  r = readers.load();
	  continue; // TODO Improve with pthread condition or futex
	}
      succ = readers.compare_exchange_strong(r,r+1);
    }
  while (!succ); // TODO Improve with pthread condition or futex
}

void
rw_atomic_lock::reader_unlock()
{
  readers--;
}

rw_atomic_lock::rw_atomic_lock()
{
  writer.store(0);
  readers.store(0);
}

namespace {
// Mutex for setting the default method group.
static pthread_mutex_t mg_mutex = PTHREAD_MUTEX_INITIALIZER;

// Parses the ITM_DEFAULT_METHOD_GROUP enviroment variable and returns 
// the desired method group.
static method_group*
parse_default_method_group()
{
  const char *env = getenv("ITM_DEFAULT_METHOD_GROUP");
  method_group *meth_gr = 0;
  if (env == NULL) 
    {
      return GTM::method_group_invalbrid();
    }

  while (isspace((unsigned char) *env))
    ++env;
  if (strncmp(env, "invalbrid", 9) == 0)
    {
      meth_gr = GTM::method_group_invalbrid();
      env += 9;
    }
  else
    goto unknown;

  while (isspace((unsigned char) *env))
    ++env;
  if (*env == '\0')
    return meth_gr;

 unknown:
  GTM::GTM_error("Unknown TM method group in environment variable "
      "ITM_DEFAULT_METHOD_GROUP\n");
  return 0;
}
} // Anon


// Sets the default method group. This function is called from within 
// begin_transaction when the first transaction starts or when 
// _ITM_getTransactionId or _ITM_inTransaction are called with no prior
// transactional execution.
void
GTM::set_default_method_group()
{
  pthread_mutex_lock(&mg_mutex);
  if(method_group::method_gr == NULL)
  {
    method_group *mg = parse_default_method_group();
    method_group::method_gr = mg;
  }
  pthread_mutex_unlock(&mg_mutex);
}
