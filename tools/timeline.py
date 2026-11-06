#!/usr/bin/env python3
"""Draw what mlfqtest's trace saw: one row per process, one column per tick.

usage: tools/timeline.py [console log]   (or from stdin)

A digit is the level the process ran at in that tick, and a dot means another
process had the CPU.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))
import mlfqcheck  # noqa: E402


def draw(events, names, width=96):
    if not events:
        return "(no trace)\n"
    start = min(e[0] for e in events)
    end = max(e[0] for e in events)
    lines = []
    pids = [p for p in names if any(e[1] == p for e in events)]
    label = max(len(names[p]) for p in pids) if pids else 4
    at = {(t, p): level for t, p, level in events}
    for left in range(start, end + 1, width):
        right = min(left + width, end + 1)
        ruler = "".join("|" if t % mlfqcheck.BOOST == 0 else " " for t in range(left, right))
        lines.append(" " * (label + 1) + ruler + f"   ticks {left} to {right - 1}, | is a boost")
        for p in pids:
            row = "".join(str(at[(t, p)]) if (t, p) in at else "." for t in range(left, right))
            lines.append(f"{names[p]:<{label}} {row}")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    text = open(sys.argv[1], errors="replace").read() if len(sys.argv) > 1 else sys.stdin.read()
    print(draw(*mlfqcheck.parse(text.replace("\r", ""))), end="")
