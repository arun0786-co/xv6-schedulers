#include "kernel/types.h"
#include "user/user.h"

// usage: schedulertest [-v]
// Forks NFORK children at once. The first IO of them sleep, as an I/O-bound
// process would; the rest spin on the CPU. Prints the average number of
// ticks the children spent running (rtime) and alive but not running
// (wtime), as reported by waitx(). With -v it also prints each child, in
// the order they were collected.
#define NFORK 10
#define IO 5
#define SPIN 1000000000

int
main(int argc, char *argv[])
{
  int verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
  int pids[NFORK];
  int n, pid, wtime, rtime, twtime = 0, trtime = 0;

  for(n = 0; n < NFORK; n++){
    pid = fork();
    if(pid < 0)
      break;
    if(pid == 0){
      if(n < IO){
        pause(200);
      } else {
        for(volatile int i = 0; i < SPIN; i++)
          ;
      }
      exit(0);
    }
    pids[n] = pid;
  }
  for(int left = n; left > 0; left--){
    if((pid = waitx(0, &wtime, &rtime)) < 0)
      break;
    trtime += rtime;
    twtime += wtime;
    if(verbose){
      int i = 0;
      while(i < n && pids[i] != pid)
        i++;
      printf("child %d (%s, pid %d): rtime %d, wtime %d\n",
             i, i < IO ? "sleeps" : "spins", pid, rtime, wtime);
    }
  }
  printf("Average rtime %d,  wtime %d\n", trtime / NFORK, twtime / NFORK);
  exit(0);
}
