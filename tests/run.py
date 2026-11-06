#!/usr/bin/env python3
"""Build each scheduler and check it in qemu.

usage: tests/run.py

Boots each policy and checks what the test programs print. Needs make, a RISC-V
gcc (riscv64-unknown-elf- or riscv64-linux-gnu-) and qemu-system-riscv64.
"""
import os
import re
import select
import subprocess
import sys
import time

SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(SRC, "tests"))
sys.path.insert(0, os.path.join(SRC, "tools"))
import mlfqcheck  # noqa: E402
import timeline  # noqa: E402

# One block of the CFS log, and any single line of it. On one CPU a block is
# printed with interrupts off, so nothing else lands inside it.
BLOCK = re.compile(r"\[Scheduler Tick\]\n((?:PID: \d+ \| vRuntime: \d+\n)+)"
                   r"--> Scheduling PID (\d+) \(lowest vRuntime\)\n")
LOGLINE = re.compile(r"\[Scheduler Tick\]\n|PID: \d+ \| vRuntime: \d+\n|"
                     r"--> Scheduling PID \d+ \(lowest vRuntime\)\n")
CHILD = re.compile(r"child (\d+) \((sleeps|spins), pid \d+\): rtime (\d+), wtime (\d+)")

failures = 0


def check(name, ok, detail=""):
    global failures
    if ok:
        print("ok   " + name)
    else:
        failures += 1
        print("FAIL " + name + (": " + detail if detail else ""))


def build(**opts):
    args = ["make", "-C", SRC, "kernel/kernel", "fs.img"]
    args += ["%s=%s" % kv for kv in sorted(opts.items())]
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        sys.stdout.write(r.stdout.decode(errors="replace")[-3000:])
        sys.exit("build failed: " + " ".join(args[4:]))


def boot(cpus, commands, limit=600):
    """Boot the last build, type each command at the shell prompt, return
    everything the console printed with the CFS log lines left in."""
    qemu = subprocess.Popen(
        ["qemu-system-riscv64", "-machine", "virt", "-bios", "none",
         "-kernel", "kernel/kernel", "-m", "128M", "-smp", str(cpus),
         "-nographic", "-global", "virtio-mmio.force-legacy=false",
         "-drive", "file=fs.img,if=none,format=raw,id=x0",
         "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0"],
        cwd=SRC, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT)
    out = b""

    def prompt(secs):
        nonlocal out
        start = len(out)
        end = time.time() + secs
        while time.time() < end:
            if select.select([qemu.stdout], [], [], 0.2)[0]:
                data = os.read(qemu.stdout.fileno(), 65536)
                if not data:
                    return False
                out += data
                text = out[start:].decode(errors="replace").replace("\r", "")
                if "$ " in LOGLINE.sub("", text):
                    return True
        return False

    try:
        ok = prompt(30)
        for cmd in commands:
            if not ok:
                break
            qemu.stdin.write(cmd.encode() + b"\n")
            qemu.stdin.flush()
            ok = prompt(limit)
    finally:
        qemu.kill()
        qemu.wait()
    text = out.decode(errors="replace").replace("\r", "")
    if not ok:
        print(text[-2000:])
        sys.exit("xv6 did not come back to the prompt")
    return text


def children(text):
    """The per-child lines of schedulertest -v, by child number."""
    return {int(i): (kind, int(r), int(w))
            for i, kind, r, w in CHILD.findall(LOGLINE.sub("", text))}


def spinners_overlap(kids):
    # with 5 equal spinners sharing one CPU, each waits about 4 times as
    # long as it runs
    spin = [v for v in kids.values() if v[0] == "spins"]
    return len(spin) == 5 and all(w >= 3 * r for _, r, w in spin), str(spin)


def runs_of(out):
    """Each mlfqtest's output, by the name it was given."""
    return out.split("mlfqtest done")


def pid_named(names, want):
    return next(p for p, n in names.items() if n == want)


def test_mlfq():
    for name, good in mlfqcheck.selftest().items():
        check("mlfq checker: " + name, good)

    build(SCHEDULER="MLFQ", CFSLOG="0")
    out = boot(1, ["mlfqtest solo", "mlfqtest two", "mlfqtest io", "mlfqtest arrive"])
    solo, two, io, arrive = runs_of(out)[:4]

    events, names = mlfqcheck.parse(solo)
    hog = pid_named(names, "hog")
    bad = mlfqcheck.violations(events, hog)
    check("mlfq: a process alone keeps the rules", not bad, "; ".join(bad[:3]))
    shapes = mlfqcheck.solo_shape(events, hog)
    check("mlfq: between boosts it runs 1, 4 and 8 ticks and sinks to the bottom",
          shapes and all(sh == [1, 4, 8, 35] for sh in shapes), str(shapes))
    print(timeline.draw(events, names, width=96), end="")

    events, names = mlfqcheck.parse(two)
    a, b = pid_named(names, "hogA"), pid_named(names, "hogB")
    bad = mlfqcheck.violations(events, a) + mlfqcheck.violations(events, b)
    check("mlfq: two processes keep the rules", not bad, "; ".join(bad[:3]))
    runs = mlfqcheck.bottom_runs(events, {a, b})
    check("mlfq: the bottom queue takes turns",
          len(runs) >= 4 and {r[0] for r in runs} == {a, b}, str(runs))
    check("mlfq: a turn at the bottom is at most 16 ticks", all(r[1] <= 16 for r in runs), str(runs))
    check("mlfq: and a turn there can be the whole 16", any(r[1] == 16 for r in runs), str(runs))

    events, names = mlfqcheck.parse(io)
    hog = pid_named(names, "hog")
    inter = pid_named(names, "interactive")
    hog_low = max(e[2] for e in mlfqcheck.of(events, hog))
    seen = mlfqcheck.of(events, inter)
    check("mlfq: the process that uses the CPU sinks", hog_low == 3, str(hog_low))
    check("mlfq: the one that sleeps stays near the top", all(e[2] < 3 for e in seen),
          f"{len(seen)} ticks seen, levels {sorted(set(e[2] for e in seen))}")

    events, names = mlfqcheck.parse(arrive)
    old, new = pid_named(names, "old"), pid_named(names, "new")
    # the parent sleeps in pause() meanwhile, which wakes it on every tick and so
    # interrupts the hog on every tick. that must not give the hog a fresh slice.
    bad = mlfqcheck.violations(events, old)
    check("mlfq: being interrupted does not restart a slice", not bad, "; ".join(bad[:3]))
    new_ev = mlfqcheck.of(events, new)
    start = new_ev[0][0]
    before = [e for e in mlfqcheck.of(events, old) if e[0] < start]
    check("mlfq: a new process starts at the top", new_ev[0][2] == 0, str(new_ev[0]))
    check("mlfq: the one it displaces had sunk", before and before[-1][2] == 3, str(before[-1:]))
    check("mlfq: it takes over at the next tick", before and start - before[-1][0] == 1,
          f"old last seen at {before[-1][0]}, new first at {start}")
    during = [e for e in mlfqcheck.of(events, old) if start <= e[0] < start + 5]
    check("mlfq: the lower one waits while a higher one runs", not during, str(during))

    # The cheater sleeps just before each tick, so no tick ever charges it, and the
    # course's rule keeps it at the top. Real time used cannot be dodged like that.
    plain = cheat_shares(MLFQ_FAIR="0")
    fair = cheat_shares(MLFQ_FAIR="1")
    print(f"     cheater's share of the work: {plain:.0%} with the course's rule, {fair:.0%} charging real time")
    check("mlfq: the course's rule can be cheated", plain > 0.5, f"{plain:.0%}")
    check("mlfq: charging real time stops it", fair < 0.45, f"{fair:.0%}")


def cheat_shares(**opts):
    build(SCHEDULER="MLFQ", CFSLOG="0", **opts)
    out = boot(1, ["mlfqtest game"])
    rows = re.findall(r"^W (\w+) \d+ (\d+)$", out, re.M)
    total = sum(int(n) for _, n in rows)
    return sum(int(n) for who, n in rows if who == "cheater") / total if total else 0.0


def bench(**opts):
    build(CFSLOG="0", **opts)
    out = boot(1, ["schedbench"], limit=900)
    rows = re.findall(r"^R (\w+) (\d+) (\d+) (\d+) (\d+)$", out, re.M)
    return {r[0]: dict(turnaround=int(r[1]), response=int(r[2]), worst=int(r[3]), switches=int(r[4])) for r in rows}


def test_compare():
    results = {name: bench(SCHEDULER=name) for name in ("RR", "FCFS", "CFS", "MLFQ")}
    print("     policy  long turn  long switches  short turn  short resp  inter turn  inter resp  inter worst")
    for name, r in results.items():
        print(f"     {name:6} {r['long']['turnaround']:10} {r['long']['switches']:14} {r['short']['turnaround']:11}"
              f" {r['short']['response']:11} {r['interactive']['turnaround']:11} {r['interactive']['response']:11}"
              f" {r['interactive']['worst']:12}")
    check("compare: every policy finished", all(len(r) == 3 for r in results.values()), str(results))
    f, m = results["FCFS"], results["MLFQ"]
    check("compare: short jobs finish far sooner under mlfq than fcfs",
          m["short"]["turnaround"] * 2 < f["short"]["turnaround"],
          f"{m['short']['turnaround']} against {f['short']['turnaround']}")
    check("compare: short jobs start sooner under mlfq than fcfs",
          m["short"]["response"] < f["short"]["response"],
          f"{m['short']['response']} against {f['short']['response']}")


def main():
    # Round robin, 3 CPUs: getreadcount, also under concurrent reads.
    build(SCHEDULER="RR")
    out = boot(3, ["readcount", "readrace 3 20000"])
    check("readcount sees its 100 bytes",
          "OK: the count grew by 100 after reading 100 bytes" in out)
    check("readrace: no read is lost on 3 CPUs",
          "readrace: expected 60000, counted 60000" in out,
          re.search(r"readrace: .*", out).group(0) if "readrace:" in out else "")

    # The counter wraps around to 0.
    build(SCHEDULER="RR", READCOUNT_START="4294967200")
    out = boot(1, ["readcount"])
    m = re.search(r"read count before: (\d+)\nread count after:  (\d+)", out)
    check("the read count wraps around to 0",
          m is not None and int(m.group(1)) >= 4294967200 and
          int(m.group(2)) < 100000 and "OK: the count grew by 100" in out,
          m.group(0) if m else "")

    # Round robin, 1 CPU: the spinners share the CPU.
    out = boot(1, ["schedulertest -v"])
    ok, detail = spinners_overlap(children(out))
    check("rr: the spinners share one CPU", ok, detail)

    # FCFS, 1 CPU: each spinner waits for all the earlier ones to finish.
    build(SCHEDULER="FCFS")
    out = boot(1, ["schedulertest -v"])
    kids = children(out)
    spin = [kids[i] for i in range(5, 10) if i in kids]
    serial = len(spin) == 5 and all(
        spin[k + 1][2] >= spin[k][2] + spin[k][1] - 2 for k in range(4))
    check("fcfs: the spinners run one after another, in creation order",
          serial, str(spin))
    # The sleepers wake after 200 ticks. If a spinner is running then, they
    # must wait for it to finish: FCFS does not preempt.
    running = [w + r for _, r, w in spin if w < 198 and w + r > 202]
    woke = [w for kind, _, w in kids.values() if kind == "sleeps"]
    if running:
        check("fcfs: a process that wakes up waits for the running one",
              len(woke) == 5 and all(w >= running[0] - 2 for w in woke),
              "spinner ends at %d, sleepers %s" % (running[0], woke))
    else:
        print("skip fcfs wake-up check: no spinner was running at tick 200")

    # CFS, 1 CPU, with the log.
    build(SCHEDULER="CFS", CFSLOG="1")
    out = boot(1, ["nicetest", "schedulertest -v"])
    check("cfs: CPU time follows the nice weights", "nicetest: OK" in out,
          "; ".join(re.findall(r"nice -?\d+: .*", out)))
    ok, detail = spinners_overlap(children(out))
    check("cfs: the spinners share one CPU", ok, detail)
    blocks = BLOCK.findall(out)
    bad = []
    for listing, chosen in blocks:
        rows = [(int(p), int(v)) for p, v in
                re.findall(r"PID: (\d+) \| vRuntime: (\d+)", listing)]
        vr = [v for _, v in rows]
        if rows[0][0] != int(chosen) or vr != sorted(vr):
            bad.append(listing + chosen)
    check("cfs log: %d blocks, each well formed" % len(blocks),
          len(blocks) > 0 and len(blocks) == out.count("[Scheduler Tick]"))
    check("cfs log: the queue is sorted and the lowest vruntime is chosen",
          len(blocks) > 0 and not bad, bad[0] if bad else "")
    grew = set()
    for listing, chosen in blocks:
        for p, v in re.findall(r"PID: (\d+) \| vRuntime: (\d+)", listing):
            grew.add(int(v))
    check("cfs log: vruntime grows as processes run", len(grew) > 5)

    test_mlfq()
    test_compare()

    print("%d failed" % failures if failures else "all passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
