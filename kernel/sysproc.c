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

uint64
sys_thread(void)
{
  uint64 start_thread, stack_address, arg;

  // Read arguments from user space registers
  argaddr(0, &start_thread); // NO if condition here
  argaddr(1, &stack_address); // NO if condition here
  argaddr(2, &arg);         // NO if condition here

  // Call allocthread (we'll implement this next) to create and prepare the thread
  struct thread *t = allocthread(start_thread, stack_address, arg);

  // Return the thread ID (tid) if successful, -1 otherwise
  return t ? t->id : -1;
}

uint64
sys_jointhread(void)
{
  int id; // Thread ID

  // Read the thread ID argument
  argint(0, &id); // NO if condition here

  // Call jointhread (we'll implement this next) to wait for the specified thread
  return jointhread(id);
}
