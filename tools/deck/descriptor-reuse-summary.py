#!/usr/bin/env python3
"""Summarise GOW3_DESCRIPTOR_REUSE_STATS=1 runs of a tools/deck/bench-run.sh directory: per run,
how much of each descriptor push the command stream already held, over the same race countdown and
photo-mode windows bench-summary.py measures (the game writes <frames.log>.desc next to the frame
log, one line of cumulative counters per flip; columns in vk_descriptor_reuse.h).
usage: python3 -I descriptor-reuse-summary.py <bench directory>   (bench-summary.py next to it)"""
import importlib.util
import statistics
import sys
from pathlib import Path

_spec = importlib.util.spec_from_file_location('bench_summary',
                                               Path(__file__).with_name('bench-summary.py'))
bench = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bench)


def read_desc(path):
    """Rows of {column: value}, from the header's column names."""
    lines = Path(path).read_text().split('\n')
    if not lines or not lines[0].startswith('#'):
        return []
    names = lines[0][1:].split()
    rows = []
    for line in lines[1:-1]:  # the last element is '' or a line cut by the killing signal
        parts = line.split()
        if len(parts) != len(names) or not all(p.isdigit() for p in parts):
            continue
        rows.append(dict(zip(names, map(int, parts))))
    return rows


def delta(rows, t0, t1):
    inside = [r for r in rows if t0 <= r['ms_of_day'] <= t1]
    if len(inside) < 2:
        return None
    first, last = inside[0], inside[-1]
    d = {k: last[k] - first[k] for k in first}
    d['frames'] = len(inside) - 1
    return d


def pct(n, d):
    return 100.0 * n / d if d else 0.0


def report(d, p):
    """Lines for bind point prefix p ('g_' or 'c_') of the window delta d."""
    g = lambda k: d[p + k]
    binds, push = g('binds'), g('push_binds')
    if not binds:
        return ['  no binds']
    elems, writes, nbytes = g('elems'), g('writes'), g('bytes')
    frames = d['frames']
    out = [
        f"  binds {binds / frames:.0f}/frame (push {pct(push, binds):.1f} %, set {pct(g('set_binds'), binds):.1f} %), "
        f"{elems / max(push, 1):.2f} descriptors and {nbytes / max(push, 1):.0f} copied bytes a push",
        f"  same layout handle as previous push (= same pipeline) {pct(g('same_layout'), push):.1f} %, "
        f"identically defined layout {pct(g('same_sig'), push):.1f} %, stream still valid {pct(g('valid'), push):.1f} %",
        f"  whole set equal to previous push, same layout (old metric) {pct(g('set_equal'), push):.2f} %",
        f"  unchanged by content (no layout/epoch condition): all {pct(g('elem_same'), elems):.1f} %; "
        f"buffers {pct(g('buf_same'), g('buf')):.1f} % (offset-only changes {pct(g('buf_offset'), g('buf')):.1f} %), "
        f"images {pct(g('img_same'), g('img')):.1f} %, samplers {pct(g('samp_same'), g('samp')):.1f} %",
        f"  descriptor mix a push: buffers {g('buf') / max(push, 1):.2f}, images {g('img') / max(push, 1):.2f}, "
        f"samplers {g('samp') / max(push, 1):.2f}, other {g('other') / max(push, 1):.2f}; unsupported writes {g('unsupported')}",
    ]
    for rule, label in (('exact', 'exact layout (gate)'), ('struct', 'identical layout'),
                        ('any', 'any layout (bound)')):
        out.append(f"  avoidable, {label:20s} elements {pct(g(rule + '_elems'), elems):5.1f} %  "
                   f"writes {pct(g(rule + '_writes'), writes):5.1f} %  bytes {pct(g(rule + '_bytes'), nbytes):5.1f} %  "
                   f"pushes with nothing new {pct(g(rule + '_binds'), push):5.1f} %")
    causes = ('raw', 'newcb', 'setfb', 'scene', 'cpass')
    out.append('  exact-rule elements lost to invalidation, % of elements: ' +
               ', '.join(f"{c} {pct(g('lost_' + c), elems):.1f}" for c in causes))
    out.append('  invalidations a frame: ' + ', '.join(f"{c} {g('inv_' + c) / frames:.1f}" for c in causes))
    return out


def main():
    bench_dir = Path(sys.argv[1])
    marks_file = next(iter(sorted(bench_dir.glob('*.marks'))), None)
    if marks_file is None:
        raise SystemExit(f'{bench_dir}: no *.marks file')
    marks = bench.read_marks(marks_file)
    summary = {}
    for run in sorted(d for d in bench_dir.iterdir() if d.is_dir()):
        desc = run / 'frames.log.desc'
        if not desc.exists():
            print(f'{run.name}: no frames.log.desc (GOW3_DESCRIPTOR_REUSE_STATS off?)')
            continue
        start = bench.replay_start(run / 'game.log')
        frames = bench.read_frames(run / 'frames.log')
        if start is None or len(frames) < 2:
            print(f'{run.name}: no replay start or frame log')
            continue
        rows = read_desc(desc)
        c0 = start + marks['countdown_ms']
        windows = {'countdown': (c0 + bench.COUNTDOWN[0], c0 + bench.COUNTDOWN[1])}
        p0 = bench.steady_start(frames, max(start + marks['photo_ms'] - bench.PHOTO_SEARCH,
                                            c0 + bench.COUNTDOWN[1]))
        if p0 is not None:
            windows['photo'] = (p0 + bench.PHOTO[0], p0 + bench.PHOTO[1])
        print(f'== {run.name}')
        for name, (t0, t1) in windows.items():
            d = delta(rows, t0, t1)
            if not d:
                print(f'  {name}: window not in the log')
                continue
            print(f'  -- {name}: {d["frames"]} frames, graphics')
            print('\n'.join(report(d, 'g_')))
            print('  -- compute')
            print('\n'.join(report(d, 'c_')))
            if d['g_elems']:
                summary.setdefault(name, []).append(
                    (pct(d['g_exact_elems'], d['g_elems']), pct(d['g_exact_bytes'], d['g_bytes']),
                     pct(d['g_struct_elems'], d['g_elems']), pct(d['g_struct_bytes'], d['g_bytes'])))
    for name, values in summary.items():
        med = [statistics.median(v[i] for v in values) for i in range(4)]
        print(f'\n{name}, median of {len(values)} runs, graphics avoidable: exact elements {med[0]:.1f} % '
              f'bytes {med[1]:.1f} %; identical layout elements {med[2]:.1f} % bytes {med[3]:.1f} %')


if __name__ == '__main__':
    main()
