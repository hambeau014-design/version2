#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

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

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                        /* Lowest priority. */
#define PRI_DEFAULT 31                   /* Default priority. */
#define PRI_MAX 63                       /* Highest priority. */

struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */
    int priority;                       /* Priority. */
    struct list_elem allelem;           /* List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element. */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /* Page directory. */
#endif

    /* ---- Priority Scheduling w/ Aging ---- */
    int age;                            /* Aging counter while in ready queue. */
    uint64_t ready_seq;                 /* FIFO tie-breaker for equal priority. */
    bool in_ready;                      /* Whether in ready_list (to avoid dup). */

    /* ---- Simplified MLFQS ----
       Active when thread_mlfqs == true.
       Q0(2 ticks), Q1(4), Q2(8). Lower level number means higher priority queue. */
    int queue_level;                    /* 0,1,2 if mlfqs; -1 otherwise. */
    int time_left;                      /* Remaining time slice in current queue. */

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

/* Project helper: clamp priority to bounds. */
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

/* Nice/CPU not used for simplified MLFQS. */
int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

#endif /* threads/thread.h */
