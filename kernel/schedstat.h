// What the kernel reports about scheduling, for the kernel and for user programs.

// which process was on a CPU at a tick, and at what level
struct schedevent {
  uint tick;
  short pid;
  short level;
};

// a finished child, from waitstat()
struct procstat {
  int pid;
  uint ctime;     // tick it was created
  int first_run;  // tick it first got a CPU
  uint etime;     // tick it exited
  uint rtime;     // ticks it was seen running
  uint nswitch;   // how many times it was given a CPU
};
