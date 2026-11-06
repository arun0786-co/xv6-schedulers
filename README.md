# Schedulers for xv6

Round robin, FCFS, a Completely Fair Scheduler and a multilevel feedback queue for the RISC-V xv6 teaching kernel, with the tools to see what each one really does. It began as part of the Operating Systems and Networks course at IIIT Hyderabad, and goes further than the assignment.

```
make qemu SCHEDULER=MLFQ CPUS=1    # RR (default), FCFS, CFS or MLFQ. Ctrl-a x quits
```

You need a RISC-V gcc (`riscv64-unknown-elf-gcc` or `riscv64-linux-gnu-gcc`) and `qemu-system-riscv64`. The repo holds the whole kernel. Upstream's code is imported unchanged in the history, so `git diff de05c9d HEAD -- kernel user Makefile` shows everything changed from it.

## The schedulers

| Policy | How it chooses | Preempts |
|---|---|---|
| Round robin | the next runnable process in the table | every tick |
| FCFS | the earliest created | never: runs until it exits or sleeps |
| CFS | the lowest `vruntime`, from a sorted run queue, with nice values weighting how fast it grows | when its slice of a 48 tick latency is used |
| MLFQ | the front of the highest non-empty of 4 queues | when its slice is used, or at the next tick if a higher queue has work |

**The MLFQ rules:** slices of 1, 4, 8 and 16 ticks. A new process starts at the top. Using a whole slice moves a process down a queue. Going to sleep keeps its level. The bottom queue is round robin. Every 48 ticks, everything goes back to the top.

The kernel also has `getreadcount()`, `waitx()`, `setnice()`, and two additions for looking inside: `waitstat()`, which reports when a process first ran and how often it was switched, and `schedtrace()`, a record of who was on the CPU at each tick.

## What they do

`tools/compare.py` built each policy and printed these, on 1 CPU in qemu, each the median of 3 runs, in ticks. `schedbench` is 4 long jobs, 4 short jobs arriving later, and 3 interactive processes that work a little and sleep. Turnaround is creation to exit. Response is creation to first time on a CPU.

| Policy | Long: turnaround | Long: times on CPU | Short: turnaround | Short: response | Interactive: turnaround | Interactive: response |
|---|---|---|---|---|---|---|
| Round robin | 250 | 60 | 25 | 4 | 119 | 4 |
| FCFS | 144 | 1 | 95 | 92 | 335 | 231 |
| CFS | 244 | 6 | 3 | 0 | 274 | 24 |
| MLFQ | 272 | 32 | 12 | 0 | 104 | 4 |

- **FCFS** has the best turnaround for long jobs and is terrible for everything else: a short job waits 92 ticks to start.
- **Round robin** responds quickly, and pays for it by switching long jobs nearly twice as often as MLFQ.
- **CFS** is best for short jobs here, for a reason worth knowing: a new process starts with `vruntime` 0, as the assignment says, so it runs ahead of everything that has been going for a while. Linux avoids this by starting new processes near the current minimum.
- **MLFQ** gives interactive processes the best turnaround, with no knowledge of which processes are which: it learns from how they behave. Long jobs pay for that.

## Seeing it

`mlfqtest` runs a workload with the kernel's trace on, and `tools/timeline.py` draws what the trace saw. A digit is the level a process ran at in that tick.

A process alone, sinking through the levels. At each `|`, a boost sends it back to the top:

```
                                                  |
hog 0111122222222333333333333333333333333333333333301111222222223333333333
```

A new process arriving while an old one is at the bottom. It takes over at the next tick, and the old one waits until the new one is finished:

```
old 011112222222233333333333333333......3333333333
new ..............................011112..........
```

## Cheating the scheduler

The course's rule, like the textbook's first version of MLFQ, says a process that sleeps before its slice ends keeps its level. That can be cheated. A process wakes just after a tick, runs for most of the time to the next, and sleeps before it arrives. Ticks only see who is running when they land, so no tick ever charges it, and it stays at the top. `mlfqtest game` runs one of these against two honest processes:

| Rule | Cheater's share of the work |
|---|---|
| the course's | 67% |
| `MLFQ_FAIR=1`, charging real time | 29% |

A fair share is 33%. `make qemu SCHEDULER=MLFQ MLFQ_FAIR=1` counts the clock time a process uses at a level, whenever it sleeps, instead of counting the ticks that happened to find it running. `kernel/proc.c` has the details, in `mlfq_charge`.

## Tests

```
python3 tests/run.py    # several minutes
```

It builds each policy and boots it in qemu.

`tests/mlfqcheck.py` is the part I would point at. It reads the kernel's trace and checks the MLFQ rules from outside: no level used for longer than its slice, no rising between boosts, a new process starting at the top, a higher queue taking over at the next tick. It has a self-test with broken traces that it must reject, since a checker that cannot fail proves nothing. It found a real bug: a process interrupted by a higher queue got a fresh slice when it came back, so a neighbour waking every tick kept it from ever sinking.

## Report

`report.md` is the course's report: how each part was built, the CFS log, and measurements.
