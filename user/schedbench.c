#include "kernel/types.h"
#include "kernel/schedstat.h"
#include "user/user.h"

// usage: schedbench
// One mixed workload, the same for every policy, reporting by kind of process:
//   long         4 processes that each need a lot of CPU
//   short        4 small jobs, which arrive while the long ones are running
//   interactive  3 processes that do a little work and sleep, over and over
// Turnaround is creation to exit. Response is creation to first time on a CPU.
// All are in ticks, and all are averages. Run it on 1 CPU to compare policies.

// about 55 ticks and 3 ticks of work, measured under FCFS on qemu
#define LONGWORK 2000000000
#define SHORTWORK 110000000
#define NLONG 4
#define NSHORT 4
#define NINTER 3
#define NKIDS (NLONG + NSHORT + NINTER)

enum { LONG, SHORT, INTER, NKIND };
static char *kindname[NKIND] = { "long", "short", "interactive" };

static void
work(int n)
{
  for(volatile int i = 0; i < n; i++)
    ;
}

static int
start(int kind, int *pids, int *kinds, int *count)
{
  int pid = fork();
  if(pid == 0){
    if(kind == LONG)
      work(LONGWORK);
    else if(kind == SHORT)
      work(SHORTWORK);
    else {
      for(int r = 0; r < 25; r++){
        work(3000000);
        pause(4);
      }
    }
    exit(0);
  }
  pids[*count] = pid;
  kinds[*count] = kind;
  (*count)++;
  return pid;
}

int
main(void)
{
  int pids[NKIDS], kinds[NKIDS], n = 0;
  int total[NKIND], response[NKIND], worst[NKIND], switches[NKIND], count[NKIND];

  for(int k = 0; k < NKIND; k++)
    total[k] = response[k] = worst[k] = switches[k] = count[k] = 0;

  for(int i = 0; i < NLONG; i++)
    start(LONG, pids, kinds, &n);
  for(int i = 0; i < NINTER; i++)
    start(INTER, pids, kinds, &n);
  // the short jobs arrive one at a time, into a machine that is already busy
  for(int i = 0; i < NSHORT; i++){
    pause(10);
    start(SHORT, pids, kinds, &n);
  }

  for(int i = 0; i < n; i++){
    struct procstat st;
    int pid = waitstat(0, &st);
    if(pid < 0)
      break;
    for(int j = 0; j < n; j++){
      if(pids[j] != pid)
        continue;
      int k = kinds[j];
      int turn = st.etime - st.ctime;
      int resp = st.first_run - st.ctime;
      total[k] += turn;
      response[k] += resp;
      if(resp > worst[k])
        worst[k] = resp;
      switches[k] += st.nswitch;
      count[k]++;
    }
  }
  for(int k = 0; k < NKIND; k++)
    printf("R %s %d %d %d %d\n", kindname[k], total[k] / count[k], response[k] / count[k],
           worst[k], switches[k] / count[k]);
  printf("schedbench done\n");
  exit(0);
}
