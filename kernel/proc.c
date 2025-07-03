// kernel/proc.c
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "custom_logger.h" // Assuming this is needed for sys_trigger

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

// Global thread ID allocator
static struct spinlock tidlock;
static int nexttid = 1;

extern void forkret(void);
// static void freeproc(struct proc *p); // Now defined below as non-static for external use in wait()

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
  initlock(&tidlock, "nexttid"); // Initialize tidlock

  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      // p->kstack = KSTACK((int) (p - proc)); // Kstack for process is now allocated in allocproc
      p->current_thread = 0; // NEW: Initialize current_thread to indicate no active thread
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

// Allocate a unique thread ID.
int
alloctid(void)
{
  int tid;
  acquire(&tidlock);
  tid = nexttid;
  nexttid = nexttid + 1;
  release(&tidlock);
  return tid;
}

// Initialize the first thread for a new process.
void
init_main_thread(struct proc *p) {
    struct thread *main_t = &p->threads[0];

    main_t->state = THREAD_EMBRYO;
    main_t->id = alloctid();

    // Main thread uses the process's trapframe and kernel stack
    main_t->trapframe = p->trapframe;
    main_t->kstack = p->kstack;

    // Initialize context for main thread
    memset(&main_t->context, 0, sizeof(main_t->context));
    main_t->context.ra = (uint64)forkret;
    main_t->context.sp = p->kstack + PGSIZE;

    p->current_thread = main_t;
    main_t->state = THREAD_RUNNABLE;
}

struct thread *
initthread(struct proc *p)
{
  // If the process is new or being re-initialized, clear out old thread data.
  // This loop ensures all thread slots are clean before setting up the main thread.
  // This is the top snippet from the slide.
  // We will always clean up all thread slots when a process is allocated,
  // as allocproc will call this for a new process.
  for (int i = 0; i < NTHREAD; ++i) {
    // freethread handles kfree for trapframe and kstack if they were allocated.
    freethread(&p->threads[i]);
    // Explicitly clear pointers and state if freethread doesn't set them to 0.
    // (freethread already sets them to 0, but this ensures initial clean state)
    p->threads[i].trapframe = 0;
    p->threads[i].kstack = 0;
    p->threads[i].id = 0;
    p->threads[i].join = 0;
    p->threads[i].sleep_n = 0;
    p->threads[i].sleep_tick0 = 0;
    p->threads[i].chan = 0;
    memset(&p->threads[i].context, 0, sizeof(p->threads[i].context));
    p->threads[i].state = THREAD_UNUSED;
  }

  // Initialize the main thread (p->threads[0]) - This is the bottom snippet from the slide.
  struct thread *main_t = &p->threads[0];
  main_t->id = p->pid; // Main thread's ID is process's PID

  // The main thread uses the process's primary trapframe and kernel stack.
  // These are already allocated in allocproc.
  main_t->trapframe = p->trapframe; // Use process's allocated trapframe
  main_t->kstack = p->kstack;       // Use process's allocated kernel stack

  // Initialize context for main thread (similar to allocproc's context init)
  memset(&main_t->context, 0, sizeof(main_t->context));
  main_t->context.ra = (uint64)forkret;
  main_t->context.sp = p->kstack + PGSIZE; // Top of process's kernel stack

  p->current_thread = main_t; // Set main thread as current for the process
  main_t->state = THREAD_RUNNABLE; // Main thread is ready to run

  return p->current_thread; // Return the initialized main thread
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      p->state = EMBRYO; // Process state is EMBRYO during creation

      release(&p->lock); // Release proc lock to acquire pid_lock
      p->pid = allocpid(); // Allocate process ID
      acquire(&p->lock); // Re-acquire proc lock

      // Allocate kernel stack for the process (main thread's kstack).
      // This will be used by the main thread.
      if((p->kstack = (uint64)kalloc()) == 0){
        release(&p->lock);
        return 0;
      }
      memset((void*)p->kstack, 0, PGSIZE);

      // Allocate and clear trapframe for the process (main thread's trapframe).
      // This will be used by the main thread.
      if((p->trapframe = (struct trapframe *)kalloc()) == 0){
        kfree((void*)p->kstack);
        release(&p->lock);
        return 0;
      }
      memset((void*)p->trapframe, 0, PGSIZE);

      // Initialize the process's thread array and its main thread.
      // This function will set p->current_thread and p->threads[0].
      if (initthread(p) == 0) { // Call initthread to set up main thread
          freeproc(p); // If main thread init fails, free process
          release(&p->lock);
          return 0;
      }

      // An empty user page table.
      p->pagetable = proc_pagetable(p);
      if(p->pagetable == 0){
        freeproc(p); // Use the modified freeproc
        release(&p->lock);
        return 0;
      }

      p->sz = PGSIZE; // Initial process memory size

      return p;
    }
    release(&p->lock);
  }
  return 0; // No unused proc found
}


// Free a thread's resources and mark it as unused.
void
freethread(struct thread *t)
{
  if(t->trapframe)
    kfree((void*)t->trapframe);
  t->trapframe = 0;

  if(t->kstack)
    kfree((void*)t->kstack);
  t->kstack = 0;

  t->id = 0;
  t->join = 0;
  t->sleep_n = 0;
  t->sleep_tick0 = 0;
  memset(&t->context, 0, sizeof(t->context));
  t->state = THREAD_UNUSED;
}

struct thread*
thread_schd(struct proc *p) {
    struct thread *t;
    struct thread *next_thread = 0; // The thread chosen to run

    // If the process has a current thread and it was running,
    // set its state to runnable if it's not already sleeping/zombie/etc.
    // This handles the case where the previous thread yielded or was preempted.
    if (p->current_thread && p->current_thread->state == THREAD_RUNNING) {
        p->current_thread->state = THREAD_RUNNABLE;
    }

    // Acquire tickslock for checking timed sleeps
    acquire(&tickslock);
    uint ticks_current = ticks; // Get current ticks
    release(&tickslock);

    // Loop through all threads of the current process
    for (t = p->threads; t < p->threads + NTHREAD; t++) {
        // First, check for RUNNABLE threads
        if (t->state == THREAD_RUNNABLE) {
            next_thread = t;
            break; // Found a runnable thread, prioritize it
        }
        // If not runnable, check for timed-out SLEEPING threads
        else if (t->state == THREAD_SLEEPING && ticks_current - t->sleep_tick0 >= t->sleep_n) {
            t->state = THREAD_RUNNABLE; // Wake up the thread
            next_thread = t; // Make it the next runnable thread
            break; // Prioritize this newly woken thread
        }
    }

    if (next_thread != 0) {
        // If a runnable thread was found (or woken up)
        next_thread->state = THREAD_RUNNING; // Set its state to running
        p->current_thread = next_thread;     // Set it as the process's current thread

        // Copy the chosen thread's trapframe to the process's active trapframe.
        // This ensures the correct user-level context is loaded when returning to user space.
        *(p->trapframe) = *(next_thread->trapframe);

        return next_thread; // Return the chosen thread
    }

    return 0; // No runnable thread found in this process
}

// Free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
void
freeproc(struct proc *p)
{
  // Free all threads belonging to this process
  for (int i = 0; i < NTHREAD; i++) {
      freethread(&p->threads[i]);
  }

  // Free process's page table
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

  p->current_thread = 0; // Clear current thread pointer
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

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  // This is now handled by init_main_thread for p->current_thread->trapframe.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE; // Process is runnable

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
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
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

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
  // This copies the parent process's trapframe to the child process's trapframe.
  // For multi-threading, this means the main thread's trapframe is copied.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(np->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE; // Process is runnable
  release(&np->lock);

  return pid;
}

// Allocate a new thread within the current process.
// Called by sys_thread.
struct thread *
allocthread(uint64 start_thread, uint64 stack_address, uint64 arg)
{
  struct proc *p = myproc();
  struct thread *t;

  for (t = p->threads; t < p->threads + NTHREAD; t++) {
    acquire(&p->lock);
    if (t->state == THREAD_UNUSED) {
      t->state = THREAD_EMBRYO;
      t->id = alloctid();

      // Allocate and clear trapframe
      if ((t->trapframe = (struct trapframe *)kalloc()) == 0) {
        freethread(t);
        release(&p->lock);
        return 0;
      }
      memset(t->trapframe, 0, PGSIZE);

      // Allocate kernel stack for this thread
      if ((t->kstack = (uint64)kalloc()) == 0) {
          freethread(t);
          release(&p->lock);
          return 0;
      }
      memset((void*)t->kstack, 0, PGSIZE);

      // Initialize context for thread switching
      memset(&t->context, 0, sizeof(t->context));
      t->context.ra = (uint64)forkret; // Thread starts execution at forkret
      t->context.sp = t->kstack + PGSIZE; // Top of thread's kernel stack

      // Set up the trapframe for user-space execution
      t->trapframe->epc = (uint64)start_thread;
      t->trapframe->sp = stack_address;
      t->trapframe->a0 = arg;
      t->trapframe->ra = -1;

      t->state = THREAD_RUNNABLE;
      release(&p->lock);
      return t;
    }
    release(&p->lock);
  }
  return 0; // No unused thread slot found
}

// Exit the current thread.
void
exitthread(void)
{
  struct proc *p = myproc();
  struct thread *cur_thread = p->current_thread;
  uint id = cur_thread->id;

  acquire(&p->lock);

  // Wake up threads that are joining on this thread
  for (struct thread *t = p->threads; t < p->threads + NTHREAD; t++) {
    if (t->state == THREAD_JOINED && t->join == id) {
      t->join = 0;
      t->state = THREAD_RUNNABLE;
      wakeup((void*)t); // Wake up the joining thread
    }
  }

  freethread(cur_thread); // Free resources of the current exiting thread

  // Check if this is the last active thread in the process.
  int active_threads = 0;
  for (struct thread *t = p->threads; t < p->threads + NTHREAD; t++) {
      if (t->state != THREAD_UNUSED && t->id != 0) { // Count other active threads
          active_threads++;
      }
  }

  if (active_threads == 0) {
      // If no other active threads, mark process for killing.
      setkilled(p);
      release(&p->lock);
      // Scheduler will pick up the process in ZOMBIE state.
  } else {
      // If other threads exist, yield CPU to scheduler.
      release(&p->lock);
      yield();
  }
}

// Join a thread (wait for it to exit).
int
jointhread(uint join_id)
{
  struct proc *p = myproc();
  struct thread *cur_thread = p->current_thread;
  struct thread *t;

  acquire(&p->lock);

  for (;;) {
    int target_found_and_active = 0;
    struct thread *target_t = 0;

    // Search for the target thread
    for (t = p->threads; t < p->threads + NTHREAD; t++) {
      if (t->id == join_id) {
        target_t = t;
        target_found_and_active = 1;
        break;
      }
    }

    // Handle cases where target is not found or already exited
    if (!target_found_and_active || target_t->state == THREAD_UNUSED || target_t->state == THREAD_ZOMBIE) {
      release(&p->lock);
      return 0; // Success: target thread already exited or never existed
    }

    // Deadlock Check: Cannot join on self
    if (cur_thread->id == join_id) {
      release(&p->lock);
      return -1; // Deadlock
    }

    // Additional deadlock check: If target_t is joining on cur_thread
    if (target_t->state == THREAD_JOINED && target_t->join == cur_thread->id) {
      release(&p->lock);
      return -1; // Deadlock: Target thread is trying to join on us
    }

    // If target_t is still active, wait for it
    cur_thread->join = join_id;
    cur_thread->state = THREAD_JOINED; // Mark as joining (will be put to sleep)

    // Sleep on the target thread's pointer as a channel.
    sleep((void*)target_t, &p->lock);

    // When woken up, re-acquire the lock and loop to re-check the condition.
    acquire(&p->lock);
  }
}

// Sleep the current thread for 'n' ticks.
void
sleepthread(int n, uint ticks0)
{
  struct proc *p = myproc();
  struct thread *cur_thread = p->current_thread;

  acquire(&p->lock);
  cur_thread->sleep_n = n;
  cur_thread->sleep_tick0 = ticks0;
  cur_thread->state = THREAD_SLEEPING;

  sleep(&ticks, &p->lock); // Sleep on the global ticks variable

  release(&p->lock);
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

// Exit the current process. Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
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

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
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
          freeproc(pp); // Call the modified freeproc
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
    sleep(p, &wait_lock);
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns. It loops, doing:
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
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE){ // Process is runnable
        // Find a runnable thread within this process
        struct thread *t = thread_schd(p);
        if(t != 0){ // If a runnable thread is found
          p->current_thread = t;

          t->state = THREAD_RUNNING;
          p->state = RUNNING;

          c->proc = p;

          swtch(&c->context, &t->context); // Switch to thread's context

          // Thread is done running for now.
          c->proc = 0;
        }
      }
      release(&p->lock);
    }
  }
}

// Switch to scheduler. Must hold only p->lock
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
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  // If a thread is running, save its context and switch to CPU's scheduler context
  if (p->current_thread) {
      swtch(&p->current_thread->context, &mycpu()->context);
  } else {
      // Fallback for processes without a current_thread (e.g., initial state)
      swtch(&p->context, &mycpu()->context); // Original process context switch
  }
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  // If a thread is running, set its state to runnable
  if (p->current_thread) {
      p->current_thread->state = THREAD_RUNNABLE;
  }
  p->state = RUNNABLE; // Process is also runnable
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  struct thread *cur_thread = p->current_thread; // Get current thread for sleep state

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);
  release(lk);

  // Go to sleep.
  // If a thread is sleeping, set its state.
  if (cur_thread) {
      cur_thread->chan = chan; // Assign channel to thread
      cur_thread->state = THREAD_SLEEPING;
  }
  p->chan = chan; // Also set process chan for process-level sleeps
  p->state = SLEEPING; // Process state is sleeping

  sched();

  // Tidy up.
  if (cur_thread) {
      cur_thread->chan = 0;
  }
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes/threads sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;
  struct thread *t;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock); // Acquire process lock for consistency

    // Check if process itself is sleeping on this channel
    if(p->state == SLEEPING && p->chan == chan) {
      p->state = RUNNABLE;
    }

    // Check if any thread in this process is sleeping on this channel
    for (t = p->threads; t < p->threads + NTHREAD; t++) {
        if (t->state == THREAD_SLEEPING && t->chan == chan) {
            t->state = THREAD_RUNNABLE;
        }
    }
    release(&p->lock);
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      // Also wake up any sleeping threads within this process
      for (struct thread *t = p->threads; t < p->threads + NTHREAD; t++) {
          if (t->state == THREAD_SLEEPING) {
              t->state = THREAD_RUNNABLE;
          }
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

// Print a process listing to console. For debugging.
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
  static char *thread_states[] = {
  [THREAD_UNUSED]    "T_unused",
  [THREAD_EMBRYO]    "T_embryo",
  [THREAD_RUNNABLE]  "T_runble",
  [THREAD_RUNNING]   "T_run   ",
  [THREAD_JOINED]    "T_joined",
  [THREAD_SLEEPING]  "T_sleep ",
  [THREAD_ZOMBIE]    "T_zombie"
  };
  struct proc *p;
  struct thread *t;
  char *p_state, *t_state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      p_state = states[p->state];
    else
      p_state = "???";
    printf("%d %s %s", p->pid, p_state, p->name);

    // Print thread info for this process
    for (t = p->threads; t < p->threads + NTHREAD; t++) {
        if (t->state == THREAD_UNUSED)
            continue;
        if (t->state >= 0 && t->state < NELEM(thread_states) && thread_states[t->state])
            t_state = thread_states[t->state];
        else
            t_state = "???";
        printf(" (TID %lu %s)", t->id, t_state);
    }
    printf("\n");
  }
}
