/* Copyright (C) 2002-2014 Free Software Foundation, Inc.
   This file is part of the GNU C Library.
   Contributed by Ulrich Drepper <drepper@redhat.com>, 2002.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.
   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <http://www.gnu.org/licenses/>.  */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <not-cancel.h>
#include "pthreadP.h"
#include <lowlevellock.h>
#include <stap-probe.h>
// (dleoni) Include to implement the data structure to track the contention of mutexes
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>

// (dleoni) Include to print the backtrace
#include <execinfo.h>

struct DataItem* hashArray[MAX_NUMBER_MUTEXES]; 
int number_of_collisions;

// (dleoni) The boolean variable to decide whether the contetion of mutexes shoud be measured
bool measure_mutexes_contention;

#define BT_BUF_SIZE 20
// (dleoni) Vectors of last addresses in the backtrace
static __thread void *buffer[BT_BUF_SIZE];


// (dleoni) Vectors of strings corresponding to the addresses in the backtrace
static __thread char **strings;


// (dleoni) Get backtrace
static void getBacktrace() {
//static void getBacktrace(struct DataItem *item) {
   int addresses,j;
   addresses = backtrace(buffer, BT_BUF_SIZE);
   strings = backtrace_symbols(buffer, addresses);
   if (strings == NULL) {
      printf("ERROR!!!\n");
      fflush(stdout);
   }
   for (j = 0; j < addresses; j++)
      printf("%s\n", strings[j]);
      fflush(stdout);
   free(strings); 
}

// (dleoni) Insert a new item in the position given as first argument
static void insertMutex(struct DataItem **position, pthread_mutex_t *address, long contention) {
   struct DataItem *next, *item;
   item = (struct DataItem*) malloc(sizeof(struct DataItem));
   item->mutex_contention = contention;
   item->acquisitions = 1; 
   item->mutex_address = address;
   item->next = NULL;
   item->backtrace_printed = false;

   // insert the new element
   *position = item;
}

// (dleoni) Search for the item in the table corresponding to the given address of the mutex: if found, return the corresponding DataItem structure, otherwise return NULL and set the index where the new element should be put

static struct DataItem** searchMutex(pthread_mutex_t *address, bool *create_new_item, unsigned int *key) {
   
   struct DataItem *next, *mutex, *preceding;
   //get the key for this address
   unsigned int index = ((((long long)address) % 1000003) % MAX_NUMBER_MUTEXES);
   *key = index;

   //if no item corresponds to the key, return the pointer to the corresponding position within the array
   mutex = hashArray[index];
   if(mutex == NULL) {
      *create_new_item = true;
      return &hashArray[index];
   }

   //there's an item: check for the one in the bucket corresponding to the address
   if(mutex->mutex_address == address) {
      return &mutex;
   }
   
   next = mutex->next;
   preceding = mutex;
  
   //check the right item in the bucket
   while((next != NULL) && (next->mutex_address != address)) {
      preceding = next;
      next = next->next;
   }

   //check if found: if not, it has to be added
   if(next == NULL) {
      *create_new_item = true;
      number_of_collisions += 1;
      return &preceding->next;
   }

   
   //return the address of the DataItem structure
   return &next;
}


#ifndef lll_lock_elision
#define lll_lock_elision(lock, try_lock, private)	({ \
      lll_lock (lock, private); 0; })
#endif

#ifndef lll_trylock_elision
#define lll_trylock_elision(a,t) lll_trylock(a)
#endif

#ifndef LLL_MUTEX_LOCK
# define LLL_MUTEX_LOCK(mutex) \
  lll_lock ((mutex)->__data.__lock, PTHREAD_MUTEX_PSHARED (mutex))
# define LLL_MUTEX_TRYLOCK(mutex) \
  lll_trylock ((mutex)->__data.__lock)
# define LLL_ROBUST_MUTEX_LOCK(mutex, id) \
  lll_robust_lock ((mutex)->__data.__lock, id, \
		   PTHREAD_ROBUST_MUTEX_PSHARED (mutex))
# define LLL_MUTEX_LOCK_ELISION(mutex) \
  lll_lock_elision ((mutex)->__data.__lock, (mutex)->__data.__elision, \
		   PTHREAD_MUTEX_PSHARED (mutex))
# define LLL_MUTEX_TRYLOCK_ELISION(mutex) \
  lll_trylock_elision((mutex)->__data.__lock, (mutex)->__data.__elision, \
		   PTHREAD_MUTEX_PSHARED (mutex))
#endif

#ifndef FORCE_ELISION
#define FORCE_ELISION(m, s)
#endif

static int __pthread_mutex_lock_full (pthread_mutex_t *mutex)
     __attribute_noinline__;

int
__pthread_mutex_lock (mutex)
      pthread_mutex_t *mutex;
{
  struct timespec start, stop;
  struct rusage resource_start, resource_stop;
  bool insert_new_mutex = false;
  unsigned int key;
  assert (sizeof (mutex->__size) >= sizeof (mutex->__data));

  unsigned int type = PTHREAD_MUTEX_TYPE_ELISION (mutex);

  LIBC_PROBE (mutex_entry, 1, mutex);

  if (__builtin_expect (type & ~(PTHREAD_MUTEX_KIND_MASK_NP
				 | PTHREAD_MUTEX_ELISION_FLAGS_NP), 0))
    return __pthread_mutex_lock_full (mutex);

  if (__builtin_expect (type == PTHREAD_MUTEX_TIMED_NP, 1))
    {
      FORCE_ELISION (mutex, goto elision);
    simple:
      /* Normal mutex.  */
      if (!measure_mutexes_contention) {
        LLL_MUTEX_LOCK (mutex);
      }
      else {
        getrusage(RUSAGE_THREAD, &resource_start);
        clock_gettime( CLOCK_MONOTONIC, &start);
        LLL_MUTEX_LOCK (mutex);
        clock_gettime( CLOCK_MONOTONIC, &stop);
        getrusage(RUSAGE_THREAD, &resource_stop);
        long mutex_after = ((long)stop.tv_sec) * 1000000000 + (long) stop.tv_nsec;
        long mutex_before = ((long)start.tv_sec) * 1000000000 + (long) start.tv_nsec;
        long mutex_acquisition = mutex_after - mutex_before;
        long resource_after = ((long)resource_stop.ru_utime.tv_sec) * 1000000 + (long) resource_stop.ru_utime.tv_usec;
        long resource_before = ((long)resource_start.ru_utime.tv_sec) * 1000000 + (long) resource_start.ru_utime.tv_usec;
        long resource_nsec_delta = (resource_after - resource_after) * 4 * 1000;
        //printf("Difference:%ld\n", mutex_acquisition - resource_nsec_delta);
        //fflush(stdout); 
        
        struct DataItem **mutex_position = searchMutex(mutex, &insert_new_mutex, &key);
        if(insert_new_mutex) {
           // (dleoni) create a new entry for the mutex
           insertMutex(mutex_position, mutex, mutex_acquisition);
        }
        else {
           struct DataItem *mutex_element = *mutex_position;
           // (dleoni) update the counters for the mutex
           mutex_element->mutex_contention += mutex_acquisition;
           mutex_element->acquisitions += 1;
           if(((mutex_element->mutex_contention / mutex_element->acquisitions)> 10000) && !(mutex_element->backtrace_printed)) {
              getBacktrace();
              mutex_element->backtrace_printed = true;
              printf("TOP CONTENDED:%ld ADDRESS:%p Index:%d\n", mutex_element->mutex_contention, mutex_element->mutex_address, key);
              fflush(stdout);
           }
        }
      }
      assert (mutex->__data.__owner == 0);
    }
#ifdef HAVE_ELISION
  else if (__builtin_expect (type == PTHREAD_MUTEX_TIMED_ELISION_NP, 1))
    {
  elision: __attribute__((unused))
      /* This case can never happen on a system without elision,
         as the mutex type initialization functions will not
	 allow to set the elision flags.  */
      /* Don't record owner or users for elision case.  This is a
         tail call.  */
      return LLL_MUTEX_LOCK_ELISION (mutex);
    }
#endif
  else if (__builtin_expect (PTHREAD_MUTEX_TYPE (mutex)
			     == PTHREAD_MUTEX_RECURSIVE_NP, 1))
    {
      /* Recursive mutex.  */
      pid_t id = THREAD_GETMEM (THREAD_SELF, tid);

      /* Check whether we already hold the mutex.  */
      if (mutex->__data.__owner == id)
	{
	  /* Just bump the counter.  */
	  if (__builtin_expect (mutex->__data.__count + 1 == 0, 0))
	    /* Overflow of the counter.  */
	    return EAGAIN;

	  ++mutex->__data.__count;

	  return 0;
	}

      /* We have to get the mutex.  */
      LLL_MUTEX_LOCK (mutex);

      assert (mutex->__data.__owner == 0);
      mutex->__data.__count = 1;
    }
  else if (__builtin_expect (PTHREAD_MUTEX_TYPE (mutex)
			  == PTHREAD_MUTEX_ADAPTIVE_NP, 1))
    {
      if (! __is_smp)
	goto simple;

      if (LLL_MUTEX_TRYLOCK (mutex) != 0)
	{
	  int cnt = 0;
	  int max_cnt = MIN (MAX_ADAPTIVE_COUNT,
			     mutex->__data.__spins * 2 + 10);
	  do
	    {
	      if (cnt++ >= max_cnt)
		{
		  LLL_MUTEX_LOCK (mutex);
		  break;
		}

#ifdef BUSY_WAIT_NOP
	      BUSY_WAIT_NOP;
#endif
	    }
	  while (LLL_MUTEX_TRYLOCK (mutex) != 0);

	  mutex->__data.__spins += (cnt - mutex->__data.__spins) / 8;
	}
      assert (mutex->__data.__owner == 0);
    }
  else
    {
      pid_t id = THREAD_GETMEM (THREAD_SELF, tid);
      assert (PTHREAD_MUTEX_TYPE (mutex) == PTHREAD_MUTEX_ERRORCHECK_NP);
      /* Check whether we already hold the mutex.  */
      if (__builtin_expect (mutex->__data.__owner == id, 0))
	return EDEADLK;
      goto simple;
    }

  pid_t id = THREAD_GETMEM (THREAD_SELF, tid);

  /* Record the ownership.  */
  mutex->__data.__owner = id;
#ifndef NO_INCR
  ++mutex->__data.__nusers;
#endif

  LIBC_PROBE (mutex_acquired, 1, mutex);

  return 0;
}

static int
__pthread_mutex_lock_full (pthread_mutex_t *mutex)
{
  int oldval;
  pid_t id = THREAD_GETMEM (THREAD_SELF, tid);

  switch (PTHREAD_MUTEX_TYPE (mutex))
    {
    case PTHREAD_MUTEX_ROBUST_RECURSIVE_NP:
    case PTHREAD_MUTEX_ROBUST_ERRORCHECK_NP:
    case PTHREAD_MUTEX_ROBUST_NORMAL_NP:
    case PTHREAD_MUTEX_ROBUST_ADAPTIVE_NP:
      THREAD_SETMEM (THREAD_SELF, robust_head.list_op_pending,
		     &mutex->__data.__list.__next);

      oldval = mutex->__data.__lock;
      do
	{
	again:
	  if ((oldval & FUTEX_OWNER_DIED) != 0)
	    {
	      /* The previous owner died.  Try locking the mutex.  */
	      int newval = id;
#ifdef NO_INCR
	      newval |= FUTEX_WAITERS;
#else
	      newval |= (oldval & FUTEX_WAITERS);
#endif

	      newval
		= atomic_compare_and_exchange_val_acq (&mutex->__data.__lock,
						       newval, oldval);

	      if (newval != oldval)
		{
		  oldval = newval;
		  goto again;
		}

	      /* We got the mutex.  */
	      mutex->__data.__count = 1;
	      /* But it is inconsistent unless marked otherwise.  */
	      mutex->__data.__owner = PTHREAD_MUTEX_INCONSISTENT;

	      ENQUEUE_MUTEX (mutex);
	      THREAD_SETMEM (THREAD_SELF, robust_head.list_op_pending, NULL);

	      /* Note that we deliberately exit here.  If we fall
		 through to the end of the function __nusers would be
		 incremented which is not correct because the old
		 owner has to be discounted.  If we are not supposed
		 to increment __nusers we actually have to decrement
		 it here.  */
#ifdef NO_INCR
	      --mutex->__data.__nusers;
#endif

	      return EOWNERDEAD;
	    }

	  /* Check whether we already hold the mutex.  */
	  if (__builtin_expect ((oldval & FUTEX_TID_MASK) == id, 0))
	    {
	      int kind = PTHREAD_MUTEX_TYPE (mutex);
	      if (kind == PTHREAD_MUTEX_ROBUST_ERRORCHECK_NP)
		{
		  THREAD_SETMEM (THREAD_SELF, robust_head.list_op_pending,
				 NULL);
		  return EDEADLK;
		}

	      if (kind == PTHREAD_MUTEX_ROBUST_RECURSIVE_NP)
		{
		  THREAD_SETMEM (THREAD_SELF, robust_head.list_op_pending,
				 NULL);

		  /* Just bump the counter.  */
		  if (__builtin_expect (mutex->__data.__count + 1 == 0, 0))
		    /* Overflow of the counter.  */
		    return EAGAIN;

		  ++mutex->__data.__count;

		  return 0;
		}
	    }

	  oldval = LLL_ROBUST_MUTEX_LOCK (mutex, id);

	  if (__builtin_expect (mutex->__data.__owner
				== PTHREAD_MUTEX_NOTRECOVERABLE, 0))
	    {
	      /* This mutex is now not recoverable.  */
	      mutex->__data.__count = 0;
	      lll_unlock (mutex->__data.__lock,
			  PTHREAD_ROBUST_MUTEX_PSHARED (mutex));
	      THREAD_SETMEM (THREAD_SELF, robust_head.list_op_pending, NULL);
	      return ENOTRECOVERABLE;
	    }
	}
      while ((oldval & FUTEX_OWNER_DIED) != 0);

      mutex->__data.__count = 1;
      ENQUEUE_MUTEX (mutex);
      THREAD_SETMEM (THREAD_SELF, robust_head.list_op_pending, NULL);
      break;

    case PTHREAD_MUTEX_PI_RECURSIVE_NP:
    case PTHREAD_MUTEX_PI_ERRORCHECK_NP:
    case PTHREAD_MUTEX_PI_NORMAL_NP:
    case PTHREAD_MUTEX_PI_ADAPTIVE_NP:
    case PTHREAD_MUTEX_PI_ROBUST_RECURSIVE_NP:
    case PTHREAD_MUTEX_PI_ROBUST_ERRORCHECK_NP:
    case PTHREAD_MUTEX_PI_ROBUST_NORMAL_NP:
    case PTHREAD_MUTEX_PI_ROBUST_ADAPTIVE_NP:
      {
	int kind = mutex->__data.__kind & PTHREAD_MUTEX_KIND_MASK_NP;
	int robust = mutex->__data.__kind & PTHREAD_MUTEX_ROBUST_NORMAL_NP;

	if (robust)
	  /* Note: robust PI futexes are signaled by setting bit 0.  */
	  THREAD_SETMEM (THREAD_SELF, robust_head.list_op_pending,
			 (void *) (((uintptr_t) &mutex->__data.__list.__next)
				   | 1));

	oldval = mutex->__data.__lock;

	/* Check whether we already hold the mutex.  */
	if (__builtin_expect ((oldval & FUTEX_TID_MASK) == id, 0))
	  {
	    if (kind == PTHREAD_MUTEX_ERRORCHECK_NP)
	      {
		THREAD_SETMEM (THREAD_SELF, robust_head.list_op_pending, NULL);
		return EDEADLK;
	      }

	    if (kind == PTHREAD_MUTEX_RECURSIVE_NP)
	      {
		THREAD_SETMEM (THREAD_SELF, robust_head.list_op_pending, NULL);

		/* Just bump the counter.  */
		if (__builtin_expect (mutex->__data.__count + 1 == 0, 0))
		  /* Overflow of the counter.  */
		  return EAGAIN;

		++mutex->__data.__count;

		return 0;
	      }
	  }

	int newval = id;
#ifdef NO_INCR
	newval |= FUTEX_WAITERS;
#endif
	oldval = atomic_compare_and_exchange_val_acq (&mutex->__data.__lock,
						      newval, 0);

	if (oldval != 0)
	  {
	    /* The mutex is locked.  The kernel will now take care of
	       everything.  */
	    int private = (robust
			   ? PTHREAD_ROBUST_MUTEX_PSHARED (mutex)
			   : PTHREAD_MUTEX_PSHARED (mutex));
	    INTERNAL_SYSCALL_DECL (__err);
	    int e = INTERNAL_SYSCALL (futex, __err, 4, &mutex->__data.__lock,
				      __lll_private_flag (FUTEX_LOCK_PI,
							  private), 1, 0);

	    if (INTERNAL_SYSCALL_ERROR_P (e, __err)
		&& (INTERNAL_SYSCALL_ERRNO (e, __err) == ESRCH
		    || INTERNAL_SYSCALL_ERRNO (e, __err) == EDEADLK))
	      {
		assert (INTERNAL_SYSCALL_ERRNO (e, __err) != EDEADLK
			|| (kind != PTHREAD_MUTEX_ERRORCHECK_NP
			    && kind != PTHREAD_MUTEX_RECURSIVE_NP));
		/* ESRCH can happen only for non-robust PI mutexes where
		   the owner of the lock died.  */
		assert (INTERNAL_SYSCALL_ERRNO (e, __err) != ESRCH || !robust);

		/* Delay the thread indefinitely.  */
		while (1)
		  pause_not_cancel ();
	      }

	    oldval = mutex->__data.__lock;

	    assert (robust || (oldval & FUTEX_OWNER_DIED) == 0);
	  }

	if (__builtin_expect (oldval & FUTEX_OWNER_DIED, 0))
	  {
	    atomic_and (&mutex->__data.__lock, ~FUTEX_OWNER_DIED);

	    /* We got the mutex.  */
	    mutex->__data.__count = 1;
	    /* But it is inconsistent unless marked otherwise.  */
	    mutex->__data.__owner = PTHREAD_MUTEX_INCONSISTENT;

	    ENQUEUE_MUTEX_PI (mutex);
	    THREAD_SETMEM (THREAD_SELF, robust_head.list_op_pending, NULL);

	    /* Note that we deliberately exit here.  If we fall
	       through to the end of the function __nusers would be
	       incremented which is not correct because the old owner
	       has to be discounted.  If we are not supposed to
	       increment __nusers we actually have to decrement it here.  */
#ifdef NO_INCR
	    --mutex->__data.__nusers;
#endif

	    return EOWNERDEAD;
	  }

	if (robust
	    && __builtin_expect (mutex->__data.__owner
				 == PTHREAD_MUTEX_NOTRECOVERABLE, 0))
	  {
	    /* This mutex is now not recoverable.  */
	    mutex->__data.__count = 0;

	    INTERNAL_SYSCALL_DECL (__err);
	    INTERNAL_SYSCALL (futex, __err, 4, &mutex->__data.__lock,
			      __lll_private_flag (FUTEX_UNLOCK_PI,
						  PTHREAD_ROBUST_MUTEX_PSHARED (mutex)),
			      0, 0);

	    THREAD_SETMEM (THREAD_SELF, robust_head.list_op_pending, NULL);
	    return ENOTRECOVERABLE;
	  }

	mutex->__data.__count = 1;
	if (robust)
	  {
	    ENQUEUE_MUTEX_PI (mutex);
	    THREAD_SETMEM (THREAD_SELF, robust_head.list_op_pending, NULL);
	  }
      }
      break;

    case PTHREAD_MUTEX_PP_RECURSIVE_NP:
    case PTHREAD_MUTEX_PP_ERRORCHECK_NP:
    case PTHREAD_MUTEX_PP_NORMAL_NP:
    case PTHREAD_MUTEX_PP_ADAPTIVE_NP:
      {
	int kind = mutex->__data.__kind & PTHREAD_MUTEX_KIND_MASK_NP;

	oldval = mutex->__data.__lock;

	/* Check whether we already hold the mutex.  */
	if (mutex->__data.__owner == id)
	  {
	    if (kind == PTHREAD_MUTEX_ERRORCHECK_NP)
	      return EDEADLK;

	    if (kind == PTHREAD_MUTEX_RECURSIVE_NP)
	      {
		/* Just bump the counter.  */
		if (__builtin_expect (mutex->__data.__count + 1 == 0, 0))
		  /* Overflow of the counter.  */
		  return EAGAIN;

		++mutex->__data.__count;

		return 0;
	      }
	  }

	int oldprio = -1, ceilval;
	do
	  {
	    int ceiling = (oldval & PTHREAD_MUTEX_PRIO_CEILING_MASK)
			  >> PTHREAD_MUTEX_PRIO_CEILING_SHIFT;

	    if (__pthread_current_priority () > ceiling)
	      {
		if (oldprio != -1)
		  __pthread_tpp_change_priority (oldprio, -1);
		return EINVAL;
	      }

	    int retval = __pthread_tpp_change_priority (oldprio, ceiling);
	    if (retval)
	      return retval;

	    ceilval = ceiling << PTHREAD_MUTEX_PRIO_CEILING_SHIFT;
	    oldprio = ceiling;

	    oldval
	      = atomic_compare_and_exchange_val_acq (&mutex->__data.__lock,
#ifdef NO_INCR
						     ceilval | 2,
#else
						     ceilval | 1,
#endif
						     ceilval);

	    if (oldval == ceilval)
	      break;

	    do
	      {
		oldval
		  = atomic_compare_and_exchange_val_acq (&mutex->__data.__lock,
							 ceilval | 2,
							 ceilval | 1);

		if ((oldval & PTHREAD_MUTEX_PRIO_CEILING_MASK) != ceilval)
		  break;

		if (oldval != ceilval)
		  lll_futex_wait (&mutex->__data.__lock, ceilval | 2,
				  PTHREAD_MUTEX_PSHARED (mutex));
	      }
	    while (atomic_compare_and_exchange_val_acq (&mutex->__data.__lock,
							ceilval | 2, ceilval)
		   != ceilval);
	  }
	while ((oldval & PTHREAD_MUTEX_PRIO_CEILING_MASK) != ceilval);

	assert (mutex->__data.__owner == 0);
	mutex->__data.__count = 1;
      }
      break;

    default:
      /* Correct code cannot set any other type.  */
      return EINVAL;
    }

  /* Record the ownership.  */
  mutex->__data.__owner = id;
#ifndef NO_INCR
  ++mutex->__data.__nusers;
#endif

  LIBC_PROBE (mutex_acquired, 1, mutex);

  return 0;
}
#ifndef __pthread_mutex_lock
strong_alias (__pthread_mutex_lock, pthread_mutex_lock)
hidden_def (__pthread_mutex_lock)
#endif


#ifdef NO_INCR
void
__pthread_mutex_cond_lock_adjust (mutex)
     pthread_mutex_t *mutex;
{
  assert ((mutex->__data.__kind & PTHREAD_MUTEX_PRIO_INHERIT_NP) != 0);
  assert ((mutex->__data.__kind & PTHREAD_MUTEX_ROBUST_NORMAL_NP) == 0);
  assert ((mutex->__data.__kind & PTHREAD_MUTEX_PSHARED_BIT) == 0);

  /* Record the ownership.  */
  pid_t id = THREAD_GETMEM (THREAD_SELF, tid);
  mutex->__data.__owner = id;

  if (mutex->__data.__kind == PTHREAD_MUTEX_PI_RECURSIVE_NP)
    ++mutex->__data.__count;
}
#endif
