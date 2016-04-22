/* Copyright (C) 2011-2016 Free Software Foundation, Inc.
   Contributed by Torvald Riegel <triegel@redhat.com>.

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

#ifndef DISPATCH_H
#define DISPATCH_H 1

#include "libitm_i.h"
#include "common.h"

#ifdef __USER_LABEL_PREFIX__
# define UPFX UPFX1(__USER_LABEL_PREFIX__)
# define UPFX1(t) UPFX2(t)
# define UPFX2(t) #t
#else
# define UPFX
#endif

// Creates ABI load/store methods (can be made virtual or static using M,
// use M2 to create separate methods names for virtual and static)
// The _PV variants are for the pure-virtual methods in the base class.
#define ITM_READ_M(T, LSMOD, M, M2)                                         \
  M _ITM_TYPE_##T ITM_REGPARM ITM_##LSMOD##T##M2 (const _ITM_TYPE_##T *ptr) \
  {                                                                         \
    return load(ptr, abi_dispatch::LSMOD);                                  \
  }

#define ITM_READ_M_PV(T, LSMOD, M, M2)                                      \
  M _ITM_TYPE_##T ITM_REGPARM ITM_##LSMOD##T##M2 (const _ITM_TYPE_##T *ptr) \
  = 0;

#define ITM_WRITE_M(T, LSMOD, M, M2)                         \
  M void ITM_REGPARM ITM_##LSMOD##T##M2 (_ITM_TYPE_##T *ptr, \
					 _ITM_TYPE_##T val)  \
  {                                                          \
    store(ptr, val, abi_dispatch::LSMOD);                    \
  }

#define ITM_WRITE_M_PV(T, LSMOD, M, M2)                      \
  M void ITM_REGPARM ITM_##LSMOD##T##M2 (_ITM_TYPE_##T *ptr, \
					 _ITM_TYPE_##T val)  \
  = 0;

// Creates ABI load/store methods for all load/store modifiers for a particular
// type.
#define CREATE_DISPATCH_METHODS_T(T, M, M2) \
  ITM_READ_M(T, R, M, M2)                \
  ITM_READ_M(T, RaR, M, M2)              \
  ITM_READ_M(T, RaW, M, M2)              \
  ITM_READ_M(T, RfW, M, M2)              \
  ITM_WRITE_M(T, W, M, M2)               \
  ITM_WRITE_M(T, WaR, M, M2)             \
  ITM_WRITE_M(T, WaW, M, M2)
#define CREATE_DISPATCH_METHODS_T_PV(T, M, M2) \
  ITM_READ_M_PV(T, R, M, M2)                \
  ITM_READ_M_PV(T, RaR, M, M2)              \
  ITM_READ_M_PV(T, RaW, M, M2)              \
  ITM_READ_M_PV(T, RfW, M, M2)              \
  ITM_WRITE_M_PV(T, W, M, M2)               \
  ITM_WRITE_M_PV(T, WaR, M, M2)             \
  ITM_WRITE_M_PV(T, WaW, M, M2)

// Creates ABI load/store methods for all types.
// See CREATE_DISPATCH_FUNCTIONS for comments.
#define CREATE_DISPATCH_METHODS(M, M2)  \
  CREATE_DISPATCH_METHODS_T (U1, M, M2) \
  CREATE_DISPATCH_METHODS_T (U2, M, M2) \
  CREATE_DISPATCH_METHODS_T (U4, M, M2) \
  CREATE_DISPATCH_METHODS_T (U8, M, M2) \
  CREATE_DISPATCH_METHODS_T (F, M, M2)  \
  CREATE_DISPATCH_METHODS_T (D, M, M2)  \
  CREATE_DISPATCH_METHODS_T (E, M, M2)  \
  CREATE_DISPATCH_METHODS_T (CF, M, M2) \
  CREATE_DISPATCH_METHODS_T (CD, M, M2) \
  CREATE_DISPATCH_METHODS_T (CE, M, M2)
#define CREATE_DISPATCH_METHODS_PV(M, M2)  \
  CREATE_DISPATCH_METHODS_T_PV (U1, M, M2) \
  CREATE_DISPATCH_METHODS_T_PV (U2, M, M2) \
  CREATE_DISPATCH_METHODS_T_PV (U4, M, M2) \
  CREATE_DISPATCH_METHODS_T_PV (U8, M, M2) \
  CREATE_DISPATCH_METHODS_T_PV (F, M, M2)  \
  CREATE_DISPATCH_METHODS_T_PV (D, M, M2)  \
  CREATE_DISPATCH_METHODS_T_PV (E, M, M2)  \
  CREATE_DISPATCH_METHODS_T_PV (CF, M, M2) \
  CREATE_DISPATCH_METHODS_T_PV (CD, M, M2) \
  CREATE_DISPATCH_METHODS_T_PV (CE, M, M2)

// Creates memcpy/memmove/memset methods.
#define CREATE_DISPATCH_METHODS_MEM()  \
virtual void memtransfer(void *dst, const void* src, size_t size,    \
    bool may_overlap, ls_modifier dst_mod, ls_modifier src_mod)       \
{                                                                     \
  if (size > 0)                                                       \
    memtransfer_static(dst, src, size, may_overlap, dst_mod, src_mod); \
}                                                                     \
virtual void memset(void *dst, int c, size_t size, ls_modifier mod)  \
{                                                                     \
  if (size > 0)                                                       \
    memset_static(dst, c, size, mod);                                 \
}

#define CREATE_DISPATCH_METHODS_MEM_PV()  \
virtual void memtransfer(void *dst, const void* src, size_t size,       \
    bool may_overlap, ls_modifier dst_mod, ls_modifier src_mod) = 0;     \
virtual void memset(void *dst, int c, size_t size, ls_modifier mod) = 0;


// Creates ABI load/store functions that can target either a class or an
// object.
#define ITM_READ(T, LSMOD, TARGET, M2)                                 \
  _ITM_TYPE_##T ITM_REGPARM _ITM_##LSMOD##T (const _ITM_TYPE_##T *ptr) \
  {                                                                    \
    return TARGET ITM_##LSMOD##T##M2(ptr);                            \
  }

#define ITM_WRITE(T, LSMOD, TARGET, M2)                                    \
  void ITM_REGPARM _ITM_##LSMOD##T (_ITM_TYPE_##T *ptr, _ITM_TYPE_##T val) \
  {                                                                        \
    TARGET ITM_##LSMOD##T##M2(ptr, val);                                  \
  }

// Creates ABI load/store functions for all load/store modifiers for a
// particular type.
#define CREATE_DISPATCH_FUNCTIONS_T(T, TARGET, M2) \
  ITM_READ(T, R, TARGET, M2)                \
  ITM_READ(T, RaR, TARGET, M2)              \
  ITM_READ(T, RaW, TARGET, M2)              \
  ITM_READ(T, RfW, TARGET, M2)              \
  ITM_WRITE(T, W, TARGET, M2)               \
  ITM_WRITE(T, WaR, TARGET, M2)             \
  ITM_WRITE(T, WaW, TARGET, M2)

// Creates ABI memcpy/memmove/memset functions.
#define ITM_MEMTRANSFER_DEF(TARGET, M2, NAME, READ, WRITE) \
void ITM_REGPARM _ITM_memcpy##NAME(void *dst, const void *src, size_t size)  \
{                                                                            \
  TARGET memtransfer##M2 (dst, src, size,                                   \
	     false, GTM::abi_dispatch::WRITE, GTM::abi_dispatch::READ);      \
}                                                                            \
void ITM_REGPARM _ITM_memmove##NAME(void *dst, const void *src, size_t size) \
{                                                                            \
  TARGET memtransfer##M2 (dst, src, size,                                   \
      GTM::abi_dispatch::memmove_overlap_check(dst, src, size,               \
	  GTM::abi_dispatch::WRITE, GTM::abi_dispatch::READ),                \
      GTM::abi_dispatch::WRITE, GTM::abi_dispatch::READ);                    \
}

#define ITM_MEMSET_DEF(TARGET, M2, WRITE) \
void ITM_REGPARM _ITM_memset##WRITE(void *dst, int c, size_t size) \
{                                                                  \
  TARGET memset##M2 (dst, c, size, GTM::abi_dispatch::WRITE);     \
}                                                                  \


// ??? The number of virtual methods is large (7*4 for integers, 7*6 for FP,
// 7*3 for vectors). Is the cache footprint so costly that we should go for
// a small table instead (i.e., only have two virtual load/store methods for
// each supported type)? Note that this doesn't affect custom code paths at
// all because these use only direct calls.
// A large cache footprint could especially decrease HTM performance (due
// to HTM capacity). We could add the modifier (RaR etc.) as parameter, which
// would give us just 4*2+6*2+3*2 functions (so we'd just need one line for
// the integer loads/stores), but then the modifier can be checked only at
// runtime.
// For memcpy/memmove/memset, we just have two virtual methods (memtransfer
// and memset).
#define CREATE_DISPATCH_FUNCTIONS(TARGET, M2)  \
  CREATE_DISPATCH_FUNCTIONS_T (U1, TARGET, M2) \
  CREATE_DISPATCH_FUNCTIONS_T (U2, TARGET, M2) \
  CREATE_DISPATCH_FUNCTIONS_T (U4, TARGET, M2) \
  CREATE_DISPATCH_FUNCTIONS_T (U8, TARGET, M2) \
  CREATE_DISPATCH_FUNCTIONS_T (F, TARGET, M2)  \
  CREATE_DISPATCH_FUNCTIONS_T (D, TARGET, M2)  \
  CREATE_DISPATCH_FUNCTIONS_T (E, TARGET, M2)  \
  CREATE_DISPATCH_FUNCTIONS_T (CF, TARGET, M2) \
  CREATE_DISPATCH_FUNCTIONS_T (CD, TARGET, M2) \
  CREATE_DISPATCH_FUNCTIONS_T (CE, TARGET, M2) \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RnWt,     NONTXNAL, W)      \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RnWtaR,   NONTXNAL, WaR)    \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RnWtaW,   NONTXNAL, WaW)    \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RtWn,     R,      NONTXNAL) \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RtWt,     R,      W)        \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RtWtaR,   R,      WaR)      \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RtWtaW,   R,      WaW)      \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RtaRWn,   RaR,    NONTXNAL) \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RtaRWt,   RaR,    W)        \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RtaRWtaR, RaR,    WaR)      \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RtaRWtaW, RaR,    WaW)      \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RtaWWn,   RaW,    NONTXNAL) \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RtaWWt,   RaW,    W)        \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RtaWWtaR, RaW,    WaR)      \
  ITM_MEMTRANSFER_DEF(TARGET, M2, RtaWWtaW, RaW,    WaW)      \
  ITM_MEMSET_DEF(TARGET, M2, W)   \
  ITM_MEMSET_DEF(TARGET, M2, WaR) \
  ITM_MEMSET_DEF(TARGET, M2, WaW)


// Creates ABI load/store functions that delegate to a transactional memcpy.
#define ITM_READ_MEMCPY(T, LSMOD, TARGET, M2)                         \
  _ITM_TYPE_##T ITM_REGPARM _ITM_##LSMOD##T (const _ITM_TYPE_##T *ptr)\
  {                                                                   \
    _ITM_TYPE_##T v;                                                  \
    TARGET memtransfer##M2(&v, ptr, sizeof(_ITM_TYPE_##T), false,    \
	GTM::abi_dispatch::NONTXNAL, GTM::abi_dispatch::LSMOD);       \
    return v;                                                         \
  }

#define ITM_WRITE_MEMCPY(T, LSMOD, TARGET, M2)                            \
  void ITM_REGPARM _ITM_##LSMOD##T (_ITM_TYPE_##T *ptr, _ITM_TYPE_##T val)\
  {                                                                       \
    TARGET memtransfer##M2(ptr, &val, sizeof(_ITM_TYPE_##T), false,      \
	GTM::abi_dispatch::LSMOD, GTM::abi_dispatch::NONTXNAL);           \
  }

#define CREATE_DISPATCH_FUNCTIONS_T_MEMCPY(T, TARGET, M2) \
  ITM_READ_MEMCPY(T, R, TARGET, M2)                \
  ITM_READ_MEMCPY(T, RaR, TARGET, M2)              \
  ITM_READ_MEMCPY(T, RaW, TARGET, M2)              \
  ITM_READ_MEMCPY(T, RfW, TARGET, M2)              \
  ITM_WRITE_MEMCPY(T, W, TARGET, M2)               \
  ITM_WRITE_MEMCPY(T, WaR, TARGET, M2)             \
  ITM_WRITE_MEMCPY(T, WaW, TARGET, M2)


namespace GTM HIDDEN {

struct gtm_jmpbuf;
struct gtm_transaction_cp;

// This is the base interface that all TM method groups have to implement.
// The method group is the common structure for all TM methods that could run
// concurrently. It manages which of its TM methods should be used for the
// start/restart of a transaction and holds their common data. Also abort and
// commit of a transaction are handled here, before the methods specific
// handlers are invoked.
// There is only one method group, when this library is used. Which one is 
// determined by an environment variable(ITM_DEFAULT_METHOD_GROUP)
struct method_group
{
public:
  // A reference to the current method group. It is set by 
  // set_default_method_group().
  static method_group *method_gr;
  // This function is invoked from assembler by _ITM_beginTransaction and calls
  // the begin function of the method_group referenced in method_gr.
  static uint32_t begin_transaction(uint32_t, const gtm_jmpbuf *)
    __asm__(UPFX "GTM_begin_transaction") ITM_REGPARM;
  // Decides which TM method should be used for the transaction, sets up the
  // appropiate meta data.
  virtual uint32_t begin(uint32_t, const gtm_jmpbuf *) = 0;
  // Aborts the current transaction.
  virtual void abort(_ITM_abortReason) ITM_NORETURN = 0;
  // Trys to commit the current transaction. If it fails, the transaction will
  // be restartet with same or another method.
  virtual void commit() = 0;
  virtual void commit_EH(void *) = 0;
  // Determines the transactional status of the current thread.
  virtual _ITM_howExecuting in_transaction() = 0;
  virtual _ITM_transactionId_t get_transaction_id() = 0;
  virtual void change_transaction_mode(_ITM_transactionState) = 0;
  // Gives the caller serial access without changing the transaction state to
  // serial. This is needed for changes on the clone table.
  virtual void acquire_serial_access() = 0;
  // Releases the serial access acquired in the function above. 
  virtual void release_serial_access() = 0;
  // Restart routine for any transaction of this method_group. The restart 
  // resaon indicates what happend.
  virtual void restart(gtm_restart_reason rr) = 0;
};


// This is the base interface that all TM methods have to implement.
struct abi_dispatch
{
public:
  enum ls_modifier { NONTXNAL, R, RaR, RaW, RfW, W, WaR, WaW };

private:
  // Disallow copies
  abi_dispatch(const abi_dispatch &) = delete;
  abi_dispatch& operator=(const abi_dispatch &) = delete;

public:
  static bool memmove_overlap_check(void *dst, const void *src, size_t size,
      ls_modifier dst_mod, ls_modifier src_mod);
  
  virtual void begin() = 0;
  virtual gtm_restart_reason trycommit() = 0;
  virtual void rollback(gtm_transaction_cp*) = 0;
  bool can_run_uninstrumented_code() const
  {
    return m_can_run_uninstrumented_code;
  }
  bool can_restart() const
  {
    return m_can_restart;
  }

  // Creates the ABI dispatch methods for loads and stores.
  CREATE_DISPATCH_METHODS_PV(virtual, )
  // Creates the ABI dispatch methods for memcpy/memmove/memset.
  CREATE_DISPATCH_METHODS_MEM_PV()

protected:
  method_group* const m_method_group;
  const bool m_can_run_uninstrumented_code;
  const bool m_can_restart;
  abi_dispatch(method_group* mg, bool uninstrumented, bool restartable) : 
    m_method_group(mg), m_can_run_uninstrumented_code(uninstrumented), 
    m_can_restart(restartable){ }
};

} // GTM namespace


#endif // DISPATCH_H
