#ifndef ASSERT_H
#define ASSERT_H
#include <stdio.h>
#include <stdlib.h>
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
/* Prints where it died before aborting — on a system with no debugger
 * attached, that message is the entire post-mortem. */
#define assert(x) \
    ((x) ? (void)0 \
         : (printf("assert failed: %s at %s:%d\n", #x, __FILE__, __LINE__), abort()))
#endif
#endif
