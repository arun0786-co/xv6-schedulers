#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "schedstat.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
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
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p, 0, 0, 0);
}

// waitx(int *status, int *wtime, int *rtime): wait() that also reports how
// many ticks the child spent running (rtime), and alive but not running
// (wtime: waiting for a CPU, or sleeping).
uint64
sys_waitx(void)
{
  uint64 addr, waddr, raddr;
  uint wtime, rtime;
  int pid;
  struct proc *p = myproc();

  argaddr(0, &addr);
  argaddr(1, &waddr);
  argaddr(2, &raddr);
  if((pid = kwait(addr, &rtime, &wtime, 0)) < 0)
    return -1;
  if(copyout(p->pagetable, waddr, (char *)&wtime, sizeof(wtime)) < 0 ||
     copyout(p->pagetable, raddr, (char *)&rtime, sizeof(rtime)) < 0)
    return -1;
  return pid;
}

// waitstat(int *status, struct procstat *st): wait() that also reports all the
// kernel knows about how the child was scheduled.
uint64
sys_waitstat(void)
{
  uint64 addr, saddr;
  struct procstat st;
  int pid;

  argaddr(0, &addr);
  argaddr(1, &saddr);
  if((pid = kwait(addr, 0, 0, &st)) < 0)
    return -1;
  if(copyout(myproc()->pagetable, saddr, (char *)&st, sizeof(st)) < 0)
    return -1;
  return pid;
}

// schedtrace(struct schedevent *buf, int max): see kschedtrace().
uint64
sys_schedtrace(void)
{
  uint64 addr;
  int max;

  argaddr(0, &addr);
  argint(1, &max);
  return kschedtrace(addr, max);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
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

// setnice(pid, nice): set a process's nice value, -20 (most CPU time) to
// 19 (least), which sets its CFS weight. Returns 0, or -1 if there is no
// such process or the value is out of range.
uint64
sys_setnice(void)
{
  int pid, nice;

  argint(0, &pid);
  argint(1, &nice);
  return ksetnice(pid, nice);
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
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
