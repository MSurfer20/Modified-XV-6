#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#ifndef SCHEDULER
#define SCHEDULER 0
#endif

struct MLFQ_Queue mlfq_queue[NUM_OF_QUEUES];

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

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
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  #if SCHEDULER==3
  int i;
  for(i=0;i<NUM_OF_QUEUES;i++)
    mlfq_queue[i].arr[0]=0,mlfq_queue[i].num_procs=0;
  #endif
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
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
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
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
  p->trace_mask = 0;
  p->ctime=ticks;
  p->rtime=0;
  p->pbs_rtime=0;
  p->stime=0;
  p->etime=0;
  p->niceness=5;
  p->static_priority=60;

  p->scheduled_count=0;
  p->curr_queue=0;
  for(int x=0;x<NUM_OF_QUEUES;x++)
    p->time_spent_queues[x]=0;
  p->qwtime=0;
  p->qrtime=0;
  p->overshot_flag=0;
  // #if SCHEDULER==3
  //   add_into_mlfq(0, p);
  // #endif

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
  p->trace_mask=0;
  p->ctime=0;
  p->niceness=0;
  p->scheduled_count=0;
  p->static_priority=0;
  for(int x=0;x<NUM_OF_QUEUES;x++)
    p->time_spent_queues[x]=0;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
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

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
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
// od -t xC initcode
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
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;


  release(&p->lock);
  #if SCHEDULER==3
    add_into_mlfq(0, p);
  #endif
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
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

  np->trace_mask=p->trace_mask;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  //TODO: Insert the process into the 0th queue
  release(&np->lock);
  #if SCHEDULER==3
    add_into_mlfq(0, np);
    if(p!=0 && p->curr_queue>0)
      yield();
  #endif

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
  p->etime=ticks;

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
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}


// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
waitx(uint64 addr, uint* rtime, uint* wtime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          *rtime = np->rtime;
          *wtime = np->etime - np->ctime - np->rtime;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

void
update_time()
{
  struct proc* p;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == RUNNING) {
      p->rtime++;
      p->pbs_rtime++;
    }
    else if(p->state == SLEEPING)
    {
      p->stime++;
    }
    //TODO: Update the number of ticks spent in this queue
    release(&p->lock); 
  }
}

void
set_overshot_proc()
{
  struct proc* p = myproc();
  acquire(&p->lock);
  p->overshot_flag=1;
  p->qrtime=0;
  p->qwtime=0;
  release(&p->lock);
}

void
update_q_wtime()
{
  struct proc* p;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == RUNNING) {
      p->qrtime++;
    }
    else if(p->state == RUNNABLE)
    {
      p->qwtime++;
    }
    if(p->state != ZOMBIE)
    {
      p->time_spent_queues[p->curr_queue]++;
    }
    release(&p->lock); 
  }
}

void
remove_from_mlfq(int queue_no, int proc_idx)
{
  for(int x=proc_idx;x<mlfq_queue[queue_no].num_procs-1;x++)
  {
    mlfq_queue[queue_no].arr[x]=mlfq_queue[queue_no].arr[x+1];
  }
  mlfq_queue[queue_no].num_procs--;
}

void
add_into_mlfq(int queue_no, struct proc* p)
{
  mlfq_queue[queue_no].arr[mlfq_queue[queue_no].num_procs]=p;
  mlfq_queue[queue_no].num_procs++;
  p->qrtime=0;
  p->qwtime=0;
  p->curr_queue=queue_no;
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
  #if SCHEDULER == 0
  printf("ROUND ROBIN\n");
  #endif
  #if SCHEDULER == 1
  printf("FCFS\n");
  #endif
  #if SCHEDULER == 2
  printf("PBS\n");
  #endif
  #if SCHEDULER == 3
  printf("MLFQ\n");
  #endif
  #if SCHEDULER == 0
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        p->scheduled_count++;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
  #endif

  #if SCHEDULER == 1
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    struct proc* lowest_time_proc = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        if(lowest_time_proc==0)
        lowest_time_proc=p;
        else if(p->ctime < lowest_time_proc->ctime)
        lowest_time_proc=p;
      }
      release(&p->lock);
    }
    if(lowest_time_proc==0)
    continue;
    acquire(&lowest_time_proc->lock);
      if(lowest_time_proc->state == RUNNABLE) {
        // printf("RUNNING PROC %d\n", (int)lowest_time_proc->pid);
        lowest_time_proc->scheduled_count++;
        lowest_time_proc->state = RUNNING;
        c->proc = lowest_time_proc;
        // printf("PID: %d CPU: %d START TIME: %d\n", lowest_time_proc->pid, cpuid(), lowest_time_proc->ctime);
        swtch(&c->context, &lowest_time_proc->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&lowest_time_proc->lock);
  }
  #endif

  #if SCHEDULER==2
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    struct proc* highest_priority_proc = 0;
    int highest_priority=110000;
    for(p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if(p->state == RUNNABLE)
      {
        int static_priority=p->static_priority;
        int niceness=p->niceness;
        int priority=static_priority-niceness+5;
        // if(p->pid==8)
        // {
        //   printf("%d %d %d\n", static_priority, niceness, priority);
        // }
        if(priority<0)
          priority=0;
        else if(priority>100)
          priority=100;
        if(highest_priority_proc==0)
        {
          highest_priority_proc = p;
          highest_priority = priority;
        }
        else if(priority < highest_priority)
        {
          release(&highest_priority_proc->lock);
          highest_priority_proc = p;
          highest_priority = priority;
        }
        else if(priority==highest_priority)
        {
          if(p->scheduled_count < highest_priority_proc->scheduled_count)
          {
            release(&highest_priority_proc->lock);
            highest_priority_proc=p;
            highest_priority=priority;
          }
          else if(p->scheduled_count==highest_priority_proc->scheduled_count && p->ctime < highest_priority_proc->ctime)
          {
            release(&highest_priority_proc->lock);
            highest_priority_proc = p;
            highest_priority = priority;
          }
          else
            release(&p->lock);
        }
        else release(&p->lock);
      }
      else
        release(&p->lock);
    }
    if(highest_priority_proc==0)
      continue;
    if(highest_priority_proc->state == RUNNABLE) 
    {
      highest_priority_proc->state = RUNNING;
      c->proc = highest_priority_proc;
      highest_priority_proc->scheduled_count+=1;
      swtch(&c->context, &highest_priority_proc->context);

      c->proc = 0;
      if((highest_priority_proc->stime)+(highest_priority_proc->pbs_rtime) > 0)
      {
        int sum_of_val=(highest_priority_proc->stime)+(highest_priority_proc->pbs_rtime);
        int sleep_time=highest_priority_proc->stime;
        sleep_time=10*sleep_time;
        if(sum_of_val != 0)
        {
          int new_niceness;
          new_niceness = (sleep_time)/(sum_of_val);
          highest_priority_proc->niceness=new_niceness;
        }
      }
    }
    release(&highest_priority_proc->lock);
  }
  #endif

  #if SCHEDULER==3
  struct cpu *c = mycpu();
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    struct proc* proc_to_execute=0;

    //TODO: Add ageing

    for(int x=1;x<NUM_OF_QUEUES;x++)
    {
      // int queue_size=mlfq_queue[x].num_procs;
      for(int y=0;y<mlfq_queue[x].num_procs;y++)
      {
        struct proc* ageing_proc;
        ageing_proc=mlfq_queue[x].arr[y];
        if(ageing_proc->qwtime>MAX_OLD_AGE && x>0)
        {
          // printf("AGEEEEEEEEEEEEEEEE");
          remove_from_mlfq(x, y);
          add_into_mlfq(x-1, ageing_proc);
          y--;
        }
      }
    }


    for(int x=0;x<NUM_OF_QUEUES;x++)
    {
      // printf("%d\n", NUM_OF_QUEUES);
      if(mlfq_queue[x].num_procs==0)
        continue;
      
      for(int y=0;y<mlfq_queue[x].num_procs;y++)
      {
        acquire(&mlfq_queue[x].arr[y]->lock);
        struct proc* temp_proc;
        temp_proc=mlfq_queue[x].arr[y];
        if(temp_proc->state!=RUNNABLE)
        {
          release(&temp_proc->lock);
          continue;
        }
        proc_to_execute=temp_proc;
        remove_from_mlfq(x,y);
        break;
      }
      if(proc_to_execute!=0)
        break;
    }
    if(proc_to_execute==0)
    continue;

    // printf("AAAAAAAAAAAAAAAAa");

    if(proc_to_execute->state!=RUNNABLE)
    {
      release(&proc_to_execute->lock);
      continue;
    }

    // printf("AAAAAAAAAAAAAAAAa");
    // printf("%d", proc_to_execute->pid);
    proc_to_execute->qwtime=0;
    proc_to_execute->state = RUNNING;
    c->proc = proc_to_execute;
    // printf("AAAAAAAAAAAAAAAAa");
    swtch(&c->context, &proc_to_execute->context);

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    // printf("AAAAAAAAAAAAAAAAa");
    c->proc = 0;
    if(proc_to_execute->state==RUNNABLE)
    {
      if(proc_to_execute->overshot_flag==1)
      {
        if(proc_to_execute->curr_queue != NUM_OF_QUEUES-1)
          proc_to_execute->curr_queue++;
        proc_to_execute->overshot_flag=0;
      }
      add_into_mlfq(proc_to_execute->curr_queue, proc_to_execute);
    }
    release(&proc_to_execute->lock);
    // printf("BBBBBBBBBBBBBBBBBBBBBBBB\n");
  }
  #endif

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

  //TODO: Insert into current queue
  // #if SCHEDULER==3
  //   mlfq_queue[p->curr_queue].arr[mlfq_queue[p->curr_queue].num_procs]=p;
  //   mlfq_queue[p->curr_queue].num_procs++;
  // #endif  
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
    first = 0;
    fsinit(ROOTDEV);
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

//----------------------------------
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);

//-------------------------
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
        #if SCHEDULER==3
          add_into_mlfq(p->curr_queue, p);
        #endif
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
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;
  int pid,rtime,wtime,nrun;
  printf("PID\t");
  #if SCHEDULER==2 || SCHEDULER==3
  printf("Priority\t");
  #endif
  printf("State\trtime\twtime\tnrun");
  #if SCHEDULER==3
  printf("\tq0\tq1\tq2\tq3\tq4");
  #endif
  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    pid=p->pid;
    printf("%d\t", pid);
    #if SCHEDULER==2
      int static_priority=p->static_priority;
      int niceness=p->niceness;
      int priority=static_priority-niceness+5;
      printf("%d\t\t", priority);
    #elif SCHEDULER==3
      int priority=p->curr_queue;
      if(p->state==ZOMBIE)
        priority = -1;
      printf("%d\t\t", priority);
    #endif
    printf("%s\t", state);
    #if SCHEDULER!=3
    wtime = ticks - p->ctime - p->rtime;
    #else
    wtime=p->qwtime;
    #endif
    rtime=p->rtime;
    nrun=p->scheduled_count;
    printf("%d\t%d\t%d\t", rtime, wtime, nrun);
    #if SCHEDULER==3
    for(int x=0;x<NUM_OF_QUEUES;x++)
    printf("%d\t", p->time_spent_queues[x]);
    #endif
    // printf("%d", p->ctime);
    printf("\n");
  }
}

int
trace(int trace_mask)
{
  struct proc* pr;
  pr=myproc();
  pr->trace_mask=trace_mask;
  return 0;
}

int
set_priority(int static_priority, int pid)
{
  struct proc *p;
  int old_static_priority=110, old_dynamic_priority;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      old_dynamic_priority = p->static_priority - p->niceness + 5;
      old_static_priority=p->static_priority;
      p->static_priority=static_priority;
      p->niceness=5;
      p->stime=0;
      p->pbs_rtime=0;
      release(&p->lock);
      if(static_priority<old_dynamic_priority)
        {
          // printf("RESCHEDUYLEEEEEEEEEEEEEEEEE");
          yield();
        }
      return old_static_priority;
    }
    release(&p->lock);
  }
  return -1;
}