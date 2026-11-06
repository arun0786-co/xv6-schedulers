#include "kernel/types.h"
#include "kernel/schedstat.h"
#include "user/user.h"

// usage: mlfqtest <solo|two|io|arrive|game>
// Runs a small workload with the kernel's trace on, then prints what the trace saw
// as "T <tick> <pid> <level>", so that tests/ can check the rules from outside.
// "P <pid> <name>" says which process is which. The workload only needs to exist
// under any policy; only MLFQ has levels to look at.

#define MAXEV 8192
static struct schedevent ev[MAXEV];

// burn CPU until the clock says n more ticks have gone by
static void
hog(int n)
{
  int end = uptime() + n;
  while(uptime() < end)
    ;
}

static int
spawn_hog(int n, char *name)
{
  int pid = fork();
  if(pid == 0){
    hog(n);
    exit(0);
  }
  printf("P %d %s\n", pid, name);
  return pid;
}

// short bursts with sleeps between them, like something waiting for input
static int
spawn_interactive(int rounds, char *name)
{
  int pid = fork();
  if(pid == 0){
    for(int i = 0; i < rounds; i++){
      for(volatile int k = 0; k < 20000; k++)
        ;
      pause(3);
    }
    exit(0);
  }
  printf("P %d %s\n", pid, name);
  return pid;
}

static void
print_trace(void)
{
  int n = schedtrace(ev, MAXEV);
  for(int i = 0; i < n; i++)
    printf("T %d %d %d\n", ev[i].tick, ev[i].pid, ev[i].level);
}

// One unit of work. Asking the clock is a system call, which costs far more than a
// few loops, so everything below asks it only once per BATCH units. Otherwise the
// measurement of how much fits in a tick does not hold for code that does not ask.
#define BATCH 64

static void
unit(void)
{
  for(volatile int k = 0; k < 1000; k++)
    ;
}

// How many units fit in a tick, measured on this machine and now.
static int
units_per_tick(void)
{
  int t = uptime();
  while(uptime() == t)
    ;
  t = uptime();
  int n = 0;
  while(uptime() < t + 3){
    for(int i = 0; i < BATCH; i++)
      unit();
    n += BATCH;
  }
  return n / 3;
}

// Work for as long as it can, and report how much through the pipe.
static void
work_until(int end, int fd)
{
  int done = 0;
  while(uptime() < end){
    for(int i = 0; i < BATCH; i++)
      unit();
    done += BATCH;
  }
  write(fd, &done, sizeof(done));
  exit(0);
}

// The cheat: wake just after a tick, run for most of the time to the next one, and
// sleep before it arrives. Every tick finds it asleep, so it is never charged.
static void
cheat_until(int end, int fd, int perburst)
{
  int done = 0;
  while(uptime() < end){
    for(int i = 0; i < perburst; i++)
      unit();
    done += perburst;
    pause(1);
  }
  write(fd, &done, sizeof(done));
  exit(0);
}

// A cheater against two honest processes, for the same length of time. Prints how
// much work each got done.
static void
game(void)
{
  int fds[3][2];
  int names[3];
  int perburst = units_per_tick() * 7 / 10; // most of a tick, with room for error
  int end = uptime() + 300;

  for(int i = 0; i < 3; i++){
    pipe(fds[i]);
    int pid = fork();
    if(pid == 0){
      if(i == 0)
        cheat_until(end, fds[i][1], perburst);
      work_until(end, fds[i][1]);
    }
    names[i] = pid;
    close(fds[i][1]);
  }
  for(int i = 0; i < 3; i++){
    int done = 0;
    read(fds[i][0], &done, sizeof(done));
    printf("W %s %d %d\n", i == 0 ? "cheater" : "honest", names[i], done);
    close(fds[i][0]);
  }
  for(int i = 0; i < 3; i++)
    wait(0);
}

int
main(int argc, char *argv[])
{
  if(argc != 2){
    printf("usage: mlfqtest <solo|two|io|arrive|game>\n");
    exit(1);
  }
  char *what = argv[1];
  int kids = 0;

  // start just after a boost, so each run is the same however long the boot took
  while(uptime() % 48 != 1)
    pause(1);
  schedtrace(0, 0);
  if(strcmp(what, "solo") == 0){
    spawn_hog(150, "hog");
    kids = 1;
  } else if(strcmp(what, "two") == 0){
    spawn_hog(200, "hogA");
    spawn_hog(200, "hogB");
    kids = 2;
  } else if(strcmp(what, "io") == 0){
    spawn_hog(150, "hog");
    spawn_interactive(30, "interactive");
    kids = 2;
  } else if(strcmp(what, "arrive") == 0){
    // long enough for the first to sink to the bottom, then a new one arrives
    spawn_hog(46, "old");
    pause(30);
    spawn_hog(6, "new");
    kids = 2;
  } else if(strcmp(what, "game") == 0){
    game();
    kids = 0;
  } else {
    printf("mlfqtest: unknown workload %s\n", what);
    exit(1);
  }
  for(int i = 0; i < kids; i++)
    wait(0);
  if(strcmp(what, "game") != 0)
    print_trace();
  else
    schedtrace(ev, 0);
  printf("mlfqtest done\n");
  exit(0);
}
