#!/usr/bin/env python3
"""Build each policy, run the same workloads on one CPU, print markdown tables.

usage: tools/compare.py [runs per policy, default 3]

Each number is the median of the runs. Needs what tests/run.py needs.
"""
import os
import re
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 3
sys.argv = sys.argv[:1]
import run  # noqa: E402
import mlfqcheck  # noqa: E402
import timeline  # noqa: E402

POLICIES = ["RR", "FCFS", "CFS", "MLFQ"]
NAMES = {"RR": "Round robin", "FCFS": "FCFS", "CFS": "CFS", "MLFQ": "MLFQ"}


def med(values):
    return int(statistics.median(values))


def main():
    print(f"`schedulertest` and `schedbench` on 1 CPU, median of {RUNS} runs, in ticks.\n")
    simple, mixed = {}, {}
    for p in POLICIES:
        run.build(SCHEDULER=p, CFSLOG="0")
        out = run.boot(1, ["schedulertest"] * RUNS + ["schedbench"] * RUNS, limit=900)
        simple[p] = [(int(r), int(w)) for r, w in re.findall(r"Average rtime (\d+),\s+wtime (\d+)", out)]
        rows = re.findall(r"^R (\w+) (\d+) (\d+) (\d+) (\d+)$", out, re.M)
        by = {}
        for kind, turn, resp, worst, sw in rows:
            by.setdefault(kind, []).append((int(turn), int(resp), int(worst), int(sw)))
        mixed[p] = by
        print(f"<!-- {p}: {len(simple[p])} schedulertest, {len(rows)} schedbench rows -->", flush=True)

    print("\n**`schedulertest`**: 5 processes that sleep and 5 that spin.\n")
    print("| Policy | Average rtime | Average wtime |\n|---|---|---|")
    for p in POLICIES:
        print(f"| {NAMES[p]} | {med(r for r, _ in simple[p])} | {med(w for _, w in simple[p])} |")

    print("\n**`schedbench`**: 4 long jobs, 4 short jobs arriving later, and 3 interactive processes.\n")
    print("| Policy | Long: turnaround | Long: times on CPU | Short: turnaround | Short: response "
          "| Interactive: turnaround | Interactive: response |")
    print("|---|---|---|---|---|---|---|")
    for p in POLICIES:
        b = mixed[p]
        print(f"| {NAMES[p]} | {med(x[0] for x in b['long'])} | {med(x[3] for x in b['long'])} "
              f"| {med(x[0] for x in b['short'])} | {med(x[1] for x in b['short'])} "
              f"| {med(x[0] for x in b['interactive'])} | {med(x[1] for x in b['interactive'])} |")

    print("\n**The cheater's share of the work**, against two honest processes. A fair share is 33%.\n")
    print("| Rule | Cheater's share |\n|---|---|")
    for label, fair in (("the course's", "0"), ("`MLFQ_FAIR=1`, charging real time", "1")):
        shares = []
        run.build(SCHEDULER="MLFQ", CFSLOG="0", MLFQ_FAIR=fair)
        out = run.boot(1, ["mlfqtest game"] * RUNS, limit=300)
        rows = re.findall(r"^W (\w+) \d+ (\d+)$", out, re.M)
        for i in range(0, len(rows), 3):
            part = rows[i:i + 3]
            total = sum(int(n) for _, n in part)
            shares.append(sum(int(n) for w, n in part if w == "cheater") / total)
        print(f"| {label} | {statistics.median(shares):.0%} |", flush=True)

    print("\n**What the kernel's trace saw** under MLFQ. A digit is the level a process ran at in that tick.\n")
    run.build(SCHEDULER="MLFQ", CFSLOG="0")
    out = run.boot(1, ["mlfqtest solo", "mlfqtest arrive"], limit=300)
    solo, arrive = run.runs_of(out)[:2]
    print("A process alone:\n\n```")
    print(timeline.draw(*mlfqcheck.parse(solo), width=70), end="")
    print("```\n\nA new process arriving while an old one is at the bottom:\n\n```")
    print(timeline.draw(*mlfqcheck.parse(arrive), width=70), end="")
    print("```")


if __name__ == "__main__":
    main()
