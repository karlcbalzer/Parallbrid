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
#ifndef INVALBRID_MG_H
#define INVALBRID_MG_H

#include "libitm_i.h"
#include "bloomfilter.h"


#define HW_RESTARTS 20
#define SW_RESTARTS 5

namespace GTM HIDDEN {
struct invalbrid_tx_data: public gtm_transaction_data
  {
    // Bloomfilter for read and write set.
    atomic<bloomfilter*> readset;
    atomic<bloomfilter*> writeset;
    // The write_log stores the speculative writes.
    gtm_log *write_log;
    size_t log_size;
    // this undolog is only used by irrevocabosw transactions.
    gtm_undolog *undo_log;
    // The local commit sequence is used by specsws to detect whether an sglsw
    // transaction is or was executing since the specsw transaction started.
    uint32_t local_commit_sequence;
    // The invalid_reason is NO_RESTART if the transaaction executed without
    // conflicts. It is set to a gtm_restart_reason, if this transaction gets
    // invalidated by a committing transaction.
    atomic<gtm_restart_reason> invalid_reason;

    void clear();
    void load (gtm_transaction_data*);
    gtm_transaction_data* save();


    static void *operator new(size_t);
    static void operator delete(void *);

    invalbrid_tx_data();
    ~invalbrid_tx_data();
  };

// This is the tx data for hardware transactions, BFHW by name.
// This is nesseccary because hardwaretransactions can't use atomics.
struct invalbrid_hw_tx_data: public gtm_transaction_data
  {
    hw_bloomfilter* writeset;

    invalbrid_hw_tx_data();
    ~invalbrid_hw_tx_data();

    static void *operator new(size_t);
    static void operator delete(void *);

    void clear();
    void load (gtm_transaction_data*);
    gtm_transaction_data* save();
  };

  struct invalbrid_mg : public method_group
  {
    // The commit lock and the software transaction counter are static, to be
    // easy accesable for hardware transactions.
    static pthread_mutex_t commit_lock;
    static bool commit_lock_available;
    static atomic<uint32_t> sw_cnt __attribute__ ((visibility ("default")));
    atomic<uint32_t> commit_sequence;
    static uint32_t hw_post_commit;
    static rw_atomic_lock hw_post_commit_lock;

    // A pointer to the tx_data of the transaction, that holds the commit lock.
    atomic<gtm_thread*> committing_tx;

    // Decides which TM method should be used for the transaction, sets up the
    // appropiate meta data.
    uint32_t begin(uint32_t, const gtm_jmpbuf *);
    // Aborts the current transaction.
    void abort(_ITM_abortReason) ITM_NORETURN;
    // Trys to commit the current transaction. If it fails, the transaction will
    // be restartet with same or another method.
    void commit();
    void commit_EH(void *);
    // Determines the transactional status of the current thread.
    _ITM_howExecuting in_transaction();
     _ITM_transactionId_t get_transaction_id();
    void change_transaction_mode(_ITM_transactionState);
    // Gives the caller serial access without changing the transaction state to
    // serial. This is needed for changes in the clone table.
    void acquire_serial_access();
    // Releases the serial access acquired in the function above.
    void release_serial_access();
    // Restart routine for any transaction of this method_group. The restart
    // resaon indicates what happend.
    void restart(gtm_restart_reason rr);

    static void invalidate();

    invalbrid_mg();

  }; // invalbrid_mg

} // GTM namespace

#endif // INVALBRIDMG_H

