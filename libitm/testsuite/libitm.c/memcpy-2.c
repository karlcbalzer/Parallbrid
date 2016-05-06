/* This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.  */

/* Verify memcpy operation.  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <libitm.h>


void
do_memset_abort(uint32_t* buf, size_t len)
{
  __transaction_atomic
  {
    memset(buf, 1, len);
    __transaction_cancel;
  }
  for (size_t i=0; i< len/sizeof(uint32_t); i++)
  {
    if (buf[i] == 0x01010101) 
      {
	abort();
      }
  }
}

void
do_memcpy_abort(uint32_t* dst, const uint32_t* src, size_t len)
{
  __transaction_atomic
  {
    memcpy(dst, src, len);
    __transaction_cancel;
  }
  for (size_t i=0; i< len/sizeof(uint32_t); i++)
  {
    if (dst[i] == src[i])
      {
	abort();
      }
  }
}

int 
main ()
{
  uint32_t src[100];
  uint32_t dst[100];
  
  memset(dst, 0, 100*sizeof(uint32_t));
  memset(src, 1, 100*sizeof(uint32_t));
  
  do_memset_abort(dst, 100 * sizeof(uint32_t));
  do_memcpy_abort(dst, src, 100 * sizeof(uint32_t));
  
  return 0;
}
