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

#ifndef BLOOMFILTER
#define BLOOMFILTER

#include "libitm_i.h"

// The bloomfilter length in bits.
#define BLOOMFILTER_LENGTH 1024
#define BLOOMFILTER_BLOCKS (BLOOMFILTER_LENGTH+63)/64

namespace GTM HIDDEN {
  struct bloomfilter
  {
    atomic<uint64_t> bf[BLOOMFILTER_BLOCKS] = {};

    // Add an address and the following ones, according to len, to the bloomfilter.
    void add_address (const void *, size_t len);
    // Sets this bloomfilter to the value of the given bloomfilter.
    void set (const bloomfilter *);
    // Check for an intersection between the bloomfilters.
    bool intersects (const bloomfilter *);
    // Returns true if the bloomfilter is empty.
    bool empty ();

    void clear();


    static void *operator new(size_t);
    static void operator delete(void *);

  }; // bloomfilter

  // bloomfilter without atomics for hardware transactions
  struct hw_bloomfilter
  {
    uint64_t bf[BLOOMFILTER_BLOCKS] = {};

    // Add an address and the following ones, according to len, to the bloomfilter.
    void add_address (const void *, size_t len);
    // Check for an intersection between the bloomfilters.
    bool intersects (const bloomfilter *);
    // Returns true if the bloomfilter is empty.
    bool empty ();

    void clear();

  }; // hw_bloomfilter
}

#endif

