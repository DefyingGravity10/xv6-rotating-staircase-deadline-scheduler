#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

int levelOrder[RSDL_LEVELS];
int activeLevel = 0;

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
  struct level l[RSDL_LEVELS]; 
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  // Initialize active and expired sets of all levels

  for (int i = 0; i < RSDL_LEVELS; i++)
  {
    ptable.l[i].activeSet = 0;
    ptable.l[i].s[0].queueIndex = -1; // Index being -1 implies that there is nothing within the queue
    ptable.l[i].s[1].queueIndex = -1;
  }

  int index = 0;
  for (int j = RSDL_STARTING_LEVEL; j < RSDL_LEVELS; j++)
  {
    levelOrder[index] = j;

    index = index + 1;
  }
  for (int k = 0; k < RSDL_STARTING_LEVEL; k++)
  {
    levelOrder[index] = k;
    index = index + 1;
  }
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  // Add init into the active queue of starting level
  int active = ptable.l[RSDL_STARTING_LEVEL].activeSet;
  ptable.l[RSDL_STARTING_LEVEL].s[active].queueIndex++;
  ptable.l[RSDL_STARTING_LEVEL].s[active].queue[ptable.l[RSDL_STARTING_LEVEL].s[active].queueIndex] = p;
  p->inQueue = 1;
  
  p->state = RUNNABLE;
  p->ticks_left = RSDL_PROC_QUANTUM; //quantum replenished when enqueued
  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  // Add newly made processes into active queue of active level
  int isEnqueued = 0;
  for (int i = 0; i < RSDL_LEVELS; i++)
  {
    int curLevel = levelOrder[i];
    int active = ptable.l[curLevel].activeSet;

    // change 4 to 63 (1 less than max number of procs in queue)
    if (ptable.l[curLevel].s[active].queueIndex >= 4)
    {
      // level is full; enqueue in next level
      continue;
    }

    ptable.l[curLevel].s[active].queueIndex++;
    ptable.l[curLevel].s[active].queue[ptable.l[curLevel].s[active].queueIndex] = np;
    isEnqueued = 1;
    np->inQueue = 1;

    np->state = RUNNABLE;
    np->ticks_left = RSDL_PROC_QUANTUM; // quantum replenished when enqueued
    break;
  }
  release(&ptable.lock);
  if (isEnqueued == 0)
  {
    panic("all levels are full!"); //not sure if this is needed??
  }

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  wakeup1(curproc->parent);
  /*
  // Parent might be sleeping in wait().
  int hasChildren = 0;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if (p != curproc && p->parent == curproc->parent && p->state != ZOMBIE) {
      hasChildren = 1;
    }
  }
  if (hasChildren == 0) {
    wakeup1(curproc->parent);
  }
*/
  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

int schedlog_active = 0;
int schedlog_lasttick = 0;

void schedlog(int n) {
  schedlog_active = 1;
  schedlog_lasttick = ticks + n;
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    // There should be one more outer for loop but we omit it for now
    int swap = 1;
    // Loop through all the items within the active queue of active level
    int l;
    for (l = 0; l < RSDL_LEVELS; l++)
    {
      int curLevel = levelOrder[l];
      int i;
      int active = ptable.l[curLevel].activeSet;
      int noRunnableProcInLevel = 1;
      for (i = 0; i <= ptable.l[curLevel].s[active].queueIndex; i++)
      {
        p = ptable.l[curLevel].s[active].queue[i];

        if (p->state != RUNNABLE)
        {
          continue;
        }

        // There is a runnable proc
        noRunnableProcInLevel = 0;
        activeLevel = curLevel;

        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.

        c->proc = p;
        switchuvm(p);

        p->state = RUNNING;

        // prints sched log
        if (schedlog_active)
        {
          if (ticks > schedlog_lasttick)
          {
            schedlog_active = 0;
          }
          else
          {

            struct proc *pp;
            for (int x = 0; x < RSDL_LEVELS; x++)
            {
              int a = ptable.l[x].activeSet;
              cprintf("%d|active|%d(0)", ticks, x);
              for (int k = 0; k <= ptable.l[x].s[a].queueIndex; k++)
              {
                pp = ptable.l[x].s[a].queue[k];
                cprintf(",[%d]%s:%d(%d)", pp->pid, pp->name, pp->state, pp->ticks_left);
              }
              cprintf("\n");
            }

            for (int x = 0; x < RSDL_LEVELS; x++)
            {
              int e = !ptable.l[x].activeSet;
              cprintf("%d|expired|%d(0)", ticks, x);
              for (int k = 0; k <= ptable.l[x].s[e].queueIndex; k++)
              {
                pp = ptable.l[x].s[e].queue[k];
                cprintf(",[%d]%s:%d(%d)", pp->pid, pp->name, pp->state, pp->ticks_left);
              }
              cprintf("\n");
            }
          }
        }

        // Update active queue
        for (int j = i; j < ptable.l[curLevel].s[active].queueIndex; j++)
        {
          ptable.l[curLevel].s[active].queue[j] = ptable.l[curLevel].s[active].queue[j + 1];
        }
        ptable.l[curLevel].s[active].queueIndex--;
        p->inQueue = 0;

        swtch(&(c->scheduler), p->context);
        switchkvm();
        // cprintf("process done running\n");
        //  Process is done running for now.
        //  It should have changed its p->state before coming back.
        c->proc = 0;
        i = -1; // Make i into 0 so that we search from the front of the queue
        break;  //?
      }

      if (noRunnableProcInLevel == 1)
      {
        continue;
      }

      swap = 0;
      l = -1;
    }

    if (swap == 1)
    {
      // swapping sets

      struct proc *pp;
      for (l = 0; l < RSDL_LEVELS; l++)
      {
        int curLevel = levelOrder[l];
        int active = ptable.l[curLevel].activeSet;
        // transfer sleeping procs
        for (int k = 0; k <= ptable.l[curLevel].s[active].queueIndex; k++)
        {
          pp = ptable.l[curLevel].s[active].queue[k];

          ptable.l[curLevel].s[!active].queueIndex++;
          ptable.l[curLevel].s[!active].queue[ptable.l[curLevel].s[!active].queueIndex] = pp;
          pp->inQueue = 1;
          pp->ticks_left = RSDL_PROC_QUANTUM;
        }
        ptable.l[curLevel].s[active].queueIndex = -1; // empty queue
        ptable.l[curLevel].activeSet = !active;
      }
    }
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  
  // Re-enqueue to same level
    if (myproc()->ticks_left > 0)
    {
      if (myproc()->inQueue != 1)
      {
        int active = ptable.l[activeLevel].activeSet;
        if (ptable.l[activeLevel].s[active].queueIndex >= 4)
        { // no room in same level
          int isEnqueued = 0;
          for (int i = 0; i < RSDL_LEVELS; i++)
          {
            int curLevel = levelOrder[i];
            int a = ptable.l[curLevel].activeSet;
            if (curLevel <= activeLevel)
            {
              continue;
            }
            if (ptable.l[curLevel].s[a].queueIndex < 4)
            { // can enqueue
              ptable.l[curLevel].s[a].queueIndex++;
              ptable.l[curLevel].s[a].queue[ptable.l[curLevel].s[a].queueIndex] = myproc();
              myproc()->inQueue = 1;
              isEnqueued = 1;
              break;
            }
          }
          if (isEnqueued == 0)
          { // no room in all levels ; enqueue in expired set of same level
            ptable.l[activeLevel].s[!active].queueIndex++;
            ptable.l[activeLevel].s[!active].queue[ptable.l[activeLevel].s[!active].queueIndex] = myproc();
            myproc()->inQueue = 1;
            myproc()->ticks_left = RSDL_PROC_QUANTUM;
          }
        }
        else
        {
          ptable.l[activeLevel].s[active].queueIndex++;
          ptable.l[activeLevel].s[active].queue[ptable.l[activeLevel].s[active].queueIndex] = myproc();
          myproc()->inQueue = 1;
        }
      }
    }
    // Enqueue in lower level
    else
    {
      if (myproc()->inQueue != 1)
      {
        int active = ptable.l[activeLevel].activeSet;

        int isEnqueued = 0;
        for (int i = 0; i < RSDL_LEVELS; i++)
        {
          int curLevel = levelOrder[i];
          int a = ptable.l[curLevel].activeSet;
          if (curLevel <= activeLevel)
          {
            continue;
          }
          if (ptable.l[curLevel].s[a].queueIndex < 4)
          { // can enqueue
            ptable.l[curLevel].s[a].queueIndex++;
            ptable.l[curLevel].s[a].queue[ptable.l[curLevel].s[a].queueIndex] = myproc();
            myproc()->inQueue = 1;
            myproc()->ticks_left = RSDL_PROC_QUANTUM;
            isEnqueued = 1;
            break;
          }
        }
        if (isEnqueued == 0)
        { // no room in all levels ; enqueue in expired set of same level
          ptable.l[activeLevel].s[!active].queueIndex++;
          ptable.l[activeLevel].s[!active].queue[ptable.l[activeLevel].s[!active].queueIndex] = myproc();
          myproc()->inQueue = 1;
          myproc()->ticks_left = RSDL_PROC_QUANTUM;
        }
      }
    }

  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  if (p->inQueue != 1)
    {
      int active = ptable.l[activeLevel].activeSet;

      if (ptable.l[activeLevel].s[active].queueIndex >= 4)
      { // no room in same level
        int isEnqueued = 0;
        for (int i = 0; i < RSDL_LEVELS; i++)
        {
          int curLevel = levelOrder[i];
          int a = ptable.l[curLevel].activeSet;
          if (curLevel <= activeLevel)
          {
            continue;
          }
          if (ptable.l[curLevel].s[a].queueIndex < 4)
          { // can enqueue
            ptable.l[curLevel].s[a].queueIndex++;
            ptable.l[curLevel].s[a].queue[ptable.l[curLevel].s[a].queueIndex] = p;
            p->inQueue = 1;
            isEnqueued = 1;
            break;
          }
        }
        if (isEnqueued == 0)
        { // no room in all levels ; enqueue in expired set of same level
          ptable.l[activeLevel].s[!active].queueIndex++;
          ptable.l[activeLevel].s[!active].queue[ptable.l[activeLevel].s[!active].queueIndex] = p;
          p->inQueue = 1;
          p->ticks_left = RSDL_PROC_QUANTUM;
        }
      }
      else
      {
        ptable.l[activeLevel].s[active].queueIndex++;
        ptable.l[activeLevel].s[active].queue[ptable.l[activeLevel].s[active].queueIndex] = p;
        p->inQueue = 1;
      }
    }
  
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  
  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan) {
      // Enqueue process

        if (p->inQueue != 1)
        {
          int active = ptable.l[activeLevel].activeSet;

          if (ptable.l[activeLevel].s[active].queueIndex >= 4)
          { // no room in same level
            int isEnqueued = 0;
            for (int i = 0; i < RSDL_LEVELS; i++)
            {
              int curLevel = levelOrder[i];
              int a = ptable.l[curLevel].activeSet;
              if (curLevel <= activeLevel)
              {
                continue;
              }
              if (ptable.l[curLevel].s[a].queueIndex < 4)
              { // can enqueue
                ptable.l[curLevel].s[a].queueIndex++;
                ptable.l[curLevel].s[a].queue[ptable.l[curLevel].s[a].queueIndex] = p;
                p->inQueue = 1;
                isEnqueued = 1;
                break;
              }
            }
            if (isEnqueued == 0)
            { // no room in all levels ; enqueue in expired set of same level
              ptable.l[activeLevel].s[!active].queueIndex++;
              ptable.l[activeLevel].s[!active].queue[ptable.l[activeLevel].s[!active].queueIndex] = p;
              p->inQueue = 1;
              p->ticks_left = RSDL_PROC_QUANTUM;
            }
          }
          else
          {
            ptable.l[activeLevel].s[active].queueIndex++;
            ptable.l[activeLevel].s[active].queue[ptable.l[activeLevel].s[active].queueIndex] = p;
            p->inQueue = 1;
          }
        }

      p->state = RUNNABLE;
    }
      
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING) {
        // Add process into queue

          if (p->inQueue != 1)
          {
            int active = ptable.l[activeLevel].activeSet;

            if (ptable.l[activeLevel].s[active].queueIndex >= 4)
            { // no room in same level
              int isEnqueued = 0;
              for (int i = 0; i < RSDL_LEVELS; i++)
              {
                int curLevel = levelOrder[i];
                int a = ptable.l[curLevel].activeSet;
                if (curLevel <= activeLevel)
                {
                  continue;
                }
                if (ptable.l[curLevel].s[a].queueIndex < 4)
                { // can enqueue
                  ptable.l[curLevel].s[a].queueIndex++;
                  ptable.l[curLevel].s[a].queue[ptable.l[curLevel].s[a].queueIndex] = p;
                  p->inQueue = 1;
                  isEnqueued = 1;
                  break;
                }
              }
              if (isEnqueued == 0)
              { // no room in all levels ; enqueue in expired set of same level
                ptable.l[activeLevel].s[!active].queueIndex++;
                ptable.l[activeLevel].s[!active].queue[ptable.l[activeLevel].s[!active].queueIndex] = p;
                p->inQueue = 1;
                p->ticks_left = RSDL_PROC_QUANTUM;
              }
            }
            else
            {
              ptable.l[activeLevel].s[active].queueIndex++;
              ptable.l[activeLevel].s[active].queue[ptable.l[activeLevel].s[active].queueIndex] = p;
              p->inQueue = 1;
            }
          }
          p->state = RUNNABLE;
        }
        
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
