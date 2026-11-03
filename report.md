# xv6: getreadcount, FCFS and CFS

`xv6_modifications.patch` applies to
[mit-pdos/xv6-riscv](https://github.com/mit-pdos/xv6-riscv) at commit
`9dbd536` (10 September 2025):

```
git clone https://github.com/mit-pdos/xv6-riscv && cd xv6-riscv
git checkout 9dbd536
git apply ../xv6_modifications.patch
make qemu SCHEDULER=CFS CPUS=1
```

Build options:

| Option | Values | Effect |
|---|---|---|
| `SCHEDULER` | `RR` (default), `FCFS`, `CFS` | scheduling policy; any other value is an error |
| `CPUS` | 1 to 8, default 3 | number of CPUs qemu gives xv6 |
| `CFSLOG` | `1` (default), `0` | CFS prints its run queue before every decision |
| `READCOUNT_START` | a number | starting value of the read counter, to test the wrap-around |

The options are compiler flags, which make cannot see change, so the
Makefile writes them to `kernel/kernel.options` and rebuilds the kernel
whenever they differ from the last build. `make clean` between policies is
not needed.

New user programs: `readcount`, `readrace`, `schedulertest`, `nicetest`.
New system calls: `getreadcount`, `waitx`, `setnice`.

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

## Performance comparison

`schedulertest`, 3 runs per policy, on qemu 10.1 on an Apple silicon Mac,
CFS built with `CFSLOG=0`. A tick is 100 ms of wall-clock time. Times are
averages over the 10 children, in ticks.

One CPU (`CPUS=1`), as the spec asks:

| Policy | Average rtime | Average wtime |
|---|---|---|
| Round robin | 22, 22, 22 | 188, 190, 189 |
| FCFS | 22, 22, 22 | 155, 159, 158 |
| CFS | 22, 22, 22 | 186, 189, 189 |

Each child, first run (children 0 to 4 sleep, 5 to 9 spin):

| Child | RR wtime | FCFS wtime | CFS wtime |
|---|---|---|---|
| 0 to 4 (sleep) | 200 each | 222 each | 204 each |
| 5 | 177 | 0 | 160 |
| 6 | 177 | 44 | 164 |
| 7 | 177 | 88 | 169 |
| 8 | 178 | 132 | 173 |
| 9 | 177 | 177 | 179 |

Each spinner runs for about 44 ticks under every policy, so the averages
differ only in waiting:

- **Round robin** switches every tick, so the five spinners finish together
  at the very end and each waits about 4 times its own run time. A sleeper
  gets the CPU within a tick of waking, so its `wtime` is just its 200-tick
  sleep.
- **FCFS** runs the spinners one after another: child 5 waits 0 ticks,
  child 6 waits for child 5, and so on. Most of them finish much earlier
  than under round robin, so the average is the lowest of the three. The
  cost shows in the sleepers: they wake at tick 200 while child 9 is
  running and must wait until it finishes at 222. On an interactive system
  that wait has no bound; a long job would freeze the shell.
- **CFS** shares the CPU like round robin, so its average is close, but in
  slices of several ticks (48 divided by the number of RUNNABLE processes)
  instead of 1, so the spinners finish a slice or so apart. A sleeper that
  wakes waits for the current slice to end (204).

Three CPUs (`CPUS=3`), for comparison:

| Policy | Average rtime | Average wtime |
|---|---|---|
| Round robin | 30, 28, 29 | 119, 118, 119 |
| FCFS | 30, 27, 29 | 113, 112, 112 |
| CFS | 30, 28, 28 | 119, 118, 118 |

With 5 spinners on 3 CPUs there is less waiting under every policy. The
3-CPU runs of the three policies ran on the host at the same time, so each
virtual CPU was slower: a spinner took 48 to 70 ticks instead of about 45.

## Testing

`python3 test.py` clones xv6, applies the patch, and builds and boots each
policy in qemu. It checks:

- `readcount`, `readrace` on 3 CPUs, and the wrap-around
- that the spinners share the CPU under round robin and CFS, and run one
  after another under FCFS
- that under FCFS a process that wakes up waits for the running one
- the `nicetest` shares
- every block of the CFS log: well formed, the queue sorted by `vruntime`,
  the first process chosen, and `vruntime` growing

It passes on Linux. Two deliberately broken kernels fail it: FCFS that
preempts every tick, and CFS whose `vruntime` never grows.

xv6's own `usertests -q` passes on 3 CPUs under round robin and CFS. Under
FCFS every test passes when run on its own except `preempt`, which never
finishes. That test starts processes that spin forever on all three CPUs and
then needs a CPU to kill them, which a non-preemptive scheduler never gives
it.
