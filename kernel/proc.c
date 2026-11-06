#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "schedstat.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

#ifdef SCHEDULER_CFS
#define TARGET_LATENCY 48  // ticks in which every runnable process gets a turn
#define MIN_SLICE 3        // shortest turn, in ticks
#define VRT_TICK 1000      // vruntime added by one tick at nice 0

// CFS weight of each nice value from -20 to 19: 1024 / 1.25^nice, rounded
// the way the Linux kernel rounds it, so nice 0 is 1024, -20 is 88761 and
// 19 is 15. One nice level apart is about 10% more or less CPU time.
static const int nice_weight[40] = {
  /* -20 */ 88761, 71755, 56483, 46273, 36291,
  /* -15 */ 29154, 23254, 18705, 14949, 11916,
  /* -10 */  9548,  7620,  6100,  4904,  3906,
  /*  -5 */  3121,  2501,  1991,  1586,  1277,
  /*   0 */  1024,   820,   655,   526,   423,
  /*   5 */   335,   272,   215,   172,   137,
  /*  10 */   110,    87,    70,    56,    45,
  /*  15 */    36,    29,    23,    18,    15,
};

// The run queue: every RUNNABLE process, in ascending order of vruntime.
// Lock order: p->lock, then rq.lock.
struct {
  struct spinlock lock;
  struct proc *head;
  int n;
} rq;
#endif

#ifdef SCHEDULER_MLFQ
#define NQUEUE 4
#define BOOST_TICKS 48     // every process goes back to the top this often
#define CLOCK_PER_TICK 1000000  // the timer interval set in clockintr()

static const int mlfq_slice[NQUEUE] = { 1, 4, 8, 16 };

// One first-in first-out queue per level. Lock order: p->lock, then mq.lock.
struct {
  struct spinlock lock;
  struct proc *head[NQUEUE];
  struct proc *tail[NQUEUE];
} mq;

// Caller must hold mq.lock.
static void
mq_append(int level, struct proc *p)
{
  p->qnext = 0;
  if(mq.tail[level])
    mq.tail[level]->qnext = p;
  else
    mq.head[level] = p;
  mq.tail[level] = p;
}

// Move p one level down, or keep it at the bottom. Caller must hold mq.lock.
static void
mq_demote(struct proc *p)
{
  if(p->level < NQUEUE - 1)
    p->level++;
  p->level_time = 0;
  p->slice_used = 0;
}
#endif

// A record of who was on a CPU at each tick, so that what a scheduler really did
// can be checked from outside. It is off until a program asks for it with
// schedtrace(), and writes nothing to the console, which would change the timing.
#define TRACE_MAX 8192
struct {
  struct spinlock lock;
  int on;
  int n;
  struct schedevent ev[TRACE_MAX];
} trace;

extern void forkret(void);
static void freeproc(struct proc *p);

// The current tick, for timestamps. It reads ticks without tickslock
// because callers may hold a p->lock, and clockintr() takes tickslock
// first and then p->lock (in wakeup), so taking them in the other order
// could deadlock.
static uint
now(void)
{
  return __atomic_load_n(&ticks, __ATOMIC_RELAXED);
}

// Mark p RUNNABLE. Under CFS it also goes into the run queue, behind every
// process with a lower or equal vruntime, so equal ones take turns.
// Caller must hold p->lock.
static void
setrunnable(struct proc *p)
{
  p->state = RUNNABLE;
#ifdef SCHEDULER_CFS
  struct proc **pp;

  acquire(&rq.lock);
  for(pp = &rq.head; *pp && (*pp)->vruntime <= p->vruntime; pp = &(*pp)->rqnext)
    ;
  p->rqnext = *pp;
  *pp = p;
  rq.n++;
  release(&rq.lock);
#endif
#ifdef SCHEDULER_MLFQ
  acquire(&mq.lock);
#if MLFQ_FAIR
  // used up its time at this level, a little at a time between sleeps
  if(p->level_time >= (uint64)mlfq_slice[p->level] * CLOCK_PER_TICK)
    mq_demote(p);
#endif
  mq_append(p->level, p);
  release(&mq.lock);
#endif
}

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
#ifdef SCHEDULER_CFS
  initlock(&rq.lock, "rq");
#endif
#ifdef SCHEDULER_MLFQ
  initlock(&mq.lock, "mq");
#endif
  initlock(&trace.lock, "trace");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  p->ctime = now();
  p->rtime = 0;
  p->etime = 0;
  p->nice = 0;
  p->vruntime = 0;
  p->first_run = -1;
  p->nswitch = 0;
#ifdef SCHEDULER_MLFQ
  // a new process starts at the top
  acquire(&mq.lock);
  p->level = 0;
  p->level_time = 0;
  p->slice_used = 0;
  release(&mq.lock);
#endif

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  setrunnable(p);

  release(&p->lock);
}

// Shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid, nice;
  struct proc *np;
  struct proc *p = myproc();

  // The child inherits the parent's nice value, as in Unix.
  acquire(&p->lock);
  nice = p->nice;
  release(&p->lock);

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));
  np->nice = nice;

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  setrunnable(np);
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  p->etime = now();

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
// If rtime and wtime are not null, they receive the ticks the child
// spent running, and alive but not running. If st is not null, it receives the
// child's whole record.
int
kwait(uint64 addr, uint *rtime, uint *wtime, struct procstat *st)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          if(rtime)
            *rtime = pp->rtime;
          if(wtime){
            // ticks counts CPU 0's timer, but rtime is charged by each
            // CPU's own timer, so on several CPUs rtime can exceed the
            // time alive by one tick.
            uint alive = pp->etime - pp->ctime;
            *wtime = alive > pp->rtime ? alive - pp->rtime : 0;
          }
          if(st){
            st->pid = pp->pid;
            st->ctime = pp->ctime;
            st->first_run = pp->first_run;
            st->etime = pp->etime;
            st->rtime = pp->rtime;
            st->nswitch = pp->nswitch;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

#ifdef SCHEDULER_FCFS
// FCFS: choose the RUNNABLE process that was created first. Several
// processes can be created in the same tick; the lower pid was allocated
// first. Returns the process with its lock held, or 0 if none is RUNNABLE.
static struct proc*
pick(void)
{
  struct proc *p, *best;
  uint ctime;
  int pid;

  for(;;){
    best = 0;
    ctime = pid = 0;
    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);
      if(p->state == RUNNABLE &&
         (best == 0 || p->ctime < ctime || (p->ctime == ctime && p->pid < pid))){
        best = p;
        ctime = p->ctime;
        pid = p->pid;
      }
      release(&p->lock);
    }
    if(best == 0)
      return 0;
    // Another CPU may have taken it since we looked.
    acquire(&best->lock);
    if(best->state == RUNNABLE && best->pid == pid)
      return best;
    release(&best->lock);
  }
}
#endif

#ifdef SCHEDULER_CFS
// CFS: take the process with the smallest vruntime, which is the head of
// the run queue, and give it an equal share of TARGET_LATENCY. Returns the
// process with its lock held, or 0 if none is RUNNABLE.
static struct proc*
pick(void)
{
  struct proc *p;
  int n;

  acquire(&rq.lock);
  if((p = rq.head) == 0){
    release(&rq.lock);
    return 0;
  }
#if CFSLOG
  printf("[Scheduler Tick]\n");
  for(struct proc *q = p; q; q = q->rqnext)
    printf("PID: %d | vRuntime: %lu\n", q->pid, q->vruntime);
  printf("--> Scheduling PID %d (lowest vRuntime)\n", p->pid);
#endif
  n = rq.n;
  rq.head = p->rqnext;
  rq.n--;
  release(&rq.lock);

  // Off the queue, p stays RUNNABLE and no other CPU can choose it. If it
  // has just yielded on another CPU, this waits until that CPU has
  // switched away from it.
  acquire(&p->lock);
  p->slice = TARGET_LATENCY / n;
  if(p->slice < MIN_SLICE)
    p->slice = MIN_SLICE;
  p->slice_used = 0;
  return p;
}
#endif

#ifdef SCHEDULER_MLFQ
// MLFQ: take the first process of the highest queue that has one. Returns the
// process with its lock held, or 0 if none is RUNNABLE.
static struct proc*
pick(void)
{
  struct proc *p = 0;

  acquire(&mq.lock);
  for(int l = 0; l < NQUEUE && p == 0; l++){
    if((p = mq.head[l]) != 0){
      mq.head[l] = p->qnext;
      if(mq.head[l] == 0)
        mq.tail[l] = 0;
      p->qnext = 0;
    }
  }
  release(&mq.lock);
  if(p == 0)
    return 0;

  // As in CFS: off the queue, it stays RUNNABLE and no other CPU can choose it,
  // and if it has just yielded on another CPU this waits until that one is done.
  acquire(&p->lock);
  // The slice is not started afresh here: a process that a higher queue interrupted
  // has only part of it used, and must go on from there. Otherwise a neighbour that
  // wakes every tick, as a sleeper in pause() does, would keep it from ever sinking.
  // A slice restarts when used up, on a boost, or on going to sleep.
  acquire(&mq.lock);
  p->on_cpu_since = r_time();
  release(&mq.lock);
  return p;
}

// Count the time since the last charge against this level. This is the clock and
// not the tick count, because a tick only sees who is running when it arrives:
// a process that always sleeps just before one is never seen at all.
// Caller must hold p->lock.
static void
mlfq_charge(struct proc *p)
{
#if MLFQ_FAIR
  uint64 now = r_time();

  acquire(&mq.lock);
  p->level_time += now - p->on_cpu_since;
  p->on_cpu_since = now;
  release(&mq.lock);
#endif
}

// Every BOOST_TICKS, all processes go back to the top. Otherwise a stream of new
// processes could keep the bottom queues from ever running, and a process that
// has turned interactive could never climb back.
static void
mlfq_boost(void)
{
  acquire(&mq.lock);
  for(int l = 1; l < NQUEUE; l++){
    if(mq.head[l] == 0)
      continue;
    if(mq.tail[0])
      mq.tail[0]->qnext = mq.head[l];
    else
      mq.head[0] = mq.head[l];
    mq.tail[0] = mq.tail[l];
    mq.head[l] = mq.tail[l] = 0;
  }
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    p->level = 0;
    p->level_time = 0;
    p->slice_used = 0;
  }
  release(&mq.lock);
}
#endif

// Called on CPU 0's timer interrupt, after the tick has been counted.
void
schedulertick(void)
{
#ifdef SCHEDULER_MLFQ
  if(now() % BOOST_TICKS == 0)
    mlfq_boost();
#endif
}

// On a timer interrupt: should the running process give up the CPU?
int
preempt(void)
{
#if defined(SCHEDULER_MLFQ)
  struct proc *p = myproc();
  int expired, higher = 0;

  acquire(&p->lock);
  mlfq_charge(p);
  acquire(&mq.lock);
#if MLFQ_FAIR
  expired = p->level_time >= (uint64)mlfq_slice[p->level] * CLOCK_PER_TICK;
#else
  expired = p->slice_used >= mlfq_slice[p->level];
#endif
  if(expired){
    // used its whole slice: the next queue down, or the back of the lowest,
    // which is what makes that one round robin
    mq_demote(p);
  } else {
    // a process waiting in a higher queue takes over at this tick
    for(int l = 0; l < p->level; l++)
      if(mq.head[l])
        higher = 1;
  }
  release(&mq.lock);
  release(&p->lock);
  return expired || higher;
#elif defined(SCHEDULER_FCFS)
  // FCFS keeps a process on the CPU until it exits or sleeps.
  return 0;
#elif defined(SCHEDULER_CFS)
  // CFS lets a process use up its slice.
  struct proc *p = myproc();
  int over;

  acquire(&p->lock);
  over = p->slice_used >= p->slice;
  release(&p->lock);
  return over;
#else
  // Round robin switches on every tick.
  return 1;
#endif
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

#if defined(SCHEDULER_FCFS) || defined(SCHEDULER_CFS) || defined(SCHEDULER_MLFQ)
    if((p = pick()) == 0){
      asm volatile("wfi");
      continue;
    }
    // It is the process's job to release its lock and then reacquire
    // it before jumping back to us.
    p->state = RUNNING;
    if(p->first_run < 0)
      p->first_run = now();
    p->nswitch++;
    c->proc = p;
    swtch(&c->context, &p->context);
    c->proc = 0;
    release(&p->lock);

#else
    // Default Round Robin scheduler
    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        if(p->first_run < 0)
          p->first_run = now();
        p->nswitch++;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
#endif
  }
}

// Called on every timer interrupt of every CPU: charge the tick to the
// process running on this CPU, if there is one.
void
proctick(void)
{
  struct proc *p = myproc();

  if(p == 0)
    return;
  acquire(&p->lock);
  p->rtime++;
#ifdef SCHEDULER_CFS
  // Weighted by nice: a tick at nice 0 adds VRT_TICK, and a heavier
  // process adds less, so it stays near the front of the queue longer.
  p->vruntime += VRT_TICK * 1024 / nice_weight[p->nice + 20];
  p->slice_used++;
#endif
#ifdef SCHEDULER_MLFQ
  acquire(&mq.lock);
  p->slice_used++;
  release(&mq.lock);
#endif
  acquire(&trace.lock);
  if(trace.on && trace.n < TRACE_MAX){
    trace.ev[trace.n].tick = now();
    trace.ev[trace.n].pid = p->pid;
#ifdef SCHEDULER_MLFQ
    trace.ev[trace.n].level = p->level;
#else
    trace.ev[trace.n].level = 0;
#endif
    trace.n++;
  }
  release(&trace.lock);
  release(&p->lock);
}

// schedtrace(0, 0) clears the record and starts it. schedtrace(buf, max) stops it
// and copies out up to max events, returning how many.
int
kschedtrace(uint64 addr, int max)
{
  struct proc *p = myproc();
  int n;

  acquire(&trace.lock);
  if(addr == 0){
    trace.n = 0;
    trace.on = 1;
    release(&trace.lock);
    return 0;
  }
  trace.on = 0;
  n = trace.n < max ? trace.n : max;
  release(&trace.lock);
  // copyout can take its time, and the record cannot change while it is off
  for(int i = 0; i < n; i++)
    if(copyout(p->pagetable, addr + i * sizeof(struct schedevent),
               (char *)&trace.ev[i], sizeof(struct schedevent)) < 0)
      return -1;
  return n;
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
#ifdef SCHEDULER_MLFQ
  mlfq_charge(p);
#endif
  setrunnable(p);
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

#ifdef SCHEDULER_MLFQ
  mlfq_charge(p);
  // the course's rule: giving up the CPU early keeps the level, with a new slice
  acquire(&mq.lock);
  p->slice_used = 0;
  release(&mq.lock);
#endif
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        setrunnable(p);
      }
      release(&p->lock);
    }
  }
}

// Set the nice value of the process with the given pid.
int
ksetnice(int pid, int nice)
{
  struct proc *p;

  if(nice < -20 || nice > 19)
    return -1;
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid && p->state != UNUSED){
      p->nice = nice;
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        setrunnable(p);
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("  ctime %u rtime %u nice %d", p->ctime, p->rtime, p->nice);
#ifdef SCHEDULER_CFS
    printf(" vruntime %lu", p->vruntime);
#endif
#ifdef SCHEDULER_MLFQ
    printf(" level %d", p->level);
#endif
    printf("\n");
  }
}
