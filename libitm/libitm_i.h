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

/* The following are internal implementation functions and definitions.
   To distinguish them from those defined by the Intel ABI, they all
   begin with GTM/gtm.  */

#ifndef LIBITM_I_H
#define LIBITM_I_H 1

#define DEBUG_INVALBRID 1

#include "libitm.h"
#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unwind.h>
#include "local_atomic"
#include <pthread.h>

/* Don't require libgcc_s.so for exceptions.  */
extern void _Unwind_DeleteException (_Unwind_Exception*) __attribute__((weak));


#include "common.h"

namespace GTM HIDDEN {

using namespace std;

typedef unsigned int gtm_word __attribute__((mode (word)));

// These values can be given to the restart handler to indicate the
// reason for the restart.  The reason can be used to decide what TM
// method implementation should be used during the next iteration.
enum gtm_restart_reason
{
  RESTART_REALLOCATE,
  RESTART_LOCKED_READ,
  RESTART_LOCKED_WRITE,
  RESTART_VALIDATE_READ,
  RESTART_VALIDATE_WRITE,
  RESTART_VALIDATE_COMMIT,
  RESTART_SERIAL_IRR,
  RESTART_NOT_READONLY,
  RESTART_CLOSED_NESTING,
  RESTART_INIT_METHOD_GROUP,
  RESTART_UNINSTRUMENTED_CODEPATH,
  RESTART_TRY_AGAIN,
  NUM_RESTARTS,
  NO_RESTART = NUM_RESTARTS
};

#ifdef DEBUG_INVALBRID
enum invalbrid_tx_types
{
  SPEC_SW,
  SGL_SW,
  NUM_TYPES
};
#endif

} // namespace GTM

#include "target.h"
#include "rwlock.h"
#include "aatree.h"
#include "dispatch.h"
#include "containers.h"

namespace GTM HIDDEN {


// A log of (de)allocation actions.  We defer handling of some actions until
// a commit of the outermost transaction.  We also rely on potentially having
// both an allocation and a deallocation for the same piece of memory in the
// log; the order in which such entries are processed does not matter because
// the actions are not in conflict (see below).
// This type is private to alloc.c, but needs to be defined so that
// the template used inside gtm_thread can instantiate.
struct gtm_alloc_action
{
  // Iff free_fn_sz is nonzero, it must be used instead of free_fn, and vice
  // versa.
  void (*free_fn)(void *);
  void (*free_fn_sz)(void *, size_t);
  size_t sz;
  // If true, this is an allocation; we discard the log entry on outermost
  // commit, and deallocate on abort.  If false, this is a deallocation and
  // we deallocate on outermost commit and discard the log entry on abort.
  bool allocated;
};

struct gtm_thread;

// Container for transaction specific data, like read and write set.
// This object only exsist 1 time per thread and is destroyed on gtm_threads
// destruction. When a transaction commits, this container has to be resetted.
struct gtm_transaction_data 
{
  virtual ~gtm_transaction_data() {};
  // Should return a pointer to an immutable transaction data object, which can
  // be used to restore the transaction data at a nested checkpoint. Be carefull
  // when using new in this method, because you then have to handle delete in
  // the dispatch rollback or load routine.
  virtual gtm_transaction_data* save() = 0;
  // Sets this transaction datas values to the values given as an argument. 
  virtual void load(gtm_transaction_data *) = 0;
  // Reset prepares this container to be used by the next transaction.
  virtual void clear() = 0; 
};

// A transaction checkpoint: data that has to be saved and restored when doing
// closed nesting.
struct gtm_transaction_cp
{
  gtm_jmpbuf jb;
  aa_tree<uintptr_t, gtm_alloc_action> alloc_actions;
  size_t user_actions_size;
  _ITM_transactionId_t id;
  uint32_t prop;
  uint32_t cxa_catch_count;
  unsigned int cxa_uncaught_count;
  // We might want to use a different but compatible dispatch method for
  // a nested transaction.
  abi_dispatch *disp;
  // Nesting level of this checkpoint (1 means that this is a checkpoint of
  // the outermost transaction).
  uint32_t nesting;
  gtm_transaction_data *tx_data;

  void save(gtm_thread* tx);
  void commit(gtm_thread* tx);
};

// A multible reader single writer lock.
struct rw_atomic_lock
{
  atomic<int32_t> writer;
  atomic<int32_t> readers;
  
  void writer_lock();
  void writer_unlock();
  void reader_lock();
  void reader_unlock();
  
  rw_atomic_lock();
};

// A log for writes. It can be used as a write buffer for lazy version
// management. 
struct gtm_log
{
  vector<gtm_word> log_data;

  // Log the value at a certain address.
  // The easiest way to inline this is to just define this here.
  gtm_word* log(const void *addr_ptr, const void *value_ptr, size_t len)
  {
    size_t words = (len + sizeof(gtm_word) - 1) / sizeof(gtm_word);
    gtm_word *entry = log_data.push(words + 2);
    entry[0] = (gtm_word) addr_ptr;
    entry[1] = len;
    memcpy(&entry[2], value_ptr, len);
    return entry;
  }

  // Sets the log size to 'until_size' thus removing the last entrys.
  void rollback (size_t until_size = 0) { log_data.set_size(until_size); }
  size_t size() const { return log_data.size(); }
  
  // In local.cc
  // Commits the log entrys  to memory.
  void commit (gtm_thread* tx);
};

// An undolog for the ITM logging functions. Can also be used as an undolog for
// eager versionmanagament transactions. Implemented with the gtm_log and commit
// and rollback switched.
struct gtm_undolog
{
  vector<gtm_word> undolog;

  // Log the previous value at a certain address.
  // The easiest way to inline this is to just define this here.
  void log(const void *ptr, size_t len)
  {
    size_t words = (len + sizeof(gtm_word) - 1) / sizeof(gtm_word);
    gtm_word *undo = undolog.push(words + 2);
    memcpy(undo, ptr, len);
    undo[words] = len;
    undo[words + 1] = (gtm_word) ptr;
  }

  void commit () { undolog.clear(); }
  size_t size() const { return undolog.size(); }

  // In local.cc
  void rollback (gtm_thread* tx, size_t until_size = 0);
};

// Contains all thread-specific data required by the entire library.
// This includes all data relevant to a single transaction. Because most
// thread-specific data is about the current transaction, we also refer to
// the transaction-specific parts of gtm_thread as "the transaction" (the
// same applies to names of variables and arguments).
// All but the shared part of this data structure are thread-local data.
// gtm_thread could be split into transaction-specific structures and other
// per-thread data (with those parts then nested in gtm_thread), but this
// would make it harder to later rearrange individual members to optimize data
// accesses. Thus, for now we keep one flat object, and will only split it if
// the code gets too messy.
struct gtm_thread
{

  struct user_action
  {
    _ITM_userCommitFunction fn;
    void *arg;
    bool on_commit;
    _ITM_transactionId_t resuming_id;
  };

  // The jump buffer by which GTM_longjmp restarts the transaction.
  // This field *must* be at the beginning of the transaction.
  gtm_jmpbuf jb;

  // Data used by alloc.c for the malloc/free undo log.
  aa_tree<uintptr_t, gtm_alloc_action> alloc_actions;
  
  // Undo log, used by th ITM logging functions.
  gtm_undolog undolog;

  // Data used by useraction.c for the user-defined commit/abort handlers.
  vector<user_action> user_actions;

  // A numerical identifier for this transaction.
  _ITM_transactionId_t id;

  // The _ITM_codeProperties of this transaction as given by the compiler.
  uint32_t prop;

  // The nesting depth for subsequently started transactions. This variable
  // will be set to 1 when starting an outermost transaction.
  uint32_t nesting;

  // Set if this transaction owns the serial write lock.
  // Can be reset only when restarting the outermost transaction.
  static const uint32_t STATE_SERIAL		= 0x0001;
  // Set if the serial-irrevocable dispatch table is installed.
  // Implies that abort is not possible.
  // Can be reset only when restarting the outermost transaction.
  static const uint32_t STATE_IRREVOCABLE	= 0x0002;
  // Set if this is a software transaction with transactional data.
  static const uint32_t STATE_SOFTWARE 		= 0x0004;
  // Set if this is a hardware transaction.
  static const uint32_t STATE_HARDWARE 		= 0x0008;

  // A bitmask of the above or -1 if this is an active non-serial transaction.
  uint32_t state;

  // In order to reduce cacheline contention on global_tid during
  // beginTransaction, we allocate a block of 2**N ids to the thread
  // all at once.  This number is the next value to be allocated from
  // the block, or 0 % 2**N if no such block is allocated.
  _ITM_transactionId_t local_tid;

  // Data used by eh_cpp.c for managing exceptions within the transaction.
  uint32_t cxa_catch_count;
  // If cxa_uncaught_count_ptr is 0, we don't need to roll back exceptions.
  unsigned int *cxa_uncaught_count_ptr;
  unsigned int cxa_uncaught_count;
  void *eh_in_flight;

  // Checkpoints for closed nesting.
  vector<gtm_transaction_cp> parent_txns;


  // Only restart_total is reset to zero when the transaction commits, the
  // other counters are total values for all previously executed transactions.
  uint32_t restart_reason[NUM_RESTARTS];
  uint32_t restart_total;
  
  #ifdef DEBUG_INVALBRID
    // A counter that records transaction types started.
    uint32_t tx_types_started[NUM_TYPES];
    // A counter that records transaction types commited.
    uint32_t tx_types_commited[NUM_TYPES];
  #endif
  
  // *** The shared part of gtm_thread starts here. ***
  // Shared state is on separate cachelines to avoid false sharing with
  // thread-local parts of gtm_thread.
  
  // This thread lock is for creating, destroying and iterating over threads. 
  static rw_atomic_lock thread_lock;

  // Points to the next thread in the list of all threads.
  gtm_thread* next_thread __attribute__((__aligned__(HW_CACHELINE_SIZE)));

  // The shared_data_lock can be used to protect the shared data.
  rw_atomic_lock shared_data_lock;
  // If this transaction is inactive, shared_state is ~0. Otherwise, this is
  // an active or serial transaction.
  atomic<gtm_word> shared_state;
  
  // The transaction specific data, used by the dispatches, implemented in the
  // method groups.
  atomic<gtm_transaction_data*> tx_data;

  // The head of the list of all threads' transactions.
  static gtm_thread* list_of_threads;
  // The number of all registered threads.
  static unsigned number_of_threads;

  // In alloc.cc
  void commit_allocations (bool, aa_tree<uintptr_t, gtm_alloc_action>*);
  void record_allocation (void *, void (*)(void *));
  void forget_allocation (void *, void (*)(void *));
  void forget_allocation (void *, size_t, void (*)(void *, size_t));
  void discard_allocation (const void *ptr)
  {
    alloc_actions.erase((uintptr_t) ptr);
  }

  // In beginend.cc
  void rollback (gtm_transaction_cp *cp = 0, bool aborting = false);
  
  gtm_thread();
  ~gtm_thread();

  static void *operator new(size_t);
  static void operator delete(void *);
  
  // In eh_cpp.cc
  void init_cpp_exceptions ();
  void revert_cpp_exceptions (gtm_transaction_cp *cp = 0);

  // In useraction.cc
  void rollback_user_actions (size_t until_size = 0);
  void commit_user_actions ();
};

} // namespace GTM

#include "tls.h"

namespace GTM HIDDEN {

// The global variable for the transaction ids. The block size variable is for 
// allocating a chunk of ids to minimize contention on nested transactions.  
#ifdef HAVE_64BIT_SYNC_BUILTINS
static atomic<_ITM_transactionId_t> global_tid;
#else
static _ITM_transactionId_t global_tid;
static pthread_mutex_t global_tid_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
static const _ITM_transactionId_t tid_block_size = 1 << 16;

// An unscaled count of the number of times we should spin attempting to
// acquire locks before we block the current thread and defer to the OS.
// This variable isn't used when the standard POSIX lock implementations
// are used.
extern uint64_t gtm_spin_count_var;

extern "C" uint32_t GTM_longjmp (uint32_t, const gtm_jmpbuf *, uint32_t)
	ITM_NORETURN ITM_REGPARM;

extern "C" void GTM_LB (const void *, size_t) ITM_REGPARM;

extern void GTM_error (const char *fmt, ...)
	__attribute__((format (printf, 1, 2)));
extern void GTM_fatal (const char *fmt, ...)
	__attribute__((noreturn, format (printf, 1, 2)));
	
extern void set_default_method_group();

// Methods that are used by the method groups.
extern abi_dispatch *dispatch_invalbrid_sglsw();
extern abi_dispatch *dispatch_invalbrid_specsw();

// The method groups that can be uses
extern method_group *method_group_invalbrid();


} // namespace GTM

#endif // LIBITM_I_H
