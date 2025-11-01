#include "threads/thread.h"
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

/* See thread.h for description of these members. */
static struct list ready_list;         /* Ready threads, ordered. */
static struct list all_list;           /* All threads. */

static struct thread *idle_thread;
static struct thread *initial_thread;

static tid_t allocate_tid (void);

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

bool thread_mlfqs;

/* RR tick counter since last yield (non-MLFQS). */
static unsigned thread_ticks;

/* FIFO arrival sequence for ready_list tie-breaking. */
static uint64_t ready_seq_counter;

/* MLFQS queue slice helper. */
static inline int mlfqs_slice_for_level (int lvl) {
  return (lvl == 0) ? 2 : (lvl == 1) ? 4 : 8; /* Q0=2, Q1=4, Q2=8 */
}

static void kernel_thread (thread_func *, void *aux);

static void init_thread (struct thread *, const char *name, int priority);
static void schedule (void);
static tid_t allocate_tid (void);
static struct thread *next_thread_to_run (void);
void idle (void *aux UNUSED);

#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
#define THREAD_MAGIC 0xcd6abf4b

/* Ready-list insertion helpers */
static void ready_insert_fifo_by_priority (struct thread *t) {
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (t->status == THREAD_READY);

  t->ready_seq = ++ready_seq_counter;
  t->in_ready = true;

  struct list_elem *e = list_begin (&ready_list);
  while (e != list_end (&ready_list)) {
    struct thread *x = list_entry (e, struct thread, elem);
    if (x->priority < t->priority) break; /* insert before lower prio */
    /* equal priority -> FIFO (append after equal) */
    e = list_next (e);
  }
  list_insert (e, &t->elem);
}

/* For simplified MLFQS: order by queue_level first (lower is higher), then FIFO. */
static void ready_insert_mlfqs (struct thread *t) {
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (t->status == THREAD_READY);

  t->ready_seq = ++ready_seq_counter;
  t->in_ready = true;

  struct list_elem *e = list_begin (&ready_list);
  while (e != list_end (&ready_list)) {
    struct thread *x = list_entry (e, struct thread, elem);
    if (x->queue_level > t->queue_level) break; /* insert before worse queue */
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

static inline bool should_preempt_now (struct thread *cur, struct thread *cand) {
  if (thread_mlfqs) return cand->queue_level < cur->queue_level;
  return cand->priority > cur->priority;
}

/* Initializes the threading system. */
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

/* Starts preemptive thread scheduling by enabling interrupts. */
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

/* Called by the timer interrupt handler at each timer tick. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Aging: increase age of all ready_list waiters; apply promotions. */
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
          /* Promote up to PRI_DEFAULT cap (project requirement) */
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

  if (thread_mlfqs) {
    if (t != idle_thread) {
      if (--t->time_left <= 0) {
        if (t->queue_level < 2)
          t->queue_level++;
        t->time_left = mlfqs_slice_for_level (t->queue_level);
        intr_yield_on_return ();
      }
    }
  } else {
    /* Baseline RR: preempt every TIME_SLICE ticks. */
    if (++thread_ticks >= TIME_SLICE)
      intr_yield_on_return ();
  }
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with given priority. */
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

  old_level = intr_disable ();
  thread_unblock (t);
  intr_set_level (old_level);

  /* Preempt if the new thread is higher priority (or higher queue). */
  if (should_preempt_now (thread_current (), t))
    thread_yield ();

  return tid;
}

/* Blocks the current thread. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);
  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state. */
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

/* Returns the running thread. */
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
#ifdef USERPROG
  process_exit ();
#endif
  intr_disable ();
  list_remove (&thread_current ()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Set current thread priority. */
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

/* Nice/CPU APIs (not used in simplified MLFQS). */
void thread_set_nice (int nice UNUSED) {}
int thread_get_nice (void) { return 0; }
int thread_get_recent_cpu (void) { return 0; }
int thread_get_load_avg (void) { return 0; }

/* Chooses and returns the next thread to be scheduled. */
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

/* Schedules a new thread. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Sets up the CPU for running user code in the thread T. */
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
  t->age = 0;
  t->ready_seq = 0;
  t->in_ready = false;
  if (thread_mlfqs) { t->queue_level = 0; t->time_left = mlfqs_slice_for_level (0); }
  else { t->queue_level = -1; t->time_left = 0; }
  t->magic = THREAD_MAGIC;
  list_push_back (&all_list, &t->allelem);
}

/* Idle thread. */
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
      /* Re-enable interrupts and wait for the next interrupt. */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;
  tid = next_tid++;
  return tid;
}
