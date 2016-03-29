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
#include <pthread.h>
#include <iostream>


using namespace GTM;

// Mutex for thread creation.
static pthread_mutex_t thr_mutex = PTHREAD_MUTEX_INITIALIZER;
// Provides a on-thread-exit callback used to release per-thread data.
static pthread_key_t thr_release_key;
static pthread_once_t thr_release_once = PTHREAD_ONCE_INIT;

// Thread linkage initialization
gtm_thread *GTM::gtm_thread::list_of_threads = 0;
unsigned GTM::gtm_thread::number_of_threads = 0;


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
  pthread_mutex_lock(&thr_mutex);
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
  pthread_mutex_unlock(&thr_mutex);
}

GTM::gtm_thread::gtm_thread ()
{
  // Register this transaction with the list of all threads' transactions.
  pthread_mutex_lock(&thr_mutex);
  next_thread = list_of_threads;
  list_of_threads = this;
  number_of_threads++;
  pthread_mutex_unlock(&thr_mutex);

  init_cpp_exceptions ();

  if (pthread_once(&thr_release_once, thread_exit_init))
    GTM_fatal("Initializing thread release TLS key failed.");
  // Any non-null value is sufficient to trigger destruction of this
  // transaction when the current thread terminates.
  if (pthread_setspecific(thr_release_key, this))
    GTM_fatal("Setting thread release TLS key failed.");
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
  method_group* meth_gr = 0;
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
  if(method_group::method_gr == 0)
  {
    method_group *mg = parse_default_method_group();
    mg->init();
    method_group::method_gr = mg;
  }
  pthread_mutex_unlock(&mg_mutex);
}
