#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

// Global variables
int activeSet = 0;

struct proc *chosenProc;
int chosenProcLevel;
int chosenProcLvIndex;

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
  struct set s[2]; 
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

// Newly created functions
void updateQueue(void);
void enqueueProcess(struct proc* p);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");

  for (int j = 0; j < RSDL_LEVELS; j++) {
    //Index being -1 implies that there is nothing within the queue
    ptable.s[0].queueIndex[j] = -1;  
    ptable.s[1].queueIndex[j] = -1;

    ptable.s[activeSet].lv_tix[j] = RSDL_LEVEL_QUANTUM;
    ptable.s[!activeSet].lv_tix[j] = RSDL_LEVEL_QUANTUM;
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

  // Panic if RSDL_LEVELS < 1
  if (RSDL_LEVELS < 1) {
    panic("Number of levels is less than 1");
  }

  // Panic if the starting level is "higher" than the number of levels
  if (RSDL_STARTING_LEVEL > RSDL_LEVELS-1) {
    panic("RSDL_STARTING_LEVEL exceeds the number of RSDL_LEVELS");
  }

  // Add init into the 1st queue
  ptable.s[activeSet].queueIndex[RSDL_STARTING_LEVEL]++;
  ptable.s[activeSet].queue[RSDL_STARTING_LEVEL][ptable.s[activeSet].queueIndex[RSDL_STARTING_LEVEL]] = p;
  p->inQueue = 1;
  p->currLevel = RSDL_STARTING_LEVEL;
  p->state = RUNNABLE;
  p->ticks_left = RSDL_PROC_QUANTUM; 
  p->level_ticks_left = &ptable.s[activeSet].lv_tix[RSDL_STARTING_LEVEL];

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

  // Add new process in next available level
  int isEnqueued = 0;
  for (int j=RSDL_STARTING_LEVEL; j<RSDL_LEVELS; j++) {
    if (ptable.s[activeSet].lv_tix[j] > 0) {
      ptable.s[activeSet].queueIndex[j]++;
      ptable.s[activeSet].queue[j][ptable.s[activeSet].queueIndex[j]] = np;
      np->inQueue = 1;
      np->currLevel = j;
      np->state = RUNNABLE;
      np->ticks_left = RSDL_PROC_QUANTUM;
      np->level_ticks_left = &ptable.s[activeSet].lv_tix[j];
      isEnqueued = 1;
      break;
    }
  }

  if (isEnqueued == 0) {
    int j = RSDL_STARTING_LEVEL;
    ptable.s[!activeSet].queueIndex[j]++;
    ptable.s[!activeSet].queue[j][ptable.s[!activeSet].queueIndex[j]] = np;
    np->inQueue = 1;
    np->currLevel = j;
    np->level_ticks_left = &ptable.s[!activeSet].lv_tix[j];
  }

  release(&ptable.lock);

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
  updateQueue();

  wakeup1(curproc->parent);

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

    // Debug statements
    //cprintf("current process is: %s\n", myproc()->name);
    //cprintf("activeSet is %d\n",activeSet);

    // Assume there are no runnable processes (i.e. noRunnableProcInLevel = 1)
    int noRunnableProcInLevel;
    int swap = 1; // Assume that no swap will occur

    // Loop through all processes within the queue
    int level;
    int i;
    for (level = 0; level < RSDL_LEVELS; level++) {
      noRunnableProcInLevel = 1;
      for (i = 0; i <= ptable.s[activeSet].queueIndex[level]; i++) {
        p = ptable.s[activeSet].queue[level][i];
        if (p->state != RUNNABLE) {
          continue;
        }
        noRunnableProcInLevel = 0;  // Implies that there is a runnable process in a level

        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
    
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;

        // Schedlog
        if (schedlog_active) {
          if (ticks > schedlog_lasttick) {
          schedlog_active = 0;
          } else {
            struct proc *pp;

            for (int x = 0; x < RSDL_LEVELS; x++) {
              cprintf("%d|active|%d(%d)", ticks, x, ptable.s[activeSet].lv_tix[x]);
              
              for (int k = 0; k <= ptable.s[activeSet].queueIndex[x]; k++) {
                pp = ptable.s[activeSet].queue[x][k];
                cprintf(",[%d]%s:%d(%d)", pp->pid, pp->name, pp->state, pp->ticks_left);
              }
              cprintf("\n");
            }

            int e = !activeSet;
            for (int x = 0; x < RSDL_LEVELS; x++) {
              cprintf("%d|expired|%d(%d)", ticks, x, ptable.s[e].lv_tix[x]);

              for (int k = 0; k <= ptable.s[e].queueIndex[x]; k++) {
                pp = ptable.s[e].queue[x][k];
                cprintf(",[%d]%s:%d(%d)", pp->pid, pp->name, pp->state, pp->ticks_left);
              }
              cprintf("\n");
            }
          }
        }

        // Take note of the currently running process (to be dequeued later)
        chosenProc = p;
        chosenProcLevel = level;
        chosenProcLvIndex = i;

        swtch(&(c->scheduler), p->context);
        switchkvm();

        c->proc = 0;
        i = -1;
        break; // ?
      }
      if (noRunnableProcInLevel == 1) {
        continue;
      }
      swap = 0;
      level = -1;
    }

    if (swap == 1) {
      if (activeSet == 0) {
        struct proc *pp;

        for (level = 0; level < RSDL_LEVELS; level++) {
          
          ptable.s[activeSet].lv_tix[level] = RSDL_LEVEL_QUANTUM;
          ptable.s[!activeSet].lv_tix[level] = RSDL_LEVEL_QUANTUM;

          if (level < RSDL_STARTING_LEVEL) {
            continue;
          }
          for (int k = 0; k <= ptable.s[activeSet].queueIndex[level]; k++) {
            pp = ptable.s[activeSet].queue[level][k];
            pp->inQueue = 1;
            pp->ticks_left = RSDL_PROC_QUANTUM;
            pp->currLevel = RSDL_STARTING_LEVEL;
            ptable.s[!activeSet].queueIndex[RSDL_STARTING_LEVEL]++;
            pp->level_ticks_left = &ptable.s[!activeSet].lv_tix[level];
            int idx = ptable.s[!activeSet].queueIndex[RSDL_STARTING_LEVEL];
            ptable.s[!activeSet].queue[RSDL_STARTING_LEVEL][idx] = pp;
          }
          ptable.s[activeSet].queueIndex[level] = -1;
        }
        activeSet = 1;
      }
      else if (activeSet == 1) {
        struct proc *pp;

        for (level = 0; level < RSDL_LEVELS; level++) {
          ptable.s[activeSet].lv_tix[level] = RSDL_LEVEL_QUANTUM;
          ptable.s[!activeSet].lv_tix[level] = RSDL_LEVEL_QUANTUM;
          
          if (level < RSDL_STARTING_LEVEL) {
            continue;
          }
          for (int k = 0; k <= ptable.s[activeSet].queueIndex[level]; k++) {
            pp = ptable.s[activeSet].queue[level][k];
            pp->inQueue = 1;
            pp->ticks_left = RSDL_PROC_QUANTUM;
            pp->currLevel = RSDL_STARTING_LEVEL;
            ptable.s[!activeSet].queueIndex[RSDL_STARTING_LEVEL]++;
            pp->level_ticks_left = &ptable.s[!activeSet].lv_tix[level];
            int idx = ptable.s[!activeSet].queueIndex[RSDL_STARTING_LEVEL];
            ptable.s[!activeSet].queue[RSDL_STARTING_LEVEL][idx] = pp;
          }
          ptable.s[activeSet].queueIndex[level] = -1;
        }
        activeSet = 0;      
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
    updateQueue();

    if (myproc()->ticks_left > 0)
    {
      panic("Yield before the quantum finished!");
    }

    else if (myproc()->inQueue != 1)
    { 
      // Enqueue in the next available level
      int isEnqueued = 0;
      int nextLowest = myproc()->currLevel + 1;
      for (int i = nextLowest; i < RSDL_LEVELS; i++) {
        if (ptable.s[activeSet].queueIndex[i] < NPROC-1 && ptable.s[activeSet].lv_tix[i] > 0) { 
          ptable.s[activeSet].queueIndex[i]++;
          int a = ptable.s[activeSet].queueIndex[i];
          ptable.s[activeSet].queue[i][a] = myproc();
          myproc()->currLevel++;
          myproc()->inQueue = 1;
          myproc()->ticks_left = RSDL_PROC_QUANTUM;          
          myproc()->level_ticks_left = &ptable.s[activeSet].lv_tix[i];
          isEnqueued = 1;
          break;
        }
      }

      // Enqueue in the expired set
      if (isEnqueued == 0) {
        int b = RSDL_STARTING_LEVEL;
        ptable.s[!activeSet].queueIndex[b]++;
        ptable.s[!activeSet].queue[b][ptable.s[!activeSet].queueIndex[b]] = myproc();
        myproc()->inQueue = 1;
        myproc()->ticks_left = RSDL_PROC_QUANTUM;
        myproc()->currLevel = RSDL_STARTING_LEVEL;
        myproc()->level_ticks_left = &ptable.s[!activeSet].lv_tix[b];
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
  updateQueue();
  enqueueProcess(p);
  
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
      enqueueProcess(p);
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
        enqueueProcess(p);
        p->state = RUNNABLE;
      }
        
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

void enqueueProcess(struct proc *p) {
  if (p->inQueue != 1) 
  {
    // Find next available level if level-quantum is less than 0
    if (ptable.s[activeSet].queueIndex[p->currLevel] >= 64 || ptable.s[activeSet].lv_tix[p->currLevel] <= 0) {
      int isEnqueued = 0;
      int nextLowest = p->currLevel + 1;
      for (int i = nextLowest; i < RSDL_LEVELS; i++) {
        if (ptable.s[activeSet].queueIndex[i] < NPROC-1 && ptable.s[activeSet].lv_tix[i] > 0) {
          ptable.s[activeSet].queueIndex[i]++;
          int a = ptable.s[activeSet].queueIndex[i];
          ptable.s[activeSet].queue[i][a] = p;
          p->currLevel = i;
          p->inQueue = 1;
          p->ticks_left = RSDL_PROC_QUANTUM; 
          p->level_ticks_left = &ptable.s[activeSet].lv_tix[i];
          isEnqueued = 1;
          break;
        }
      }

      // Place in expired set if no level is available
      if (isEnqueued == 0) {
        int b = RSDL_STARTING_LEVEL;
        ptable.s[!activeSet].queueIndex[b]++;
        ptable.s[!activeSet].queue[b][ptable.s[!activeSet].queueIndex[b]] = p;
        p->currLevel = b;
        p->inQueue = 1;
        p->level_ticks_left = &ptable.s[!activeSet].lv_tix[b];
        p->ticks_left = RSDL_PROC_QUANTUM;
      }
    }
    // Enqueue on the same level. No replenishment of quanta
    else {  
      ptable.s[activeSet].queueIndex[p->currLevel]++; 
      int a = ptable.s[activeSet].queueIndex[p->currLevel];
      ptable.s[activeSet].queue[p->currLevel][a] = p;
      p->inQueue = 1;
    }
  }
}

void enqueueNextLevel() {
  acquire(&ptable.lock);
  updateQueue();

  int isEnqueued = 0;
  int dequeuedLevel = myproc()->currLevel;
  int nextLowest = myproc()->currLevel+1;
  struct proc *p;

  for (int i = nextLowest; i < RSDL_LEVELS; i++) {
    // Implies that we can enqueue processes here. Conditional just checks if there is enough space to place the procs
    if (ptable.s[activeSet].queueIndex[i] + ptable.s[activeSet].queueIndex[myproc()->currLevel] + 1 < NPROC-1 && ptable.s[activeSet].lv_tix[i] > 0) {
      // Try lang zzz
      for (int j = 0; j <= ptable.s[activeSet].queueIndex[myproc()->currLevel]; j++) {
        p = ptable.s[activeSet].queue[myproc()->currLevel][j];
        ptable.s[activeSet].queueIndex[i]++;
        ptable.s[activeSet].queue[i][ptable.s[activeSet].queueIndex[i]] = p;
        p->currLevel = i;
        p->inQueue = 1;
        p->ticks_left = RSDL_PROC_QUANTUM;
        p->level_ticks_left = &ptable.s[activeSet].lv_tix[i];
      }
      ptable.s[activeSet].queueIndex[i]++;
      ptable.s[activeSet].queue[i][ptable.s[activeSet].queueIndex[i]] = myproc();
      myproc()->currLevel = i;
      myproc()->inQueue = 1;
      myproc()->ticks_left = RSDL_PROC_QUANTUM;
      myproc()->level_ticks_left = &ptable.s[activeSet].lv_tix[i];
      isEnqueued = 1;
      ptable.s[activeSet].queueIndex[dequeuedLevel] = -1;
    }
    if (isEnqueued == 1) {
      break;
    }
  }
  
  if (isEnqueued == 0) {
    for (int j = 0; j <= ptable.s[activeSet].queueIndex[myproc()->currLevel]; j++) {
      p = ptable.s[activeSet].queue[myproc()->currLevel][j];
      ptable.s[!activeSet].queueIndex[RSDL_STARTING_LEVEL]++;
      ptable.s[!activeSet].queue[RSDL_STARTING_LEVEL][ptable.s[!activeSet].queueIndex[RSDL_STARTING_LEVEL]] = p;
      p->currLevel = RSDL_STARTING_LEVEL;
      p->inQueue = 1;
      p->ticks_left = RSDL_PROC_QUANTUM;
      p->level_ticks_left = &ptable.s[!activeSet].lv_tix[RSDL_STARTING_LEVEL];
    }
    ptable.s[!activeSet].queueIndex[RSDL_STARTING_LEVEL]++;
    ptable.s[!activeSet].queue[RSDL_STARTING_LEVEL][ptable.s[!activeSet].queueIndex[RSDL_STARTING_LEVEL]] = myproc();
    myproc()->currLevel = RSDL_STARTING_LEVEL;
    myproc()->inQueue = 1;
    myproc()->ticks_left = RSDL_PROC_QUANTUM;
    myproc()->level_ticks_left = &ptable.s[!activeSet].lv_tix[RSDL_STARTING_LEVEL];
    ptable.s[activeSet].queueIndex[dequeuedLevel] = -1;
  }
  
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

void updateQueue() {
  // Update level queue
  for (int j = chosenProcLvIndex; j <= ptable.s[activeSet].queueIndex[chosenProcLevel]; j++) {
    ptable.s[activeSet].queue[chosenProcLevel][j] = ptable.s[activeSet].queue[chosenProcLevel][j+1];
  }
  ptable.s[activeSet].queueIndex[chosenProcLevel]--;
  chosenProc->inQueue = 0;
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
