 #include "types.h"
    #include "param.h"
    #include "memlayout.h"
    #include "riscv.h"
    #include "spinlock.h"
    #include "proc.h"
    #include "defs.h"
    #include "custom_logger.h" // Assuming this is part of your previous phase

    // Forward declarations (if needed for functions defined later in the file)
    static void freeproc(struct proc *p);
    struct thread *initthread(struct proc *p);
    void freethread(struct thread *t);
    struct thread *allocthread(uint64 start_thread, uint64 stack_address, uint64 arg);
    void exitthread();
    int jointhread(uint id);
    void sleepthread(int n, uint ticks0);
    int thread_schd(struct proc *p); // Declaration for thread_schd

    struct cpu cpus[NCPU];

    struct proc proc[NPROC];

    struct proc *initproc;

    int nextpid = 1;
    struct spinlock pid_lock;

    extern void forkret(void);
    // static void freeproc(struct proc *p); // Already declared above

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

      for(p = proc; p < &proc[NPROC]; p++) {
        initlock(&p->lock, "proc");
        p->state = UNUSED;
        // Changed kstack assignment for xv6 RISC-V: each proc has its own kstack
        p->kstack = KSTACK((int) (p - proc)); 
        p->current_thread = 0; // Initialize current_thread to indicate no active thread for the process
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
      push_off(); // Ensure interrupts are off for mycpu, as it modifies cpu struct
      int id = cpuid();
      struct cpu *c = &cpus[id];
      pop_off();
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
        freeproc(p); // Use the modified freeproc here
        release(&p->lock);
        return 0;
      }

      // An empty user page table.
      p->pagetable = proc_pagetable(p);
      if(p->pagetable == 0){
        freeproc(p); // Use the modified freeproc here
        release(&p->lock);
        return 0;
      }

      // Set up new context to start executing at forkret,
      // which returns to user space.
      memset(&p->context, 0, sizeof(p->context));
      p->context.ra = (uint64)forkret;
      p->context.sp = p->kstack + PGSIZE;

      // Initialize the main thread for the new process
      // This ensures p->current_thread is set correctly for subsequent thread operations
      if (!initthread(p)) { // Call initthread to set up the process's main thread
          freeproc(p);
          release(&p->lock);
          return 0;
      }

      return p;
    }

    // Frees a thread's resources and resets its state to UNUSED.
    void
    freethread(struct thread *t)
    {
      t->state = THREAD_UNUSED;
      if (t->trapframe) {
        kfree((void*)t->trapframe); // Free the trapframe memory
      }
      t->trapframe = 0;
      t->id = 0;
      t->join = 0;
      t->sleep_n = 0;
      t->sleep_tick0 = 0;
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
      p->current_thread = 0; // Reset current_thread to null

      // Free all threads associated with the process
      for (int i = 0; i < NTHREAD; ++i) {
        freethread(&p->threads[i]); // Call freethread for each thread
      }
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
      // The main thread's trapframe is already set up in allocproc via initthread
      p->trapframe->epc = 0;       // user program counter
      p->trapframe->sp = PGSIZE;   // user stack pointer

      safestrcpy(p->name, "initcode", sizeof(p->name));
      p->cwd = namei("/");

      p->state = RUNNABLE;

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
      np->state = RUNNABLE;
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
        // processes are waiting.
        intr_on();

        int found = 0;
        for(p = proc; p < &proc[NPROC]; p++) {
          acquire(&p->lock);
          if(p->state == RUNNABLE) {
            // Before switching to the process, try to schedule a thread within it
            if (thread_schd(p)) { // If thread_schd finds a runnable thread
              // Switch to chosen process. It is the process's job
              // to release its lock and then reacquire it
              // before jumping back to us.
              p->state = RUNNING;
              c->proc = p;
              swtch(&c->context, &p->context);

              // Process is done running for now.
              // It should have changed its p->state before coming back.
              c->proc = 0;
              found = 1;
            }
          }
          release(&p->lock);
        }

        if(found == 0){
          // nothing to run; stop running on this core until an interrupt.
          intr_on();
          asm volatile("wfi");
        }
      }
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
        panic("sched running");
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
      p->state = RUNNABLE;
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
      
      // Must acquire p->lock in order to
      // change p->state and then call sched.
      // Once we hold p->lock, we can be
      // guaranteed that we won't miss any wakeup
      // (wakeup locks p->lock),
      // so it's okay to release lk.

      acquire(&p->lock);  //DOC: sleeplock1
      release(lk);

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

    // Wake up all processes sleeping on chan.
    // Must be called without any p->lock.
    void
    wakeup(void *chan)
    {
      struct proc *p;

      for(p = proc; p < &proc[NPROC]; p++) {
        if(p != myproc()){
          acquire(&p->lock);
          if(p->state == SLEEPING && p->chan == chan) {
            p->state = RUNNABLE;
          }
          release(&p->lock);
        }
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
        printf("\n");
      }
    }


    // Initializes a process's threads. If no current_thread, it sets up the first thread.
    // Returns 0 on failure, pointer to current_thread on success.
    struct thread *
    initthread(struct proc *p)
    {
      if (!p->current_thread) { // If no current thread is set for the process
        for (int i = 0; i < NTHREAD; ++i) {
          // Clear any old trapframe pointers and free old threads
          p->threads[i].trapframe = 0;
          freethread(&p->threads[i]); // Ensure the thread is in a clean state
        }

        // Initialize the main thread (first thread in the array)
        struct thread *t = &p->threads[0];
        t->id = p->pid; // Main thread ID matches process ID

        if ((t->trapframe = (struct trapframe *)kalloc()) == 0) {
          freethread(t); // Free if trapframe allocation fails
          return 0;
        }

        t->state = THREAD_RUNNING; // Main thread starts as running
        p->current_thread = t;      // Set as the current thread for the process
      }
      return p->current_thread; // Return the initialized current thread
    }


    // Frees a thread's resources and resets its state to UNUSED.
    void
    freethread(struct thread *t)
    {
      t->state = THREAD_UNUSED;
      if (t->trapframe) {
        kfree((void*)t->trapframe); // Free the trapframe memory
      }
      t->trapframe = 0;
      t->id = 0;
      t->join = 0;
      t->sleep_n = 0;
      t->sleep_tick0 = 0;
    }


    // Allocates a new thread in the current process.
    // start_thread: function pointer for the new thread's entry point.
    // stack_address: base address for the new thread's stack.
    // arg: argument to pass to the thread function.
    struct thread *
    allocthread(uint64 start_thread, uint64 stack_address, uint64 arg)
    {
      struct proc *p = myproc(); // Get the current process

      // Ensure the process has been initialized for threading
      if (!initthread(p)) {
        return 0; // Failed to initialize or acquire resources
      }

      // Find an unused thread slot in the process's thread array
      for (struct thread *t = p->threads; t < p->threads + NTHREAD; t++) {
        if (t->state == THREAD_UNUSED) { // Found an unused slot
          t->id = allocpid(); // Assign a unique ID to the thread (using proc's allocpid)

          // Allocate memory for the thread's trapframe
          if ((t->trapframe = (struct trapframe *)kalloc()) == 0) {
            freethread(t); // Clean up if allocation fails
            return 0;
          }

          // Copy the current process's trapframe (important for initial state)
          *t->trapframe = *p->trapframe;

          // Set up the stack pointer for the new thread
          t->trapframe->sp = stack_address;

          // Set the argument for the new thread
          t->trapframe->a0 = arg;

          // Set return address to a special value (or -1) indicating thread end
          t->trapframe->ra = (uint64)exitthread; // When thread returns, it will call exitthread

          // Set the program counter to the start function of the new thread
          t->trapframe->epc = (uint64)start_thread;

          t->state = THREAD_RUNNABLE; // Thread is now ready to run
          return t; // Return the newly allocated thread
        }
      }
      return 0; // No unused thread slots found
    }

    // Exits the current thread. Frees its resources and wakes up joining threads.
    void
    exitthread()
    {
      struct proc *p = myproc();
      struct thread *t_exiting = p->current_thread;
      uint id_exiting = t_exiting->id;

      // Wake up any threads that are waiting to join with this thread
      for (struct thread *t = p->threads; t < p->threads + NTHREAD; t++) {
        if (t->state == THREAD_JOINED && t->join == id_exiting) {
          t->join = 0; // Clear join ID
          t->state = THREAD_RUNNABLE; // Make the joining thread runnable
        }
      }

      // Free the resources of the exiting thread
      freethread(t_exiting);

      // Check if there are other runnable threads in the same process
      // If not, the process itself might need to be killed if no other threads exist.
      // This function will attempt to schedule another thread within the same process.
      // If no other thread can be scheduled, it means the process has no active threads left.
      if (!thread_schd(p)) {
        setkilled(p); // If no runnable thread is found, kill the parent process.
                      // This ensures the process exits if all its threads are done.
      }
      sched(); // Yield control to the scheduler to pick a new thread/process
    }

    // Allows the current thread to wait for a specific thread to terminate.
    // join_id: ID of the thread to wait for.
    int
    jointhread(uint join_id)
    {
      struct proc *p = myproc();
      struct thread *current_t = p->current_thread;

      if (!current_t) {
        return -3; // No current thread (should not happen)
      }

      // Check for deadlock: current thread trying to join itself
      if (current_t->id == join_id) {
        return -1; // Deadlock: cannot join self
      }

      // Check for circular join (potential deadlock)
      // Iterate through the join chain to detect if current_t is in the chain
      uint target_id_in_chain = join_id;
      while (target_id_in_chain != 0) {
          if (target_id_in_chain == current_t->id) {
              return -1; // Deadlock detected in join chain
          }
          // Find the thread in the process with target_id_in_chain
          int found_in_chain = 0;
          for (int i = 0; i < NTHREAD; i++) {
              if (p->threads[i].id == target_id_in_chain) {
                  target_id_in_chain = p->threads[i].join; // Move to the next join target in the chain
                  found_in_chain = 1;
                  break;
              }
          }
          if (!found_in_chain) { // If a thread in the chain is not found, break
              target_id_in_chain = 0;
          }
      }

      // Find the target thread in the current process
      struct thread *target_t = 0;
      for (int i = 0; i < NTHREAD; i++) {
        if (p->threads[i].id == join_id) {
          target_t = &p->threads[i];
          break;
        }
      }

      if (!target_t || target_t->state == THREAD_UNUSED) {
        return -2; // Target thread not found or already unused
      }

      // Set the current thread to wait for the target thread
      current_t->join = join_id;
      current_t->state = THREAD_JOINED;

      // Yield control to the scheduler. The current thread will sleep until target_t exits.
      yield();

      // After waking up, clear the join ID and return.
      current_t->join = 0; // Clear the join ID once woke up.
      return 0; // Success
    }

    // Puts the current thread to sleep for 'n' ticks.
    void
    sleepthread(int n, uint ticks0)
    {
      struct thread *t = myproc()->current_thread;
      if (!t) return; // Should not happen for a running thread

      acquire(&tickslock); // Protect access to ticks
      t->sleep_n = n;
      t->sleep_tick0 = ticks0;
      t->state = THREAD_SLEEPING;
      release(&tickslock); // Release lock before scheduling

      // Yield control. The thread will wake up after 'n' ticks
      // or if explicitly woken up.
      sched();
    }


    // Schedules the next runnable thread within the current process.
    // Returns 1 if a thread was scheduled, 0 otherwise.
    int
    thread_schd(struct proc *p) {
        if (!p->current_thread) {
            return 1; // Process has no current thread, it means it's newly initialized or has no threads.
                      // We can return 1 and let proc's scheduler pick a runnable process
        }

        // If the current thread is running, mark it runnable for now,
        // unless it's explicitly sleeping or joined.
        if (p->current_thread->state == THREAD_RUNNING) {
            p->current_thread->state = THREAD_RUNNABLE;
        }

        acquire(&tickslock); // Acquire lock for ticks variable
        uint current_ticks = ticks;
        release(&tickslock);

        struct thread *next_thread = 0;
        // Start searching from the thread after current_thread to ensure fairness
        struct thread *t = p->current_thread + 1;

        for (int i = 0; i < NTHREAD; i++, t++) {
            // Wrap around the thread array if we reach the end
            if (t >= p->threads + NTHREAD) {
                t = p->threads;
            }

            // Check for runnable threads
            if (t->state == THREAD_RUNNABLE) {
                next_thread = t;
                break;
            }
            // Check for sleeping threads that have timed out
            else if (t->state == THREAD_SLEEPING) {
                acquire(&tickslock);
                uint ticks_now = ticks;
                release(&tickslock);
                // Check if enough ticks have passed for the thread to wake up
                if (ticks_now - t->sleep_tick0 >= t->sleep_n) {
                    next_thread = t;
                    t->sleep_n = 0; // Reset sleep counter
                    t->sleep_tick0 = 0;
                    break;
                }
            }
        }

        if (next_thread == 0) {
            return 0; // No runnable or woke-up sleeping thread found in this process
        } else if (p->current_thread != next_thread) { // If a different thread is selected
            next_thread->state = THREAD_RUNNING; // Mark the new thread as running

            // Save current trapframe to old thread's trapframe (if old thread exists)
            // This is crucial for switching from one thread to another within the same process.
            if (p->current_thread && p->current_thread->trapframe) {
                *p->current_thread->trapframe = *p->trapframe;
            }

            p->current_thread = next_thread; // Update the process's current thread pointer
            *p->trapframe = *next_thread->trapframe; // Load new thread's trapframe into process's trapframe
        }
        return 1; // A thread was scheduled
    }
