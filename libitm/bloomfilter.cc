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

#include "bloomfilter.h"

using namespace GTM;

namespace
{
  // sc_const: a constant which:
  //  * is not zero
  //  * is odd
  //  * is a not-very-regular mix of 1's and 0's
  //  * does not need any other special mathematical properties
  static const uint64_t sc_const = 0xdeadbeefdeadbeefLL;


  // left rotate a 64-bit value by k bits
  static inline uint64_t
  Rot64(uint64_t x, int k)
  {
      return (x << k) | (x >> (64 - k));
  }

  // Mix all 4 inputs together so that h0, h1 are a hash of them all.
  //
  // For two inputs differing in just the input bits
  // Where "differ" means xor or subtraction
  // And the base value is random, or a counting value starting at that bit
  // The final result will have each bit of h0, h1 flip
  // For every input bit,
  // with probability 50 +- .3% (it is probably better than that)
  // For every pair of input bits,
  // with probability 50 +- .75% (the worst case is approximately that)
  static inline void
  ShortEnd(uint64_t &h0, uint64_t &h1, uint64_t &h2, uint64_t &h3)
  {
      h3 ^= h2;  h2 = Rot64(h2,15);  h3 += h2;
      h0 ^= h3;  h3 = Rot64(h3,52);  h0 += h3;
      h1 ^= h0;  h0 = Rot64(h0,26);  h1 += h0;
      h2 ^= h1;  h1 = Rot64(h1,51);  h2 += h1;
      h3 ^= h2;  h2 = Rot64(h2,28);  h3 += h2;
      h0 ^= h3;  h3 = Rot64(h3,9);   h0 += h3;
      h1 ^= h0;  h0 = Rot64(h0,47);  h1 += h0;
      h2 ^= h1;  h1 = Rot64(h1,54);  h2 += h1;
      h3 ^= h2;  h2 = Rot64(h2,32);  h3 += h2;
      h0 ^= h3;  h3 = Rot64(h3,25);  h0 += h3;
      h1 ^= h0;  h0 = Rot64(h0,63);  h1 += h0;
  }

  // The short version of Jenkins SpookyHash function. This version is
  // simplified, because we only hash addresses, that are 4 or 8 bytes long.
  void spooky_hash(const void *ptr, uint64_t *hash)
  {
      // a and b are the seeds for this hash function.
      uint64_t a=0;
      uint64_t b=0;
      uint64_t c=sc_const;
      uint64_t d=sc_const;

      d += ((uint64_t)sizeof(void*)) << 56;
      c += (uint64_t)ptr;
      ShortEnd(a,b,c,d);
      *hash = a;
  }
} // Anon namespace

// Bloomfilter definitions
void
bloomfilter::add_address(const void *ptr, size_t len)
{
  uint64_t tmp_bf[BLOOMFILTER_BLOCKS] = {};
  for (size_t j = 0; j<len; j++)
  {
    uint64_t hash;
    // Hash the pointer so it can be added to the filter.
    spooky_hash((uint8_t*)ptr + j, &hash);
    // Determine the bit to be set in the bloomfilter.
    int bit = hash % BLOOMFILTER_LENGTH;
    // Set the bit.
    tmp_bf[bit/64] |= (1 << (bit % 64));
  }
  for (int i=0; i<BLOOMFILTER_BLOCKS; i++)
    if (tmp_bf[i] != 0)
      bf[i] |= tmp_bf[i];
}

void
bloomfilter::set(const bloomfilter* bfilter)
{
  const atomic<uint64_t> *data = bfilter->bf;
  for (int i=0; i<BLOOMFILTER_BLOCKS; i++)
    bf[i].store(data[i].load());
}

bool
bloomfilter::intersects(const bloomfilter* bfilter)
{
  const atomic<uint64_t> *data = bfilter->bf;
  for (int i=0; i<BLOOMFILTER_BLOCKS; i++)
  {
    // If there is a bit that is set in both filters, then there is an
    // intersection of the bloomfilters. This means, that there is the
    // possibilty of an element beeing part of both filters.
    if (bf[i].load() & data[i].load())
      return true;
  }
  return false;
}

bool
bloomfilter::empty ()
{
  bool ret = true;
  for (int i=0; i<BLOOMFILTER_BLOCKS; i++)
    if (bf[i].load() != 0) ret = false;
  return ret;
}

void
bloomfilter::clear()
{
  for (int i=0; i<BLOOMFILTER_BLOCKS; i++) bf[i].store(0);
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

// Hardware bloomfilter definitions
void
hw_bloomfilter::add_address(const void *ptr, size_t len)
{
  for (size_t j = 0; j<len; j++)
  {
    uint64_t hash;
    // Hash the pointer so it can be added to the filter.
    spooky_hash((uint8_t*)ptr + j, &hash);
    // Determine the bit to be set in the bloomfilter.
    int bit = hash % BLOOMFILTER_LENGTH;
    // Set the bit.
    bf[bit/64] |= (1 << (bit % 64));
  }
}

bool
hw_bloomfilter::intersects(const bloomfilter *bfilter)
{
  const atomic<uint64_t> *data = bfilter->bf;
  for (int i=0; i<BLOOMFILTER_BLOCKS; i++)
  {
    // If there is a bit that is set in both filters, then there is an
    // intersection of the bloomfilters. This means, that there is the
    // possibilty of an element beeing part of both filters.
    if (bf[i] & data[i].load())
      return true;
  }
  return false;
}

bool hw_bloomfilter::empty()
{
  bool ret = true;
  for (int i=0; i<BLOOMFILTER_BLOCKS; i++)
    if (bf[i] != 0) ret = false;
  return ret;
}

void hw_bloomfilter::clear()
{
  for (int i=0; i<BLOOMFILTER_BLOCKS; i++) bf[i] = 0;
}

// Allocate a hardware bloomfilter structure.
void *
hw_bloomfilter::operator new (size_t s)
{
  void *bf;

  assert(s == sizeof(hw_bloomfilter));

  bf = xmalloc (sizeof (hw_bloomfilter), true);

  return bf;
}

// Free the given hardware bloomfilter.
void
hw_bloomfilter::operator delete(void *bf)
{
  free(bf);
}

