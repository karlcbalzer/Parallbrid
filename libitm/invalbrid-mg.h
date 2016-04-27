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
#ifndef INVALBRID_MG_H
#define INVALBRID_MG_H

#include "libitm_i.h"
#include <pthread.h>
#include <unordered_map>


namespace GTM HIDDEN {
  struct bloomfilter
  {
    atomic<uint32_t> bf[32] = {};
    
    // Add an address to the bloomfilter.
    void add_address (void *);
    // Adds a bloomfilter to this bloomfilter.
    void set (const bloomfilter *);
    // Check for an intersection between the bloomfilters.
    bool intersects (const bloomfilter *);
    // Returns true if the bloomfilter is empty.
    bool empty ();
    
    void clear();
    
  }; // bloomfilter
  
  struct invalbrid_tx_data: public gtm_transaction_data
  {
    // Bloomfilter for read and write set.
    atomic<bloomfilter*> readset;
    atomic<bloomfilter*> writeset;
    // The write_log stores the speculative writes. 
    gtm_log *write_log;
    size_t log_size;
    // The hash map is used for an easy access to the speculative writes.
    std::unordered_map<const void *, const gtm_word *> write_hash;
    // The invalid flag and the local commit sequenz are used by specsws 
    uint32_t local_commit_sequence;
    // The invalid flag that is set by other transactions, if they invalidate
    // the transaction this transaction data belongs to. The flag is enclosed
    // in an atomic to ensure memory order when threads access it.
    atomic<bool> invalid;
    atomic<gtm_restart_reason> invalid_reason;
    
    void clear();
    void load (gtm_transaction_data*);
    gtm_transaction_data* save();
    
    invalbrid_tx_data();
    ~invalbrid_tx_data();
  };
  
  struct invalbrid_mg : public method_group
  {
    // The commit lock and the software transaction counter are static, to be
    // easy accesable for hardware transactions. 
    static pthread_mutex_t commit_lock;
    static atomic<uint32_t> sw_cnt __attribute__ ((visibility ("default")));
    atomic<uint32_t> commit_sequence;
    uint32_t hw_post_commit;
    
    // A pointer to the tx_data of the transaction, that holds the commit lock.
    atomic<gtm_thread*> committing_tx;
    
    // Decides which TM method should be used for the transaction, sets up the
    // appropiate meta data.
    uint32_t begin(uint32_t, const gtm_jmpbuf *);
    // Aborts the current transaction.
    void abort(_ITM_abortReason) ITM_NORETURN ;
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
    
    invalbrid_mg();
      
  }; // invalbrid_mg
  
} // GTM namespace

#endif // INVALBRIDMG_H
