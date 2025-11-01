#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

/* =========================================================================
 *  Pintos Project 1 (threads) — Learning Version
 *  - Preemptive PRIORITY scheduling w/ FIFO among equals
 *  - Aging (ready-queue waiting +1 each tick; at 20 -> priority +1)
 *  - Simplified 3-level MLFQS when booted with -mlfqs (Q0=2, Q1=4, Q2=8)
 *  - Synchronization wait-queues ordered by priority (FIFO on ties)
 * -------------------------------------------------------------------------
 *  This header only introduces SMALL, LOCAL changes:
 *   - Extend struct thread with fields for FIFO tie-breaking, aging, MLFQS
 *   - Keep all stock Pintos interfaces so existing code/tests still compile
 * =========================================================================
 */

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include <stdbool.h>

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                        /* Lowest priority. */
#define PRI_DEFAULT 31                   /* Default priority. */
#define PRI_MAX 63                       /* Highest priority. */

struct thread
  {
    /* ---- Stock Pintos fields (unchanged) ---- */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */
    int priority;                       /* Priority. */
    struct list_elem allelem;           /* List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element. */

    /* ---- Project 1 Learning Additions ---- */
    /* Aging: incremented each tick while waiting in ready_list. */
    int age;

    /* FIFO tie-breaker: increasing sequence when inserted to ready_list.
       Among equal priority (or same MLFQS level), smaller ready_seq runs first. */
    uint64_t ready_seq;

    /* Ready-list membership flag (defensive: avoid double-insert). */
    bool in_ready;

    /* Simplified MLFQS (enabled with -mlfqs):
       Q0=2 ticks, Q1=4, Q2=8. Lower level number = higher queue. */
    int queue_level;   /* -1 in non-MLFQS mode; otherwise 0/1/2. */
    int time_left;     /* Remaining timeslice within current queue. */

    unsigned magic;                     /* Detects stack overflow. */
  };

/* If false (default), use round-robin with (extended) priority.
   If true, use simplified 3-level MLFQS (not the numeric BSD one). */
extern bool thread_mlfqs;

/* Small helper to clamp priority to valid range. */
static inline int clamp_priority (int p) {
  if (p > PRI_MAX) return PRI_MAX;
  if (p < PRI_MIN) return PRI_MIN;
  return p;
}

void thread_init (void);
void thread_start (void);
void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Priority helpers. */
int thread_get_priority (void);
void thread_set_priority (int);

/* Dummy stubs (intentionally trivial) to satisfy tests framework API. */
int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

#endif /* threads/thread.h */
