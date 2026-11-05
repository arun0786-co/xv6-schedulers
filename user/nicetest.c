#include "kernel/types.h"
#include "user/user.h"

// usage: nicetest [nice...]
// For CFS on one CPU: make qemu SCHEDULER=CFS CFSLOG=0 CPUS=1
// Starts one CPU-bound child per nice value (default 0 and 5). They all spin
// until the same tick and then exit, so each child's share of the CPU time
// should match its share of the total weight.
#define TICKS 100
#define MAXCHILD 8

// the kernel's weight table, nice -20 to 19
static const int weight[40] = {
  88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
   9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
   1024,   820,   655,   526,   423,   335,   272,   215,   172,   137,
    110,    87,    70,    56,    45,    36,    29,    23,    18,    15,
};

int
main(int argc, char *argv[])
{
  int nice[MAXCHILD] = {0, 5}, pids[MAXCHILD], rtime[MAXCHILD];
  int n = 2, end, pid, wtime, rt, ok = 1;
  int totalw = 0, totalr = 0;

  if(argc > 1){
    n = argc - 1;
    if(n > MAXCHILD){
      fprintf(2, "nicetest: at most %d children\n", MAXCHILD);
      exit(1);
    }
    for(int i = 0; i < n; i++){
      char *s = argv[i + 1];
      nice[i] = *s == '-' ? -atoi(s + 1) : atoi(s);
      if(nice[i] < -20 || nice[i] > 19){
        fprintf(2, "nicetest: nice must be -20 to 19\n");
        exit(1);
      }
    }
  }

  end = uptime() + TICKS;
  for(int i = 0; i < n; i++){
    if((pid = fork()) == 0){
      if(setnice(getpid(), nice[i]) < 0)
        exit(1);
      while(uptime() < end)
        for(volatile int k = 0; k < 100000; k++)
          ;
      exit(0);
    }
    pids[i] = pid;
  }
  for(int left = n; left > 0; left--){
    if((pid = waitx(0, &wtime, &rt)) < 0)
      break;
    for(int i = 0; i < n; i++)
      if(pids[i] == pid)
        rtime[i] = rt;
  }

  for(int i = 0; i < n; i++){
    totalw += weight[nice[i] + 20];
    totalr += rtime[i];
  }
  for(int i = 0; i < n; i++){
    int want = 100 * weight[nice[i] + 20] / totalw;
    int got = totalr ? 100 * rtime[i] / totalr : 0;
    printf("nice %d: weight %d, expected %d%%, got %d%% (%d ticks)\n",
           nice[i], weight[nice[i] + 20], want, got, rtime[i]);
    if(got < want - 5 || got > want + 5)
      ok = 0;
  }
  printf(ok ? "nicetest: OK\n" : "nicetest: FAIL\n");
  exit(ok ? 0 : 1);
}
