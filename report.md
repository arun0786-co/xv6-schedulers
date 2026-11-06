# xv6: getreadcount, FCFS, CFS and MLFQ

This tree is [mit-pdos/xv6-riscv](https://github.com/mit-pdos/xv6-riscv) at commit
`9dbd536` (10 September 2025), which is the commit `import xv6-riscv at 9dbd536`
here, with these changes on top:

```
make qemu SCHEDULER=CFS CPUS=1
```

Build options:

| Option | Values | Effect |
|---|---|---|
| `SCHEDULER` | `RR` (default), `FCFS`, `CFS`, `MLFQ` | scheduling policy; any other value is an error |
| `MLFQ_FAIR` | `0` (default), `1` | MLFQ charges real time used at a level, not ticks that land on it |
| `CPUS` | 1 to 8, default 3 | number of CPUs qemu gives xv6 |
| `CFSLOG` | `1` (default), `0` | CFS prints its run queue before every decision |
| `READCOUNT_START` | a number | starting value of the read counter, to test the wrap-around |

The options are compiler flags, which make cannot see change, so the
Makefile writes them to `kernel/kernel.options` and rebuilds the kernel
whenever they differ from the last build. `make clean` between policies is
not needed.

New user programs: `readcount`, `readrace`, `schedulertest`, `nicetest`, `mlfqtest`, `schedbench`.
New system calls: `getreadcount`, `waitx`, `setnice`, `waitstat`, `schedtrace`.

## Part A: getreadcount

The counter lives in `kernel/sysfile.c`. `sys_read` adds the value
`fileread` returns when it is positive, so failed reads and end of file add
nothing.

- **Concurrency.** `read()` runs on several CPUs at once. A plain
  `count += n` is a load, an add and a store, and two CPUs can both load the
  old value, so one of the additions is lost. The counter is changed with
  `__atomic_fetch_add`, which compiles to a single RISC-V `amoadd.w`
  instruction.
- **Overflow.** The counter is a 32-bit `uint`, so it wraps around to 0
  after 4294967295, and `getreadcount` returns `uint` to user space. Unsigned
  overflow is defined in C to wrap, unlike signed overflow. `readcount`
  subtracts the two readings as unsigned numbers, which gives the right
  difference even when the counter wrapped in between.

`readcount` prints the count, reads 100 bytes from `/README`, prints the
count again and checks that it grew by at least 100. It can grow by more if
other processes read at the same time. Booted with
`READCOUNT_START=4294967200`:

```
$ readcount
read count before: 4294967210
read count after:  14
OK: the count grew by 100 after reading 100 bytes
```

`readrace [procs] [reads]` starts several processes that each read one byte
at a time, then checks that the count grew by exactly `procs * reads`.

## Part B: scheduling

### Time accounting

Each process records `ctime` (the tick it was created), `rtime` (ticks spent
running) and `etime` (the tick it exited). Every CPU's timer interrupt
charges one tick to the process running on that CPU (`proctick` in
`proc.c`). `waitx(&status, &wtime, &rtime)` is `wait` that also returns the
child's `rtime` and `wtime = etime - ctime - rtime`: the ticks it was alive
but not running, whether waiting for a CPU or sleeping.

`schedulertest [-v]` forks 10 children at once. Children 0 to 4 sleep for 200
ticks, like I/O-bound processes. Children 5 to 9 each spin through
1,000,000,000 loop iterations. It prints the average `rtime` and `wtime` of
the 10 children, and with `-v` each child's own times. The course's own
`schedulertest` was behind an IIIT login, so this one was written from its
description.

### Choosing the policy

`scheduler()` in `kernel/proc.c` keeps xv6's round-robin loop for `RR`. For
`FCFS` and `CFS` it calls `pick()`, which returns the next process with its
lock held. The timer interrupt handlers in `kernel/trap.c` used to call
`yield()` on every tick. They now ask `preempt()` first: always for round
robin, never for FCFS, and for CFS only when the time slice is used up.

### FCFS

`pick()` scans the process table for the RUNNABLE process with the smallest
`ctime`. Processes created in the same tick are ordered by pid, since pids
are handed out in creation order. The scan locks each process only while
looking at it. It then locks the chosen one and checks that it is still
RUNNABLE, because another CPU may have taken it in between. `preempt()`
returns 0, so a process keeps its CPU until it exits or sleeps, even when an
older process wakes up.

### CFS

- **B.1 Priority.** Each process has a `nice` value from -20 to 19, default
  0, inherited across `fork` as in Unix. The weight comes from a table of
  `1024 / 1.25^nice`, rounded the way Linux rounds it: 88761 at nice -20,
  1024 at 0 and 15 at 19. The new `setnice(pid, nice)` system call sets it.
- **B.2 Virtual runtime.** `vruntime` starts at 0 when a process is created.
  Every tick a process runs adds `1000 * 1024 / weight`, so it is counted in
  thousandths of a tick. A tick at nice 0 adds 1000, at nice 5 it adds 3056,
  at nice -20 it adds 11. With whole ticks and integer division, a process
  heavier than 1024 would add 0 and keep `vruntime` 0 forever.
- **B.2 Scheduling.** The run queue is a linked list of the RUNNABLE
  processes in ascending `vruntime`. Every place where xv6 makes a process
  RUNNABLE (`userinit`, `fork`, `yield`, `wakeup`, `kill`) now calls
  `setrunnable()`, which inserts the process behind every process with a
  lower or equal `vruntime`, so equal ones take turns. `pick()` takes the
  head of the queue, which has the smallest `vruntime`. The queue has its own
  spinlock, always taken after a process lock and never before one, so the
  two cannot deadlock.
- **B.3 Time slice.** When a process is picked, its slice is
  `48 / (number of RUNNABLE processes)` ticks, but at least 3. The timer
  interrupt counts the ticks it uses, and `preempt()` sends it back to the
  queue when the slice is used up. As the spec asks, a process that wakes up
  does not cut the current slice short.

Because `vruntime` starts at 0, a process created late runs ahead of
everything else until it catches up. Linux avoids this by starting a new
process at the smallest `vruntime` in the queue; the spec asks for 0, and
that is what this does.

### CFS log

Before every decision `pick()` prints the whole run queue, under the queue's
lock, so blocks from different CPUs cannot mix. `CFSLOG=0` turns it off.
`vRuntime` is in thousandths of a tick. Two consecutive blocks from
`schedulertest` on one CPU:

```
[Scheduler Tick]
PID: 10 | vRuntime: 4000
PID: 11 | vRuntime: 4000
PID: 12 | vRuntime: 4000
PID: 13 | vRuntime: 4000
PID: 9 | vRuntime: 9000
--> Scheduling PID 10 (lowest vRuntime)
[Scheduler Tick]
PID: 4 | vRuntime: 0
PID: 5 | vRuntime: 0
PID: 6 | vRuntime: 0
PID: 7 | vRuntime: 0
PID: 8 | vRuntime: 0
PID: 11 | vRuntime: 4000
PID: 12 | vRuntime: 4000
PID: 13 | vRuntime: 4000
PID: 9 | vRuntime: 9000
PID: 10 | vRuntime: 13000
--> Scheduling PID 4 (lowest vRuntime)
```

PID 10 had run 4 ticks. It ran a 9-tick slice (48 / 5 runnable), so it went
from 4000 to 13000 and back into the queue behind PID 9. PIDs 4 to 8 are the
sleeping children: xv6's `pause` sleeps on the tick counter and checks the
time on every tick, so a sleeping process is briefly RUNNABLE once per tick.
They have run less than a tick in total, so their `vruntime` is still 0 and
they go first.

`nicetest` runs two spinning processes at nice 0 and nice 5 for 100 ticks on
one CPU. With weights 1024 and 335 they should get 75% and 25% of the CPU:

```
nice 0: weight 1024, expected 75%, got 76% (76 ticks)
nice 5: weight 335, expected 24%, got 24% (24 ticks)
nicetest: OK
```

### MLFQ

The bonus part. There are four queues with slices of 1, 4, 8 and 16 ticks, held in `mq` in `proc.c`. `setrunnable` appends a process to the queue of its level, and `pick` takes the front of the highest non-empty one.

- **New processes** start at level 0 (`allocproc`).
- **A used slice:** `preempt` runs on each timer interrupt. If the slice is used it calls `mq_demote` and the process yields, so it goes to the back of the next queue down, or of the lowest, which is what makes that one round robin.
- **A higher queue:** `preempt` also looks for work in any higher queue, and yields if there is some, so that takes over at the next tick. The spec does not say where the interrupted process goes; here it goes to the back of its own queue, and keeps what it has used of its slice.
- **Sleeping:** the level stays, and the slice starts afresh when it wakes, as the spec says.
- **The boost:** every 48 ticks on CPU 0, `mlfq_boost` moves all the queues onto queue 0 and sets every process to level 0. The tick is charged first, so a boost starts the running process's turn afresh.

A bug worth recording: at first `pick` restarted the slice each time, so a process interrupted by a higher queue came back with a whole new slice. xv6 wakes every sleeper in `pause()` on every tick, so a single sleeping neighbour kept a CPU-bound process at level 1 for 29 ticks instead of 4. `tests/mlfqcheck.py` found it from the kernel's trace.

## Performance comparison

`schedulertest` on one CPU, as the spec asks, on qemu 10.0 in a Linux container on an Apple silicon Mac, with CFS built with `CFSLOG=0`. The medians of 3 runs are below. A tick is 100 ms of wall-clock time, and times are averages over the 10 children, in ticks. `tools/compare.py` prints this table.

| Policy | Average rtime | Average wtime |
|---|---|---|
| Round robin | 14 | 156 |
| FCFS | 15 | 131 |
| CFS | 14 | 152 |
| MLFQ | 14 | 157 |

Each spinner runs for about the same time under every policy, so the averages differ only in waiting:

- **Round robin** switches every tick, so the five spinners finish together at the very end, and each waits about 4 times its own run time. A sleeper gets the CPU within a tick of waking.
- **FCFS** runs the spinners one after another, so most finish much earlier and the average is the lowest. The cost shows in the sleepers, which wake while a spinner is running and must wait until it finishes. On an interactive system that wait has no bound; a long job would freeze the shell.
- **CFS** shares the CPU like round robin, in slices of several ticks.
- **MLFQ** comes out like round robin here, because every process is in the same position: all five spinners sink together and share the bottom queue, and boosts bring them back together. Where processes differ, it does not behave like round robin at all, which `schedulertest` cannot show.

So `schedbench` mixes long jobs, short jobs that arrive later, and interactive processes. `README.md` has that table and what it shows.

## Testing

`python3 tests/run.py` builds and boots each policy and checks what the test programs print. For CFS it replays the log. For MLFQ, `tests/mlfqcheck.py` checks the rules against the kernel's tick trace.
