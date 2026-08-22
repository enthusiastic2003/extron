#ifndef _LIBALLOC_H
#define _LIBALLOC_H
#include <stdint.h>
#include <stddef.h>
#include <kernel/mm/vmm.h>

/** \defgroup ALLOCHOOKS liballoc hooks 
 *
 * These are the OS specific functions which need to 
 * be implemented on any platform that the library
 * is expected to work on.
 */

/** @{ */



// If we are told to not define our own size_t, then we skip the define.
//#define _HAVE_UINTPTR_T
//typedef	unsigned long	uintptr_t;

//This lets you prefix malloc and friends
#define PREFIX(func)		k ## func

#ifdef __cplusplus
extern "C" {
#endif



/** This function is supposed to lock the memory data structures. It
 * could be as simple as disabling interrupts or acquiring a spinlock.
 * It's up to you to decide. 
 *
 * \return 0 if the lock was acquired successfully. Anything else is
 * failure.
 */
int liballoc_lock();
int liballoc_unlock();
void* liballoc_alloc(size_t num_pages);
int liballoc_free(void* addr, size_t num_pages);




       

void    *PREFIX(malloc)(size_t);				///< The standard function.
void    *PREFIX(realloc)(void *, size_t);		///< The standard function.
void    *PREFIX(calloc)(size_t, size_t);		///< The standard function.
void     PREFIX(free)(void *);					///< The standard function.


#ifdef __cplusplus
}
#endif


/** @} */

#endif


