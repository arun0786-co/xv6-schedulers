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

    print("%d failed" % failures if failures else "all passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
