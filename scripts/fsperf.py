#!/usr/bin/env python3
"""Filesystem metadata and data performance, measured through real syscalls.

    scripts/fsperf.py DIR [--threads N] [--files N] [--seconds S] [--json OUT]
                          [--only GROUPS] [--skip GROUPS] [--compare BASE.json]

DIR is any directory on the filesystem under test — a local one, or an NFS mount served
by lightnfsd.  Everything is ordinary open/read/write/stat/rename/unlink, so what comes
out is what an application sees: the whole stack, client page cache included.  That is
the point of measuring here rather than with `lightnfs-ctl bench`, which reports the
protocol stack's own ceiling over a memory backend and never touches a mount.

Two families:
  metadata  create / stat / open+close / chmod / rename / lookup-miss / readdir /
            unlink / mkdir+rmdir / symlink+readlink / link — ops per second and the
            latency distribution of each
  data      sequential write, sequential read, re-read (cache), random read, random
            write, and the cost of fsync — at several block sizes, in MiB/s and IOPS

Numbers from one run mean little.  Take a baseline with --json, then --compare it:
a run is only comparable against one from the same machine, filesystem and mount.
"""
import argparse
import errno
import json
import math
import mmap
import os
import random
import shutil
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

NS = 1_000_000_000

# ---- measurement ---------------------------------------------------------------------


class Samples:
    """Latencies of one operation, in nanoseconds, plus the wall time it all took."""

    # Percentiles stay honest up to this many samples; past it we keep counting but stop
    # recording, and say so.
    CAP = 2_000_000

    def __init__(self, name, unit='ops', bytes_each=0):
        self.name = name
        self.unit = unit
        self.bytes_each = bytes_each
        self.lat = []
        self.count = 0
        self.truncated = False
        self.wall_ns = 0
        self._lock = threading.Lock()

    def add(self, batch):
        with self._lock:
            self.count += len(batch)
            if len(self.lat) < self.CAP:
                self.lat.extend(batch)
            else:
                self.truncated = True

    def pct(self, p):
        if not self.lat:
            return float('nan')
        s = sorted(self.lat)
        i = min(len(s) - 1, max(0, int(math.ceil(p / 100 * len(s))) - 1))
        return s[i]

    def rate(self):
        return self.count / (self.wall_ns / NS) if self.wall_ns else 0.0

    def mib_s(self):
        if not self.bytes_each or not self.wall_ns:
            return 0.0
        return self.count * self.bytes_each / (1 << 20) / (self.wall_ns / NS)

    def row(self):
        return {
            'op': self.name,
            'count': self.count,
            'seconds': round(self.wall_ns / NS, 4),
            'rate': round(self.rate(), 1),
            'mib_s': round(self.mib_s(), 2),
            'p50_us': round(self.pct(50) / 1000, 1),
            'p95_us': round(self.pct(95) / 1000, 1),
            'p99_us': round(self.pct(99) / 1000, 1),
            'max_us': round(max(self.lat) / 1000, 1) if self.lat else 0.0,
            'truncated': self.truncated,
        }


def timed(fn, *args):
    t0 = time.perf_counter_ns()
    fn(*args)
    return time.perf_counter_ns() - t0


def run_parallel(threads, worker, *args):
    """Runs `worker(thread_index, *args)` on `threads` threads; returns the wall time."""
    t0 = time.perf_counter_ns()
    if threads == 1:
        worker(0, *args)
    else:
        with ThreadPoolExecutor(max_workers=threads) as pool:
            for fut in [pool.submit(worker, i, *args) for i in range(threads)]:
                fut.result()
    return time.perf_counter_ns() - t0


# ---- metadata ------------------------------------------------------------------------


def bench_metadata(root, threads, files, results):
    """`files` files per thread, each in that thread's own subdirectory."""
    per = max(1, files // threads)
    dirs = [os.path.join(root, f't{i}') for i in range(threads)]
    for d in dirs:
        os.mkdir(d)

    def names(i):
        return [os.path.join(dirs[i], f'f{n:06d}') for n in range(per)]

    def phase(name, body, unit='ops'):
        s = Samples(name, unit)
        s.wall_ns = run_parallel(threads, body, s)
        results.append(s)
        return s

    def do_create(i, s):
        batch = []
        for p in names(i):
            t0 = time.perf_counter_ns()
            fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
            os.close(fd)
            batch.append(time.perf_counter_ns() - t0)
        s.add(batch)

    def do_stat(i, s):
        s.add([timed(os.stat, p) for p in names(i)])

    def do_open_close(i, s):
        batch = []
        for p in names(i):
            t0 = time.perf_counter_ns()
            os.close(os.open(p, os.O_RDONLY))
            batch.append(time.perf_counter_ns() - t0)
        s.add(batch)

    def do_chmod(i, s):
        s.add([timed(os.chmod, p, 0o640) for p in names(i)])

    def do_rename(i, s):
        batch = []
        for p in names(i):
            batch.append(timed(os.rename, p, p + '.r'))
        s.add(batch)
        for p in names(i):  # put the names back for the phases that follow
            os.rename(p + '.r', p)

    def do_miss(i, s):
        def miss(p):
            try:
                os.stat(p)
            except FileNotFoundError:
                pass
        s.add([timed(miss, p + '.absent') for p in names(i)])

    def do_readdir(i, s):
        # One listdir is one operation over `per` entries; repeat so the number means
        # something on a fast filesystem.  The op is named with the entry count because
        # listdir/s is only comparable at the same directory size.
        reps = max(4, min(64, 4000 // max(1, per)))
        s.add([timed(os.listdir, dirs[i]) for _ in range(reps)])

    def do_symlink(i, s):
        batch = []
        for p in names(i):
            t0 = time.perf_counter_ns()
            os.symlink('target', p + '.l')
            os.readlink(p + '.l')
            batch.append(time.perf_counter_ns() - t0)
        s.add(batch)
        for p in names(i):
            os.unlink(p + '.l')

    def do_link(i, s):
        batch = []
        for p in names(i):
            batch.append(timed(os.link, p, p + '.h'))
        s.add(batch)
        for p in names(i):
            os.unlink(p + '.h')

    def do_mkdir_rmdir(i, s):
        batch = []
        for n in range(per):
            p = os.path.join(dirs[i], f'd{n:06d}')
            t0 = time.perf_counter_ns()
            os.mkdir(p)
            os.rmdir(p)
            batch.append(time.perf_counter_ns() - t0)
        s.add(batch)

    def do_unlink(i, s):
        s.add([timed(os.unlink, p) for p in names(i)])

    phase('create', do_create)
    phase('stat', do_stat)
    phase('open+close', do_open_close)
    phase('chmod', do_chmod)
    phase('rename', do_rename)
    phase('lookup-miss', do_miss)
    phase(f'readdir({per})', do_readdir, unit='listdir')
    phase('symlink+readlink', do_symlink)
    phase('link', do_link)
    phase('mkdir+rmdir', do_mkdir_rmdir)
    phase('unlink', do_unlink)

    for d in dirs:
        shutil.rmtree(d, ignore_errors=True)


# ---- data ----------------------------------------------------------------------------


def aligned_buffer(size):
    """A page-aligned buffer, which O_DIRECT requires."""
    return mmap.mmap(-1, size)


def bench_data(root, threads, seconds, block_sizes, direct, results):
    file_size = 256 << 20  # the per-thread working set; capped by `seconds` in practice
    flags_extra = 0
    if direct:
        flags_extra = getattr(os, 'O_DIRECT', 0)
        if not flags_extra:
            print('fsperf: O_DIRECT not available on this platform, ignoring --direct',
                  file=sys.stderr)

    paths = [os.path.join(root, f'data{i}.bin') for i in range(threads)]
    written = [0] * threads

    def phase(name, body, bs, unit='ops'):
        s = Samples(name, unit, bytes_each=bs)
        s.wall_ns = run_parallel(threads, body, s)
        results.append((bs, s))
        return s

    def seq_write(i, s, bs, sync_each=False):
        buf = aligned_buffer(bs)
        buf.write(bytes((j * 31 + i) & 0xFF for j in range(min(bs, 4096))) *
                  max(1, bs // 4096))
        fd = os.open(paths[i], os.O_WRONLY | os.O_CREAT | os.O_TRUNC | flags_extra, 0o644)
        try:
            deadline = time.perf_counter_ns() + int(seconds * NS)
            batch, off = [], 0
            while time.perf_counter_ns() < deadline and off < file_size:
                t0 = time.perf_counter_ns()
                os.pwrite(fd, buf, off)
                if sync_each:
                    os.fsync(fd)
                batch.append(time.perf_counter_ns() - t0)
                off += bs
            if not sync_each:
                os.fsync(fd)
            written[i] = max(written[i], off)
            s.add(batch)
        finally:
            os.close(fd)
            buf.close()

    def seq_read(i, s, bs):
        buf = aligned_buffer(bs)
        fd = os.open(paths[i], os.O_RDONLY | flags_extra)
        try:
            deadline = time.perf_counter_ns() + int(seconds * NS)
            batch, off = [], 0
            while time.perf_counter_ns() < deadline:
                if off + bs > written[i]:
                    off = 0
                    if written[i] < bs:
                        break
                t0 = time.perf_counter_ns()
                os.preadv(fd, [buf], off)
                batch.append(time.perf_counter_ns() - t0)
                off += bs
            s.add(batch)
        finally:
            os.close(fd)
            buf.close()

    def rand_io(i, s, bs, write):
        buf = aligned_buffer(bs)
        fd = os.open(paths[i], (os.O_RDWR if write else os.O_RDONLY) | flags_extra)
        try:
            span = max(bs, (written[i] // bs) * bs)
            rng = random.Random(1234 + i)
            deadline = time.perf_counter_ns() + int(seconds * NS)
            batch = []
            while time.perf_counter_ns() < deadline:
                off = rng.randrange(0, max(1, span // bs)) * bs
                t0 = time.perf_counter_ns()
                if write:
                    os.pwrite(fd, buf, off)
                else:
                    os.preadv(fd, [buf], off)
                batch.append(time.perf_counter_ns() - t0)
            if write:
                os.fsync(fd)
            s.add(batch)
        finally:
            os.close(fd)
            buf.close()

    for bs in block_sizes:
        phase('seq-write', lambda i, s, bs=bs: seq_write(i, s, bs), bs)
        phase('seq-read', lambda i, s, bs=bs: seq_read(i, s, bs), bs)
        # A second read pass: on a cached filesystem this is the page cache, on an
        # O_DIRECT or cache-cold path it repeats the first number.
        phase('seq-reread', lambda i, s, bs=bs: seq_read(i, s, bs), bs)

    small = min(block_sizes)
    phase('rand-read', lambda i, s: rand_io(i, s, small, False), small)
    phase('rand-write', lambda i, s: rand_io(i, s, small, True), small)
    phase('write+fsync', lambda i, s: seq_write(i, s, small, sync_each=True), small)

    for p in paths:
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass


# ---- environment ---------------------------------------------------------------------


def mount_info(path):
    """The mount this path is on, and its options, from /proc/self/mountinfo."""
    try:
        real = os.path.realpath(path)
        best = None
        with open('/proc/self/mountinfo', encoding='utf-8') as fh:
            for line in fh:
                parts = line.split()
                sep = parts.index('-')
                mnt, fstype, opts = parts[4], parts[sep + 1], parts[sep + 3]
                if real == mnt or real.startswith(mnt.rstrip('/') + '/'):
                    if best is None or len(mnt) > len(best[0]):
                        best = (mnt, fstype, opts)
        if best:
            return {'mountpoint': best[0], 'fstype': best[1], 'options': best[2]}
    except OSError:
        pass
    return {'mountpoint': '?', 'fstype': '?', 'options': ''}


# ---- rendering -----------------------------------------------------------------------


def fmt_lat(us):
    """Microseconds below a millisecond, milliseconds above — a fixed unit makes every
    column on a fast filesystem read 0.00."""
    if us < 1000:
        return f'{us:.1f}us'
    return f'{us / 1000:.2f}ms'


def print_metadata(rows):
    print()
    print(f'{"metadata op":<18}{"ops/s":>12}{"p50":>10}{"p95":>10}{"p99":>10}{"max":>10}')
    print('-' * 70)
    for r in rows:
        print(f'{r["op"]:<18}{r["rate"]:>12,.0f}{fmt_lat(r["p50_us"]):>10}'
              f'{fmt_lat(r["p95_us"]):>10}{fmt_lat(r["p99_us"]):>10}'
              f'{fmt_lat(r["max_us"]):>10}')


def human_bs(bs):
    return f'{bs // 1024}K' if bs < (1 << 20) else f'{bs >> 20}M'


def print_data(rows):
    print()
    print(f'{"data op":<14}{"bs":>6}{"MiB/s":>11}{"IOPS":>11}'
          f'{"p50":>10}{"p95":>10}{"p99":>10}')
    print('-' * 72)
    for r in rows:
        print(f'{r["op"]:<14}{human_bs(r["bs"]):>6}{r["mib_s"]:>11,.1f}{r["rate"]:>11,.0f}'
              f'{fmt_lat(r["p50_us"]):>10}{fmt_lat(r["p95_us"]):>10}'
              f'{fmt_lat(r["p99_us"]):>10}')


def compare(base, cur, tolerance):
    """Prints current vs baseline as a percentage, for every op both runs have; returns
    the ops that came out more than `tolerance` percent slower."""
    def key(section, r):
        return (section, r['op'], r.get('bs', 0))

    old = {key(sec, r): r for sec in ('metadata', 'data') for r in base.get(sec, [])}
    print()
    print(f'{"op":<22}{"baseline":>12}{"current":>12}{"change":>10}')
    print('-' * 56)
    worse = []
    for sec in ('metadata', 'data'):
        for r in cur.get(sec, []):
            k = key(sec, r)
            if k not in old:
                continue
            metric = 'mib_s' if sec == 'data' and r['mib_s'] else 'rate'
            a, b = old[k][metric], r[metric]
            if not a:
                continue
            delta = (b - a) / a * 100
            label = r['op'] + (f' {human_bs(r["bs"])}' if sec == 'data' else '')
            print(f'{label:<22}{a:>12,.1f}{b:>12,.1f}{delta:>9.1f}%')
            if delta < -tolerance:
                worse.append((label, delta))
    if worse:
        print()
        print(f'slower than the baseline by more than {tolerance:g}%:')
        for label, delta in worse:
            print(f'  {label}: {delta:.1f}%')
    return worse


# ---- main ----------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(
        description='Filesystem metadata and data performance, through real syscalls.',
        epilog='groups: metadata, data')
    ap.add_argument('dir', metavar='DIR', help='a directory on the filesystem under test')
    ap.add_argument('--threads', type=int, default=4, help='concurrent workers (default 4)')
    ap.add_argument('--files', type=int, default=2000,
                    help='metadata files per thread-set (default 2000)')
    ap.add_argument('--seconds', type=float, default=3.0,
                    help='seconds per data point (default 3)')
    ap.add_argument('--bs', default='4K,64K,1M',
                    help='data block sizes (default 4K,64K,1M)')
    ap.add_argument('--direct', action='store_true',
                    help='open data files O_DIRECT (bypass the client page cache)')
    ap.add_argument('--only', default='', help='metadata or data')
    ap.add_argument('--skip', default='')
    ap.add_argument('--json', metavar='OUT', help='write the full result as JSON')
    ap.add_argument('--compare', metavar='BASE.json',
                    help='compare against a previous --json run')
    ap.add_argument('--tolerance', type=float, default=10.0, metavar='PCT',
                    help='with --compare, exit 1 when an op is more than PCT%% slower '
                         '(default 10); run-to-run noise on a local filesystem is easily '
                         'this large, so raise it or compare only what you care about')
    args = ap.parse_args()

    groups = ('metadata', 'data')
    only = {g for g in args.only.split(',') if g}
    skip = {g for g in args.skip.split(',') if g}
    for g in only | skip:
        if g not in groups:
            print(f'fsperf: unknown group {g!r}; known: {", ".join(groups)}', file=sys.stderr)
            return 2

    def parse_bs(text):
        out = []
        for part in text.split(','):
            part = part.strip().upper()
            mult = {'K': 1024, 'M': 1 << 20}.get(part[-1:], 1)
            out.append(int(part.rstrip('KM')) * mult)
        return sorted(set(out))

    try:
        block_sizes = parse_bs(args.bs)
    except ValueError:
        print(f'fsperf: cannot parse --bs {args.bs!r}', file=sys.stderr)
        return 2
    if args.threads < 1 or args.files < 1 or args.seconds <= 0:
        print('fsperf: --threads/--files must be >= 1 and --seconds > 0', file=sys.stderr)
        return 2

    root = os.path.abspath(args.dir)
    if not os.path.isdir(root):
        print(f'fsperf: {root} is not a directory', file=sys.stderr)
        return 2
    base = os.path.join(root, f'.fsperf-{os.getpid()}')
    try:
        os.mkdir(base)
    except OSError as e:
        print(f'fsperf: cannot create {base}: {e}', file=sys.stderr)
        return 2

    info = mount_info(root)
    print(f'fsperf: {root}')
    print(f'  mount    {info["mountpoint"]} ({info["fstype"]})')
    if info['options']:
        print(f'  options  {info["options"]}')
    print(f'  run      threads={args.threads} files={args.files} '
          f'seconds={args.seconds} bs={args.bs}'
          f'{" O_DIRECT" if args.direct else ""}')

    result = {
        'dir': root,
        'mount': info,
        'threads': args.threads,
        'files': args.files,
        'seconds': args.seconds,
        'block_sizes': block_sizes,
        'direct': bool(args.direct),
        'started': time.strftime('%Y-%m-%dT%H:%M:%S'),
        'metadata': [],
        'data': [],
    }
    rc = 0
    try:
        if 'metadata' not in skip and (not only or 'metadata' in only):
            md = []
            bench_metadata(base, args.threads, args.files, md)
            result['metadata'] = [s.row() for s in md]
            print_metadata(result['metadata'])
        if 'data' not in skip and (not only or 'data' in only):
            dt = []
            try:
                bench_data(base, args.threads, args.seconds, block_sizes, args.direct, dt)
            except OSError as e:
                if args.direct and e.errno == errno.EINVAL:
                    print('fsperf: O_DIRECT rejected here; rerun without --direct',
                          file=sys.stderr)
                    return 2
                raise
            for bs, s in dt:
                row = s.row()
                row['bs'] = bs
                result['data'].append(row)
            print_data(result['data'])
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if any(r['truncated'] for r in result['metadata'] + result['data']):
        print(f'\nnote: more than {Samples.CAP:,} samples in some op — percentiles are '
              f'over the first {Samples.CAP:,}, rates are over all of them')

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as fh:
            json.dump(result, fh, indent=2)
        print(f'\nfsperf: wrote {args.json}')

    if args.compare:
        try:
            with open(args.compare, encoding='utf-8') as fh:
                baseline = json.load(fh)
        except (OSError, ValueError) as e:
            print(f'fsperf: cannot read {args.compare}: {e}', file=sys.stderr)
            return 2
        if baseline.get('threads') != args.threads or baseline.get('direct') != bool(args.direct):
            print('fsperf: the baseline was taken with different settings '
                  f'(threads={baseline.get("threads")}, direct={baseline.get("direct")}) — '
                  'the comparison below is not apples to apples', file=sys.stderr)
        if compare(baseline, result, args.tolerance):
            rc = 1
    return rc


if __name__ == '__main__':
    sys.exit(main())
