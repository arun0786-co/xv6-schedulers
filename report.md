# xv6 Operating System Modifications Report

## Overview
This report documents the implementation of a system call for tracking read operations and three different CPU scheduling algorithms in the xv6 operating system: Round Robin (default), First Come First Serve (FCFS), and Completely Fair Scheduler (CFS).

## Part A: getreadcount System Call

### Implementation Details

#### A.1: System Call Implementation
The `getreadcount()` system call tracks and returns the total number of bytes read by the `read()` system call across all processes since boot.

**Files Modified:**
- `kernel/syscall.h`: Added `SYS_getreadcount` system call number
- `kernel/syscall.c`: Added function declaration and system call mapping
- `kernel/sysfile.c`: 
  - Added global variable `total_read_bytes` to track total bytes read
  - Modified `sys_read()` to increment counter when bytes are successfully read
  - Implemented `sys_getreadcount()` to return the current count
- `user/usys.pl`: Added system call entry point
- `user/user.h`: Added function declaration for user space

**Key Features:**
- Overflow handling: The counter wraps around to 0 when it reaches maximum value
- Thread-safe: Uses existing locking mechanisms in the read system call
- Only counts successful reads (bytes_read > 0)

#### A.2: User Program
Created `user/readcount.c` that:
- Calls `getreadcount()` and prints the initial value
- Reads 100 bytes from a file (README)
- Calls `getreadcount()` again and verifies the increase
- Provides clear success/failure feedback

**Testing:**
```bash
make clean
make
# Run xv6 and execute: readcount
```

## Part B: CPU Scheduling Algorithms

### B.1: Makefile Modifications
Added support for the `SCHEDULER` macro to compile different scheduling algorithms:
```makefile
# Scheduler macro support
ifdef SCHEDULER
CFLAGS += -DSCHEDULER_$(SCHEDULER)
endif
```

**Usage:**
- `make SCHEDULER=FCFS` - Compile with First Come First Serve scheduler
- `make SCHEDULER=CFS` - Compile with Completely Fair Scheduler
- `make` - Compile with default Round Robin scheduler

### B.2: Process Structure Modifications
Extended the `struct proc` in `kernel/proc.h` with scheduling fields:
```c
// Scheduling fields
uint64 creation_time;        // Process creation time for FCFS
int nice;                    // Nice value for CFS (-20 to 19)
uint64 vruntime;             // Virtual runtime for CFS
uint64 weight;               // Process weight for CFS
uint64 last_run_time;        // Last time process ran
```

### B.3: First Come First Serve (FCFS) Scheduler

**Implementation:**
- Modified `scheduler()` function in `kernel/proc.c`
- Selects the process with the earliest `creation_time`
- Non-preemptive: processes run until completion
- Uses global time counter for process creation timestamps

**Key Features:**
- Processes are scheduled based on arrival time
- No preemption (processes run to completion)
- Simple and predictable scheduling

**Algorithm:**
1. Iterate through all processes
2. Find the RUNNABLE process with the smallest `creation_time`
3. Schedule that process and run it until completion

### B.4: Completely Fair Scheduler (CFS)

**Implementation:**
- Comprehensive CFS implementation with priority support
- Virtual runtime tracking for fair CPU distribution
- Dynamic time slice calculation
- Detailed logging for verification

#### B.4.1: Priority Support
- Nice values range from -20 (highest priority) to 19 (lowest priority)
- Weight calculation: `weight = 1024 / (1.25 ^ nice)`
- Precomputed weight table for efficiency:
  - Nice -20: weight = 88761 (highest priority)
  - Nice 0: weight = 1365 (default)
  - Nice 19: weight = 26 (lowest priority)

#### B.4.2: Virtual Runtime Tracking
- Each process has a `vruntime` field representing normalized CPU time
- Updated after each time slice: `vruntime += (run_time * 1024) / weight`
- Processes with lower vruntime have higher priority

#### B.4.3: Scheduling Algorithm
- Always schedules the process with the smallest vruntime
- Maintains fairness by tracking how much CPU time each process has received
- Processes are inserted in correct position based on vruntime when becoming runnable

#### B.4.4: Time Slice Calculation
- Target latency: 48 ticks
- Time slice = target_latency / number_of_runnable_processes
- Minimum time slice: 3 ticks
- Dynamic adjustment based on system load

#### B.4.5: Logging and Verification
Comprehensive logging system that prints:
```
[Scheduler Tick]
PID: 3 | vRuntime: 200
PID: 4 | vRuntime: 150
PID: 5 | vRuntime: 180
--> Scheduling PID 4 (lowest vRuntime)
```

### B.5: Multi-Level Feedback Queue (MLFQ) Scheduler (Bonus)

**Implementation:**
- Four priority queues: 0 (highest) → 3 (lowest)
- Time slices: Queue 0 → 1 tick, Queue 1 → 4 ticks, Queue 2 → 8 ticks, Queue 3 → 16 ticks
- Comprehensive MLFQ algorithm with starvation prevention

#### B.5.1: Queue Management
- New processes start in queue 0 (highest priority)
- Processes are demoted to lower queues when time slice expires
- Voluntary yield (I/O bound) keeps process in same queue
- Round-robin scheduling within lowest priority queue

#### B.5.2: Starvation Prevention
- Every 48 ticks, all processes are boosted to queue 0
- Prevents long-running processes from being permanently starved
- Ensures fair CPU distribution over time

#### B.5.3: Scheduling Rules
- Always schedule from highest non-empty queue
- Preemption occurs at tick boundaries
- Time slice expiry triggers queue demotion
- Comprehensive logging for queue transitions

**Key Features:**
- Adaptive priority adjustment based on process behavior
- I/O bound processes maintain higher priority
- CPU bound processes gradually move to lower queues
- Prevents starvation through periodic boosting

### B.6: Global Time Management
- Added `global_time` counter incremented on each timer interrupt
- Used for process creation timestamps and runtime calculations
- Ensures consistent time tracking across all scheduling algorithms

## Implementation Files Summary

### Modified Files:
1. **kernel/syscall.h** - Added system call number
2. **kernel/syscall.c** - Added system call mapping
3. **kernel/sysfile.c** - Implemented read tracking and getreadcount
4. **kernel/proc.h** - Extended process structure
5. **kernel/proc.c** - Implemented schedulers and helper functions
6. **kernel/trap.c** - Added global time increment
7. **user/usys.pl** - Added user space interface
8. **user/user.h** - Added function declaration
9. **user/readcount.c** - Created test program
10. **Makefile** - Added scheduler macro support

### New Files:
- `user/readcount.c` - Test program for getreadcount system call

## Compilation and Testing

### Build Commands:
```bash
# Default Round Robin
make clean && make

# First Come First Serve
make clean && make SCHEDULER=FCFS

# Completely Fair Scheduler
make clean && make SCHEDULER=CFS

# Multi-Level Feedback Queue (Bonus)
make clean && make SCHEDULER=MLFQ
```

### Testing:
1. **System Call Test:**
   - Run xv6: `make qemu`
   - Execute: `readcount`
   - Verify output shows initial count, bytes read, and final count

2. **Scheduler Testing:**
   - Run xv6 with different schedulers
   - Use `Ctrl+P` to see process information
   - For CFS, observe the detailed logging output

## Performance Characteristics

### Round Robin (Default)
- **Advantages:** Simple, fair, prevents starvation
- **Disadvantages:** Context switching overhead, not optimal for I/O bound processes
- **Use Case:** General purpose, balanced workload

### First Come First Serve (FCFS)
- **Advantages:** Simple, predictable, minimal overhead
- **Disadvantages:** Can cause convoy effect, poor for I/O bound processes
- **Use Case:** Batch processing, simple systems

### Completely Fair Scheduler (CFS)
- **Advantages:** Fair CPU distribution, handles mixed workloads well, priority support
- **Disadvantages:** More complex, higher overhead
- **Use Case:** Interactive systems, mixed CPU and I/O workloads

### Multi-Level Feedback Queue (MLFQ)
- **Advantages:** Adaptive to process behavior, excellent for mixed workloads, prevents starvation
- **Disadvantages:** Complex implementation, requires careful tuning
- **Use Case:** Interactive systems, servers with mixed CPU and I/O workloads

