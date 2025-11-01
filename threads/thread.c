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

static struct list ready_list;
static struct list all_list;

static struct thread *idle_thread;
static struct thread *initial_thread;
static struct lock tid_lock;

static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

bool thread_mlfqs = false;
static unsigned thread_ticks;
static uint64_t ready_seq_counter;

#define THREAD_MAGIC 0xcd6abf4b
#define TIME_SLICE 4

static void kernel_thread (thread_func *, void *aux);
static void init_thread (struct thread *, const char *name, int priority);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Forward declare idle() to avoid implicit warnings. */
static void idle (void *aux UNUSED);

/* Ensure kernel_thread_frame is defined (some trees don't have it in headers). */
#ifndef KERNEL_THREAD_FRAME_DEFINED
struct kernel_thread_frame
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };
#define KERNEL_THREAD_FRAME_DEFINED 1
#endif

static struct thread *
running_thread (void) 
{
  uint32_t *esp;
  asm ("mov %%esp, %0" : "=g" (esp));
  return (struct thread *) pg_round_down ((const void *) esp);
}

static bool
is_thread (struct thread *t) 
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

static void *
alloc_frame (struct thread *t, size_t size) 
{
  t->stack -= size;
  return t->stack;
}

static inline int mlfqs_slice_for_level (int lvl) {
  return (lvl == 0) ? 2 : (lvl == 1) ? 4 : 8;
}

static inline bool should_preempt_now (struct thread *cur, struct thread *cand) {
  if (thread_mlfqs) return cand->queue_level < cur->queue_level;
  return cand->priority > cur->priority;
}

static void ready_insert_fifo_by_priority (struct thread *t) {
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (t->status == THREAD_READY);
  t->ready_seq = ++ready_seq_counter;
  t->in_ready = true;
  struct list_elem *e = list_begin (&ready_list);
  while (e != list_end (&ready_list)) {
    struct thread *x = list_entry (e, struct thread, elem);
    if (x->priority < t->priority) break;
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
    if (x->queue_level > t->queue_level) break;
    if (x->queue_level == t->queue_level) { e = list_next (e); continue; }
    e = list_next (e);
  }
  list_insert (e, &t->elem);
}

static inline void ready_insert (struct thread *t) {
  if (thread_mlfqs) ready_insert_mlfqs (t);
  else ready_insert_fifo_by_priority (t);
}

void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);
  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  ready_seq_counter = 0;
  thread_ticks = 0;
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

void
thread_start (void) 
{
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);
  intr_enable ();
  sema_down (&idle_started);
}

void
thread_tick (void) 
{
  struct thread *t = thread_current ();
  if (t == idle_thread) idle_ticks++;
  else kernel_ticks++;

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
          int np = w->priority + 1;
          if (np > PRI_DEFAULT) np = PRI_DEFAULT;
          if (np != w->priority) {
            w->priority = np;
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
        if (t->queue_level < 2) t->queue_level++;
        t->time_left = mlfqs_slice_for_level (t->queue_level);
        intr_yield_on_return ();
      }
    }
  } else {
    if (++thread_ticks >= TIME_SLICE) intr_yield_on_return ();
  }
}

void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks\n", idle_ticks, kernel_ticks);
}

typedef void kernel_thread_func (thread_func *, void *);

static void
kernel_thread (thread_func *function, void *aux) 
{
  intr_enable ();
  function (aux);
  thread_exit ();
}

tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Build stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Build stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = switch_entry;

  /* Build stack frame for switch_threads().
     이 부분이 switch.S와 반드시 일치해야 합니다. */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = (void (*) (void)) kernel_thread;
  sf->ebx = 0;
  sf->ebp = 0;
  sf->esi = 0;
  sf->edi = 0;

  t->stack = (uint8_t *) sf;

  thread_unblock (t);

  if (t->priority > thread_current ()->priority)
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

  if (should_preempt_now (cur, t)) {
    if (intr_context ()) intr_yield_on_return ();
    else thread_yield ();
  }
}

void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  ASSERT (!intr_context ());
  old_level = intr_disable ();
  if (cur != idle_thread) {
    ASSERT (!cur->in_ready);
    cur->status = THREAD_READY;
    ready_insert (cur);
  }
  schedule ();
  intr_set_level (old_level);
}

struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  ASSERT (is_thread (t));
  /* ASSERT (t->status == THREAD_RUNNING); */ /* relax for mid-schedule calls */
  return t;
}

tid_t thread_tid (void) { return thread_current ()->tid; }
const char *thread_name (void) { return thread_current ()->name; }

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

void
thread_set_priority (int new_priority) 
{
  if (thread_mlfqs) return;
  enum intr_level old = intr_disable ();
  struct thread *cur = thread_current ();
  if (new_priority > PRI_MAX) new_priority = PRI_MAX;
  if (new_priority < PRI_MIN) new_priority = PRI_MIN;
  cur->priority = new_priority;
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

int thread_get_priority (void) { return thread_current ()->priority; }
void thread_set_nice (int nice UNUSED) {}
int thread_get_nice (void) { return 0; }
int thread_get_recent_cpu (void) { return 0; }
int thread_get_load_avg (void) { return 0; }

static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list)) return idle_thread;
  struct thread *n = list_entry (list_pop_front (&ready_list), struct thread, elem);
  n->in_ready = false;
  n->age = 0;
  if (thread_mlfqs && n->time_left <= 0) n->time_left = mlfqs_slice_for_level (n->queue_level);
  return n;
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
  if (cur != next) prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

void
thread_schedule_tail (struct thread *prev) 
{
  struct thread *cur = running_thread ();
  ASSERT (intr_get_level () == INTR_OFF);
  cur->status = THREAD_RUNNING;
  if (prev != NULL && prev->status == THREAD_DYING) {
    ASSERT (prev != cur);
    palloc_free_page (prev);
  }
}

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

void
idle (void *idle_started_) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);
  for (;;) {
    intr_disable ();
    thread_block ();
    asm volatile ("sti; hlt" : : : "memory");
  }
}

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

/* Expose for debugging and for lib/kernel/debug.c backtraces */
void
thread_foreach (void (*func)(struct thread *t, void *aux), void *aux)
{
  ASSERT (func != NULL);
  enum intr_level old_level = intr_disable ();
  struct list_elem *e;
  for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e)) {
    struct thread *t = list_entry (e, struct thread, allelem);
    func(t, aux);
  }
  intr_set_level (old_level);
}

uint32_t thread_stack_ofs = offsetof(struct thread, stack);
