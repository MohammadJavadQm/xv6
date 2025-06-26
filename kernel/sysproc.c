#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "custom_logger.h"



uint64
sys_trigger(void) {
    log_message(INFO, "This is a log to test a new xv6 system call");
    return 0;
}


uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// kernel/sysproc.c

// ... (rest of the file, after sys_uptime function) ...


// Implementation of the thread system call
// Allocates a new thread in the current process
uint64
sys_thread(void)
{
  uint64 start_thread, stack_address, arg;

  // Retrieve arguments from user space
  // argaddr() helps fetch argument values from the user's registers/stack
  argaddr(0, &start_thread);    // Function pointer for the new thread's entry point
  argaddr(1, &stack_address);   // Base address of the new thread's stack
  argaddr(2, &arg);             // Argument to pass to the thread function

  // Call the kernel function to allocate and initialize a new thread
  // This function (allocthread) will be implemented later.
  struct thread *t = allocthread(start_thread, stack_address, arg);

  // Return the new thread's ID, or 0 if allocation failed
  return t ? t->id : 0;
}

// Implementation of the jointhread system call
// Allows the current thread to wait for a specific thread to terminate
uint64
sys_jointhread(void)
{
  int id;

  // Retrieve the thread ID to join with from user space
  argint(0, &id);

  // Call the kernel function to perform the join operation
  // This function (jointhread) will be implemented later.
  return jointhread(id);
}
