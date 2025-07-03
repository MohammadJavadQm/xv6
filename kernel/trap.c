#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;
  struct proc *p = myproc();
  struct thread *t = p->current_thread; // Get the current thread

  // Check if trap is from user mode.
  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // Ensure process and thread are in a valid state.
  if(p->state == UNUSED)
    panic("usertrap: p->state UNUSED");
  if(t->state == THREAD_UNUSED) // Check thread state too
    panic("usertrap: t->state UNUSED");

  // Set up trapframe for kernel.
  // The trapframe is where saved user registers are stored.
  // p->trapframe is already set to t->trapframe in scheduler.
  p->trapframe->epc = r_sepc();

  // Send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  uint64 scause = r_scause();

  if(scause == 8){
    // System call
    if(killed(p))
      exit(-1); // If process is killed, exit

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall(); // Call the syscall handler
  } else if((which_dev = devintr()) != 0){
    // Device interrupt
    // ok
  }
  // NEW LOGIC FOR THREAD-SPECIFIC TRAPS:
  else if (p->current_thread && p->current_thread->id != p->pid) {
    // This condition checks if the current context is a thread (not the main process thread)
    // and an unexpected trap occurred.
    // This means it's a thread-specific trap (e.g., page fault, illegal instruction)
    // that should only terminate the thread, not the whole process.
    // The specific check `r_sepc() != r_stval() || r_scause() != 0xc` from the slide
    // is a common way to filter for certain types of unexpected traps.
    // However, if any non-syscall/non-device trap occurs in a non-main thread,
    // we generally want to terminate that thread.
    // Let's use the simpler condition from the slide's "else if" block.
    printf("usertrap(): thread unexpected scause 0x%lx pid=%d tid=%lu\n",r_scause(), p->pid, p->current_thread->id);
    printf("             sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    exitthread(); // Terminate only the current thread
  }
  // END NEW LOGIC
  else {
    // Unknown trap or process-level trap (e.g., main process thread trap)
    // This is the existing logic for handling process-level traps.
    // It might kill the whole process.
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("             sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p); // Mark process for killing
  }

  // Check if process/thread was killed during trap handling or by syscall
  if(killed(p)) {
    // If the process is killed, and this is the main thread, or the last thread,
    // then exit the process.
    // exitthread() already handles if it's the last thread.
    // If it's the main thread and killed, it should exit.
    // The original XV6 `exit(-1)` here means the whole process exits.
    // We need to ensure `exitthread()` is called for threads, and `exit()` for processes.
    // The `exitthread()` call in the `else if` block above handles thread-specific kills.
    // This `if(killed(p)) exit(-1);` should apply to the process if it's killed.
    // If `p->current_thread->id == p->pid` (main thread) and killed, then `exit(-1)`.
    // Otherwise, if it's a non-main thread and killed, it should call `exitthread()`.
    // The current structure implies that if a non-main thread gets an unexpected trap,
    // it calls `exitthread()`. If the *process* is killed (e.g., via `kill` syscall),
    // then this `if(killed(p))` block is for the process.

    // Let's refine this based on the original structure and new thread logic.
    // If the process is marked killed, and we are in the main thread context, exit.
    // If we are in a non-main thread context and it's killed, exitthread() would have been called.
    if (p->current_thread->id == p->pid) { // If it's the main thread
        exit(-1); // Exit the process
    } else {
        // If it's a non-main thread and process is killed,
        // this thread should also exit.
        exitthread(); // This will handle yielding or process exit if it's the last thread.
    }
  }


  // Give up the CPU if this is a timer interrupt.
  if(which_dev == 2) {
    // If a thread is running, yield its CPU time.
    // The `yield()` function now correctly sets `p->current_thread->state = THREAD_RUNNABLE`.
    yield();
  }

  usertrapret(); // Return to user space or scheduler
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    // interrupt or trap from an unknown source
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if(cpuid() == 0){
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 1000000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if(scause == 0x8000000000000009L){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

