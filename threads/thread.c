#include "threads/thread.h"
/* =========================================================================
 *  Pintos Project 1 (threads) — Learning Version
 *  This is a FULL, DROP-IN replacement for the stock threads/thread.c that:
 *   - Keeps ALL stock private helpers (running_thread, alloc_frame, etc.)
 *   - Preserves TIME_SLICE preemption via intr_yield_on_return()
 *   - Adds FIFO among equal priorities using a 'ready_seq' tiebreaker
 *   - Adds Aging (ready waiters age++; at 20 -> priority+1 up to PRI_DEFAULT)
 *   - Adds simplified 3-level MLFQS when booted with -mlfqs
 *   - Keeps semaphore/condvar ordering adjustments in synch.c (not here)
 * =========================================================================
 */

#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"

/* -------------------------------------------------------------------------
 * Global scheduler data structures
 * ------------------------------------------------------------------------- */
static struct list ready_list;         /* Ready threads (ordered). */
static struct list all_list;           /* All threads. */

static struct thread *idle_thread;     /* Idle thread. */
static struct thread *initial_thread;  /* First thread. */
static struct lock tid_lock;           /* Protects next_tid. */

static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Extended scheduling flags. */
bool thread_mlfqs = false;

/* RR tick counter since last yield (used in non-MLFQS mode). */
static unsigned thread_ticks;

/* FIFO arrival sequence for ready_list tie-breaking. */
static uint64_t ready_seq_counter;

/* -------------------------------------------------------------------------
 * Stock internal helpers (kept from Pintos)
 * ------------------------------------------------------------------------- */
#define THREAD_MAGIC 0xcd6abf4b
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */

static void kernel_thread (thread_func *, void *aux);
static void init_thread (struct thread *, const char *name, int priority);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };
static void idle (void *aux UNUSED);   /* forward declaration */

/* Returns the running thread. */
static struct thread *
running_thread (void)
{
  uint32_t *esp;
  asm ("mov %%esp, %0" : "=g" (esp));
  return (struct thread *) pg_round_down ((const void *) esp);  // 수정됨
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t) 
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Project 1 stack frame allocator: reserve SIZE bytes on T's stack. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack grows downward. */
  t->stack -= size;
  return t->stack;
}

/* -------------------------------------------------------------------------
 * MLFQS helpers (simple queue-level scheduler, NOT numeric BSD)
 * ------------------------------------------------------------------------- */
static inline int mlfqs_slice_for_level (int lvl) {
  /* Q0=2 ticks, Q1=4, Q2=8 */
  return (lvl == 0) ? 2 : (lvl == 1) ? 4 : 8;
}

/* Preemption helper: should CAND preempt CUR now? */
static inline bool should_preempt_now (struct thread *cur, struct thread *cand) {
  if (thread_mlfqs) return cand->queue_level < cur->queue_level;
  return cand->priority > cur->priority;
}

/* Ready-list insertion: priority/FIFO or MLFQS-level/FIFO. */
static void ready_insert_fifo_by_priority (struct thread *t) {
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (t->status == THREAD_READY);

  t->ready_seq = ++ready_seq_counter;
  t->in_ready = true;

  struct list_elem *e = list_begin (&ready_list);
  while (e != list_end (&ready_list)) {
    struct thread *x = list_entry (e, struct thread, elem);
    if (x->priority < t->priority) break; /* insert before lower prio */
    /* equal priority -> FIFO (append after equals) */
    e = list_next (e);
  }
  list_insert (e, &t->elem);
}

static void ready_insert_mlfqs (struct thread *t) {
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (t->status == THREAD_READY);

  t->ready_seq = ++ready_seq_counter;
  t->in_ready = true;

  struct list_elem *e = list_begin (&ready_list);
  while (e != list_end (&ready_list)) {
    struct thread *x = list_entry (e, struct thread, elem);
    if (x->queue_level > t->queue_level) break; /* better (lower) level first */
    if (x->queue_level == t->queue_level) {
      e = list_next (e); /* FIFO within same level */
      continue;
    }
    e = list_next (e);
  }
  list_insert (e, &t->elem);
}

static inline void ready_insert (struct thread *t) {
  if (thread_mlfqs) ready_insert_mlfqs (t);
  else ready_insert_fifo_by_priority (t);
}

/* -------------------------------------------------------------------------
 * Initialization
 * ------------------------------------------------------------------------- */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  ready_seq_counter = 0;
  thread_ticks = 0;

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* -------------------------------------------------------------------------
 * Ticking (called by timer interrupt)
 * ------------------------------------------------------------------------- */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
  else
    kernel_ticks++; /* Project 1: we do not distinguish user vs kernel. */

  /* -------- Aging on ready_list --------
     Every waiting thread ages by +1 per tick.
     When age hits 20:
       - Non-MLFQS: increase priority by +1 up to PRI_DEFAULT (as requested),
                    and re-insert to keep ordering by new priority.
       - MLFQS: promote one queue level upward (toward 0), and reset slice. */
  if (!list_empty (&ready_list)) {
    struct list_elem *e = list_begin (&ready_list);
    while (e != list_end (&ready_list)) {
      struct thread *w = list_entry (e, struct thread, elem);
      e = list_next (e);
      w->age++;
      if (w->age >= 20) {
        w->age = 0;
        if (thread_mlfqs) {
          if (w->queue_level > 0) {
            enum intr_level ol = intr_disable ();
            list_remove (&w->elem);
            w->queue_level--;
            w->time_left = mlfqs_slice_for_level (w->queue_level);
            ready_insert (w);
            intr_set_level (ol);
          }
        } else {
          int newp = w->priority + 1;
          if (newp > PRI_DEFAULT) newp = PRI_DEFAULT;
          if (newp != w->priority) {
            w->priority = newp;
            enum intr_level ol = intr_disable ();
            list_remove (&w->elem);
            ready_insert (w);
            intr_set_level (ol);
          }
        }
      }
    }
  }

  /* -------- Timeslice handling -------- */
  if (thread_mlfqs) {
    if (t != idle_thread) {
      if (--t->time_left <= 0) {
        if (t->queue_level < 2) t->queue_level++;      /* Demote */
        t->time_left = mlfqs_slice_for_level (t->queue_level);
        intr_yield_on_return ();                       /* Preempt */
      }
    }
  } else {
    /* Keep original RR behavior so tests priority-preempt-timer / fifo pass. */
    if (++thread_ticks >= TIME_SLICE)
      intr_yield_on_return ();
  }
}

void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks\n",
          idle_ticks, kernel_ticks);
}

/* -------------------------------------------------------------------------
 * Thread creation / blocking / unblocking / yielding
 * ------------------------------------------------------------------------- */
typedef void kernel_thread_func (thread_func *, void *);

static void
kernel_thread (thread_func *function, void *aux) 
{
  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Run the thread function. */
  thread_exit ();       /* If function returns, kill the thread. */
}

/* Creates a new kernel thread named NAME with given PRIORITY. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) switch_entry;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_threads;
  sf->ebp = 0;

  /* Add to run queue. */
  old_level = intr_disable ();
  thread_unblock (t);
  intr_set_level (old_level);

  /* Preempt if the new thread is higher priority (or higher MLFQS queue). */
  if (should_preempt_now (thread_current (), t))
    thread_yield ();

  return tid;
}

void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;
  struct thread *cur = thread_current ();

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  t->status = THREAD_READY;
  t->in_ready = false;
  t->age = 0;
  ready_insert (t);
  intr_set_level (old_level);

  /* Immediate preemption if needed. */
  if (should_preempt_now (cur, t)) {
    if (intr_context ()) intr_yield_on_return ();
    else thread_yield ();
  }
}

/* Yields the CPU. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;

  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    {
      ASSERT (!cur->in_ready);
      cur->status = THREAD_READY;
      ready_insert (cur);
    }
  schedule ();
  intr_set_level (old_level);
}

/* -------------------------------------------------------------------------
 * Thread introspection and termination
 * ------------------------------------------------------------------------- */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);
  return t;
}

tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

const char *
thread_name (void) 
{
  return thread_current ()->name;
}

void
thread_exit (void) 
{
  ASSERT (!intr_context ());

  intr_disable ();
  list_remove (&thread_current ()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* -------------------------------------------------------------------------
 * Priority helpers
 * ------------------------------------------------------------------------- */
void
thread_set_priority (int new_priority) 
{
  if (thread_mlfqs) return;

  enum intr_level old = intr_disable ();
  struct thread *cur = thread_current ();
  cur->priority = clamp_priority (new_priority);

  if (!list_empty (&ready_list)) {
    struct thread *top = list_entry (list_front (&ready_list), struct thread, elem);
    if (should_preempt_now (cur, top)) {
      intr_set_level (old);
      thread_yield ();
      return;
    }
  }
  intr_set_level (old);
}

int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Stubs required by the spec (unused in this simplified MLFQS). */
void thread_set_nice (int nice UNUSED) {}
int thread_get_nice (void) { return 0; }
int thread_get_recent_cpu (void) { return 0; }
int thread_get_load_avg (void) { return 0; }

/* -------------------------------------------------------------------------
 * Scheduler core
 * ------------------------------------------------------------------------- */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else {
    struct thread *n =
      list_entry (list_pop_front (&ready_list), struct thread, elem);
    n->in_ready = false;
    n->age = 0;
    if (thread_mlfqs && n->time_left <= 0)
      n->time_left = mlfqs_slice_for_level (n->queue_level);
    return n;
  }
}

static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (is_thread (next));
  ASSERT (cur->status != THREAD_RUNNING || cur == next);

  if (cur != next) {
    prev = switch_threads (cur, next);
  }
  thread_schedule_tail (prev);
}

/* Finish thread switch. */
void
thread_schedule_tail (struct thread *prev) 
{
  struct thread *cur = running_thread ();
  ASSERT (intr_get_level () == INTR_OFF);

  cur->status = THREAD_RUNNING;

  /* If the thread we switched from is dying, destroy it. */
  if (prev != NULL && prev->status == THREAD_DYING) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Initialize a thread structure. */
static void
init_thread (struct thread *t, const char *name, int priority) 
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;

  /* Learning additions */
  t->age = 0;
  t->ready_seq = 0;
  t->in_ready = false;
  if (thread_mlfqs) { t->queue_level = 0; t->time_left = mlfqs_slice_for_level (0); }
  else { t->queue_level = -1; t->time_left = 0; }

  t->magic = THREAD_MAGIC;
  list_push_back (&all_list, &t->allelem);
}

/* -------------------------------------------------------------------------
 * Idle thread
 * ------------------------------------------------------------------------- */
void
idle (void *idle_started_) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);
  for (;;) 
    {
      intr_disable ();
      thread_block ();
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Returns a TID to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}
/* thread_foreach()
   Runs FUNC for each thread in all_list (used by debug_backtrace_all). */
void
thread_foreach (void (*func)(struct thread *t, void *aux), void *aux)
{
  ASSERT (func != NULL);

  enum intr_level old_level = intr_disable ();
  struct list_elem *e;

  for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
  {
    struct thread *t = list_entry (e, struct thread, allelem);
    func(t, aux);
  }
  intr_set_level (old_level);
}

/* Needed by switch.S to locate stack offset in struct thread. */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);
