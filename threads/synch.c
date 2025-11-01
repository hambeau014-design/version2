/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Semaphore structure is defined in synch.h */

/* Initializes semaphore SEMA to VALUE. */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);
  sema->value = value;
  list_init (&sema->waiters);
}

/* Insert into a waiters list ordered by priority (FIFO for ties). */
static inline void
push_waiter_prio_fifo (struct list *waiters, struct thread *t)
{
  if (list_empty (waiters)) {
    list_push_back (waiters, &t->elem);
    return;
  }
  struct list_elem *e = list_begin (waiters);
  for (; e != list_end (waiters); e = list_next (e)) {
    struct thread *x = list_entry (e, struct thread, elem);
    if (x->priority < t->priority) break;
  }
  list_insert (e, &t->elem);
}

/* Down or "P" operation on a semaphore. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      push_waiter_prio_fifo (&sema->waiters, thread_current ());
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/* Up or "V" operation on a semaphore. */
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (!list_empty (&sema->waiters)) 
    {
      struct thread *t = list_entry (list_pop_front (&sema->waiters),
                                     struct thread, elem);
      thread_unblock (t);
    }
  sema->value++;
  intr_set_level (old_level);

  /* Allow immediate preemption if a higher-priority thread was unblocked. */
  thread_yield ();
}

/* Lock and condition variable follow Pintos skeleton with waiter ordering. */

void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);
  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  sema_down (&lock->semaphore);
  lock->holder = thread_current ();
}

bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}

void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  lock->holder = NULL;
  sema_up (&lock->semaphore);
}

bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

/* Condition variable. */

void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);
  list_init (&cond->waiters);
}

struct semaphore_elem 
  {
    struct list_elem elem;      /* List element. */
    struct semaphore semaphore; /* This semaphore. */
    int priority;               /* Cached waiter priority for ordering. */
  };

static inline void
cond_push_waiter_prio (struct list *waiters, struct semaphore_elem *se)
{
  if (list_empty (waiters)) {
    list_push_back (waiters, &se->elem);
    return;
  }
  struct list_elem *e = list_begin (waiters);
  for (; e != list_end (waiters); e = list_next (e)) {
    struct semaphore_elem *x = list_entry (e, struct semaphore_elem, elem);
    if (x->priority < se->priority) break;
  }
  list_insert (e, &se->elem);
}

void
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  sema_init (&waiter.semaphore, 0);
  waiter.priority = thread_current ()->priority;
  cond_push_waiter_prio (&cond->waiters, &waiter);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters))
    {
      struct semaphore_elem *se =
        list_entry (list_pop_front (&cond->waiters), struct semaphore_elem, elem);
      sema_up (&se->semaphore);
    }
}

void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}

/* Try-down helper (kept identical to Pintos). */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}
