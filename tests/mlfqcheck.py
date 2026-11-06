"""Check the course's MLFQ rules against the kernel's tick trace.

A trace is a list of (tick, pid, level): who a tick found on the CPU, and at what
level. A tick is recorded before any boost it causes, so the event at tick 48
still shows the old level, and the boost shows from tick 49.
"""
import re

SLICE = [1, 4, 8, 16]
BOOST = 48


def parse(text):
    """Returns (events, names) from what mlfqtest printed."""
    events = [(int(t), int(p), int(l)) for t, p, l in re.findall(r"^T (\d+) (\d+) (\d+)$", text, re.M)]
    names = {int(p): n for p, n in re.findall(r"^P (\d+) (\w+)$", text, re.M)}
    return events, names


def of(events, pid):
    return [e for e in events if e[1] == pid]


def segment_of(tick):
    """Which stretch between boosts a tick belongs to. The event at tick 48 is the
    end of the stretch before the boost it causes."""
    return (tick - 1) // BOOST


def by_segment(events):
    out = {}
    for e in events:
        out.setdefault(segment_of(e[0]), []).append(e)
    return out


def violations(events, pid):
    """The rules any process must keep, whatever else is running."""
    bad = []
    mine = of(events, pid)
    if not mine:
        return [f"pid {pid} never ran"]
    if mine[0][2] != 0:
        bad.append(f"pid {pid} did not start at the top: {mine[0]}")
    for seg, evs in by_segment(mine).items():
        counts = [0, 0, 0, 0]
        last = 0
        for t, _, level in evs:
            if level < last:
                bad.append(f"pid {pid} rose from level {last} to {level} between boosts, at tick {t}")
            last = max(last, level)
            counts[level] += 1
        for level in range(3):
            if counts[level] > SLICE[level]:
                bad.append(f"pid {pid} used {counts[level]} ticks at level {level}, slice is {SLICE[level]}")
        # after a boost the next tick it is seen must be at the top, unless it has
        # been running in this stretch before that and has already sunk
        if seg > 0 and evs[0][2] != 0:
            bad.append(f"pid {pid} was at level {evs[0][2]} first after the boost before tick {evs[0][0]}")
    return bad


def solo_shape(events, pid):
    """A hog alone keeps exactly to the slices: 1, 4 and 8 ticks, and the rest at the
    bottom until the boost. Only whole stretches can be judged."""
    mine = of(events, pid)
    shapes = []
    segs = by_segment(mine)
    for seg in sorted(segs)[1:-1]:
        shapes.append([sum(1 for e in segs[seg] if e[2] == level) for level in range(4)])
    return shapes


def bottom_runs(events, pids):
    """Each unbroken stretch of ticks a process had at the lowest level, in order.
    A boost between two stretches keeps them apart, even for the same process."""
    runs = []
    last = None
    for t, p, level in events:
        if p not in pids or level != 3:
            continue
        if runs and runs[-1][0] == p and t == last + 1:
            runs[-1][1] += 1
        else:
            runs.append([p, 1])
        last = t
    return runs


def selftest():
    """The checker has to be able to fail. Each of these breaks one rule."""
    good = [(1, 5, 0)] + [(t, 5, 1) for t in range(2, 6)] + [(t, 5, 2) for t in range(6, 14)]
    cases = {
        "good trace passes": (good, True),
        "two ticks at level 0": ([(1, 5, 0), (2, 5, 0)], False),
        "five ticks at level 1": ([(1, 5, 0)] + [(t, 5, 1) for t in range(2, 7)], False),
        "nine ticks at level 2": ([(1, 5, 0)] + [(t, 5, 2) for t in range(2, 11)], False),
        "starts low": ([(1, 5, 2)], False),
        "rises with no boost": ([(1, 5, 0), (2, 5, 1), (3, 5, 0)], False),
        "no boost happened": ([(1, 5, 0), (50, 5, 3)], False),
    }
    return {name: bool(violations(trace, 5)) != ok for name, (trace, ok) in cases.items()}
