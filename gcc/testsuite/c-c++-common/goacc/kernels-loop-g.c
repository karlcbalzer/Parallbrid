/* { dg-additional-options "-O2" } */
/* { dg-additional-options "-g" } */
/* { dg-additional-options "-ftree-parallelize-loops=32" } */
/* { dg-additional-options "-fdump-tree-parloops1-all" } */
/* { dg-additional-options "-fdump-tree-optimized" } */

#include "kernels-loop.c"

/* Check that only one loop is analyzed, and that it can be parallelized.  */
/* { dg-final { scan-tree-dump-times "SUCCESS: may be parallelized" 1 "parloops1" } } */
/* { dg-final { scan-tree-dump-not "FAILED:" "parloops1" } } */

/* Check that the loop has been split off into a function.  */
/* { dg-final { scan-tree-dump-times "(?n);; Function .*main._omp_fn.0" 1 "optimized" } } */

/* { dg-final { scan-tree-dump-times "(?n)oacc function \\(32," 1 "parloops1" } } */

