#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  
  // struct proc* curr_proc;
  // curr_proc=myproc();
  // int temp=(1<<SYS_exit)&(curr_proc->trace_mask);
  // if(temp)
  //   printf("%d: syscall exit (%d) -> 0\n", curr_proc->pid, n);
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
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_waitx(void)
{
  uint64 addr, addr1, addr2;
  uint wtime, rtime;
  if(argaddr(0, &addr) < 0)
    return -1;
  if(argaddr(1, &addr1) < 0) // user virtual memory
    return -1;
  if(argaddr(2, &addr2) < 0)
    return -1;
  int ret = waitx(addr, &wtime, &rtime);
  struct proc* p = myproc();
  if (copyout(p->pagetable, addr1,(char*)&wtime, sizeof(int)) < 0)
    return -1;
  if (copyout(p->pagetable, addr2,(char*)&rtime, sizeof(int)) < 0)
    return -1;
  return ret;
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
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

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
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

  if(argint(0, &pid) < 0)
    return -1;
  
  // struct proc* curr_proc;
  // curr_proc=myproc();
  // int temp=(1<<SYS_exit)&(curr_proc->trace_mask);
  // if(temp)
  //   printf("%d: syscall kill (%d) -> 0\n", curr_proc->pid, pid);
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
sys_trace(void)
{
  int trace_mask=0;

  // Getting 0th argument to syscall trace as an int
  if(argint(0, &trace_mask) < 0)
    return -1;
  
  int return_val = trace(trace_mask);
  return return_val;
}

uint64
sys_set_priority(void)
{
  int static_priority=60;
  int pid=0;

  if(argint(0, &static_priority) < 0)
    return -1;
  if(static_priority<0 || static_priority>100)
    return -1;
  if(argint(1, &pid) < 0)
    return -1;
  int return_val=set_priority(static_priority, pid);
  if(return_val<0)
  return -1;
  return static_priority;
}