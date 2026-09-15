#!/usr/bin/env python3
"""POSIX filesystem semantics checker: runs a directory through the syscall-level
behaviour POSIX requires, and reports what it does not do.

    scripts/posix_semantics.py DIR [--only GROUPS] [--skip GROUPS] [-v]

DIR is any directory on the filesystem under test — a local one (to convince yourself
the expectations here are right), or an NFS mount served by lightnfsd.  Everything is
done with real syscalls through a real kernel mount, which is the only vantage point
these semantics exist at: the repository's own acceptance client speaks NFS operations,
not open()/rename()/fcntl(), so it cannot see them.

Each check runs in its own fresh subdirectory of DIR and cleans up after itself.  A
check that cannot apply here says so (SKIP) instead of failing: running as root voids
the permission checks, a v3 mount has no byte-range locks (lightnfs implements no
NLM/NSM), and a filesystem without sparse support cannot be asked about holes.

Exit 0 when nothing failed, 1 when something did, 2 on a usage or setup error.
"""
import argparse
import ctypes
import errno
import os
import stat
import sys
import time

# ---- harness -------------------------------------------------------------------------

CHECKS = []


class Fail(Exception):
    """A POSIX requirement the filesystem did not meet."""


class Skip(Exception):
    """The check cannot apply in this environment."""


def check(group, name):
    def register(fn):
        CHECKS.append((group, name, fn))
        return fn

    return register


def want(cond, what):
    if not cond:
        raise Fail(what)


def want_eq(got, expected, what):
    if got != expected:
        raise Fail(f'{what}: got {got!r}, want {expected!r}')


def want_errno(expected, fn, *args, what=''):
    """`fn(*args)` must raise OSError with errno `expected`."""
    names = expected if isinstance(expected, tuple) else (expected,)
    try:
        fn(*args)
    except OSError as e:
        if e.errno in names:
            return
        raise Fail(f'{what}: raised {errno.errorcode.get(e.errno, e.errno)}, want '
                   f'{"/".join(errno.errorcode.get(n, str(n)) for n in names)}') from None
    raise Fail(f'{what}: succeeded, want {"/".join(errno.errorcode.get(n, str(n)) for n in names)}')


def write_file(path, data=b'x', mode=0o644):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    try:
        os.write(fd, data)
    finally:
        os.close(fd)
    return path


def read_file(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        out = b''
        while True:
            chunk = os.read(fd, 65536)
            if not chunk:
                return out
            out += chunk
    finally:
        os.close(fd)


def rmtree(path):
    """Like shutil.rmtree, but tolerates the .nfsXXXX files a silly-rename leaves and
    the EBUSY they answer while still open."""
    try:
        entries = os.listdir(path)
    except FileNotFoundError:
        return
    for name in entries:
        full = os.path.join(path, name)
        try:
            if stat.S_ISDIR(os.lstat(full).st_mode):
                rmtree(full)
            else:
                os.unlink(full)
        except OSError:
            pass
    try:
        os.rmdir(path)
    except OSError:
        pass


# ---- basics --------------------------------------------------------------------------


@check('basic', 'create, stat, read back, unlink')
def basic_roundtrip(d):
    p = os.path.join(d, 'f')
    write_file(p, b'hello posix')
    st = os.stat(p)
    want(stat.S_ISREG(st.st_mode), 'st_mode is not a regular file')
    want_eq(st.st_size, 11, 'st_size after write')
    want_eq(read_file(p), b'hello posix', 'contents read back')
    os.unlink(p)
    want_errno(errno.ENOENT, os.stat, p, what='stat after unlink')


@check('basic', 'O_CREAT|O_EXCL is exclusive')
def basic_excl(d):
    p = os.path.join(d, 'f')
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    os.close(fd)
    want_errno(errno.EEXIST, os.open, p, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
               what='second O_EXCL create')


@check('basic', 'O_TRUNC empties an existing file')
def basic_trunc(d):
    p = write_file(os.path.join(d, 'f'), b'0123456789')
    fd = os.open(p, os.O_WRONLY | os.O_TRUNC)
    os.close(fd)
    want_eq(os.stat(p).st_size, 0, 'size after O_TRUNC')


@check('basic', 'O_APPEND writes land at end of file')
def basic_append(d):
    p = write_file(os.path.join(d, 'f'), b'aaa')
    fd = os.open(p, os.O_WRONLY | os.O_APPEND)
    try:
        os.lseek(fd, 0, os.SEEK_SET)  # POSIX: O_APPEND ignores the offset
        os.write(fd, b'bbb')
    finally:
        os.close(fd)
    want_eq(read_file(p), b'aaabbb', 'contents after O_APPEND write at offset 0')


@check('basic', 'pread/pwrite do not move the file offset')
def basic_pwrite(d):
    p = write_file(os.path.join(d, 'f'), b'0123456789')
    fd = os.open(p, os.O_RDWR)
    try:
        os.lseek(fd, 3, os.SEEK_SET)
        os.pwrite(fd, b'XY', 6)
        want_eq(os.lseek(fd, 0, os.SEEK_CUR), 3, 'offset after pwrite')
        want_eq(os.pread(fd, 2, 6), b'XY', 'pread of what pwrite wrote')
        want_eq(os.lseek(fd, 0, os.SEEK_CUR), 3, 'offset after pread')
    finally:
        os.close(fd)
    want_eq(read_file(p), b'012345XY89', 'contents after pwrite')


@check('basic', 'write past EOF leaves a zero-filled gap')
def basic_gap(d):
    p = os.path.join(d, 'f')
    fd = os.open(p, os.O_WRONLY | os.O_CREAT, 0o644)
    try:
        os.lseek(fd, 4096, os.SEEK_SET)
        os.write(fd, b'end')
    finally:
        os.close(fd)
    want_eq(os.stat(p).st_size, 4099, 'size after a write past EOF')
    want_eq(read_file(p), b'\0' * 4096 + b'end', 'the gap reads back as zeroes')


@check('basic', 'ftruncate grows and shrinks')
def basic_truncate(d):
    p = write_file(os.path.join(d, 'f'), b'0123456789')
    os.truncate(p, 4)
    want_eq(read_file(p), b'0123', 'contents after shrinking truncate')
    os.truncate(p, 8)
    want_eq(os.stat(p).st_size, 8, 'size after growing truncate')
    want_eq(read_file(p), b'0123' + b'\0' * 4, 'a growing truncate zero-fills')


# ---- errno ---------------------------------------------------------------------------


@check('errno', 'the documented errno for each misuse')
def errno_table(d):
    f = write_file(os.path.join(d, 'file'))
    sub = os.path.join(d, 'dir')
    os.mkdir(sub)
    write_file(os.path.join(sub, 'inner'))

    want_errno(errno.ENOENT, os.open, os.path.join(d, 'absent'), os.O_RDONLY,
               what='open of a missing file')
    want_errno(errno.ENOTDIR, os.open, os.path.join(f, 'x'), os.O_RDONLY,
               what='path with a non-directory component')
    want_errno(errno.ENOTDIR, os.stat, f + '/', what='trailing slash on a regular file')
    want_errno(errno.EISDIR, os.open, sub, os.O_WRONLY, what='open a directory for writing')
    want_errno(errno.EISDIR, os.unlink, sub, what='unlink a directory')
    want_errno(errno.ENOTDIR, os.rmdir, f, what='rmdir a regular file')
    want_errno(errno.ENOTEMPTY, os.rmdir, sub, what='rmdir a non-empty directory')
    want_errno(errno.EEXIST, os.mkdir, sub, what='mkdir over an existing directory')
    want_errno(errno.ENAMETOOLONG, os.stat, os.path.join(d, 'n' * 256),
               what='a 256-byte component')


@check('errno', 'a symlink loop is ELOOP')
def errno_loop(d):
    a, b = os.path.join(d, 'a'), os.path.join(d, 'b')
    os.symlink('b', a)
    os.symlink('a', b)
    want_errno(errno.ELOOP, os.stat, a, what='stat through a symlink loop')


# ---- hard links ----------------------------------------------------------------------


@check('link', 'a hard link shares the inode and the link count')
def link_basic(d):
    a = write_file(os.path.join(d, 'a'), b'shared')
    b = os.path.join(d, 'b')
    os.link(a, b)
    sa, sb = os.stat(a), os.stat(b)
    want_eq(sb.st_ino, sa.st_ino, 'st_ino of a hard link')
    want_eq(sa.st_nlink, 2, 'st_nlink after link()')
    want_eq(sb.st_nlink, 2, 'st_nlink seen through the new name')
    want_eq(read_file(b), b'shared', 'contents through the new name')


@check('link', 'writing through one name is visible through the other')
def link_shared_data(d):
    a = write_file(os.path.join(d, 'a'), b'one')
    b = os.path.join(d, 'b')
    os.link(a, b)
    write_file(a, b'two')
    want_eq(read_file(b), b'two', 'data written through the other name')


@check('link', 'unlink drops the link count, the last one frees the name')
def link_unlink(d):
    a = write_file(os.path.join(d, 'a'))
    b = os.path.join(d, 'b')
    os.link(a, b)
    os.unlink(a)
    want_eq(os.stat(b).st_nlink, 1, 'st_nlink after unlinking one name')
    want_errno(errno.ENOENT, os.stat, a, what='stat of the unlinked name')
    os.unlink(b)


@check('link', 'link() refuses a directory and a missing source')
def link_refusals(d):
    sub = os.path.join(d, 'dir')
    os.mkdir(sub)
    want_errno((errno.EPERM, errno.EACCES), os.link, sub, os.path.join(d, 'l'),
               what='hard link to a directory')
    want_errno(errno.ENOENT, os.link, os.path.join(d, 'absent'), os.path.join(d, 'l2'),
               what='hard link to a missing file')
    f = write_file(os.path.join(d, 'f'))
    want_errno(errno.EEXIST, os.link, f, sub, what='link onto an existing name')


# ---- symlinks ------------------------------------------------------------------------


@check('symlink', 'readlink returns the target verbatim, lstat sees the link')
def symlink_basic(d):
    target = 'some/relative/target'
    p = os.path.join(d, 'l')
    os.symlink(target, p)
    want_eq(os.readlink(p), target, 'readlink')
    want(stat.S_ISLNK(os.lstat(p).st_mode), 'lstat does not report a symlink')


@check('symlink', 'a dangling symlink resolves to ENOENT but lstat works')
def symlink_dangling(d):
    p = os.path.join(d, 'l')
    os.symlink('absent', p)
    want_errno(errno.ENOENT, os.stat, p, what='stat through a dangling symlink')
    os.lstat(p)
    os.unlink(p)


@check('symlink', 'a symlink is followed for stat and open')
def symlink_follow(d):
    f = write_file(os.path.join(d, 'f'), b'target data')
    p = os.path.join(d, 'l')
    os.symlink('f', p)
    want_eq(os.stat(p).st_ino, os.stat(f).st_ino, 'stat through a symlink')
    want_eq(read_file(p), b'target data', 'open through a symlink')


@check('symlink', 'an empty or over-long target is rejected')
def symlink_limits(d):
    want_errno(errno.ENOENT, os.symlink, '', os.path.join(d, 'empty'),
               what='symlink with an empty target')


# ---- rename --------------------------------------------------------------------------


@check('rename', 'rename moves a file and leaves no old name')
def rename_basic(d):
    a = write_file(os.path.join(d, 'a'), b'data')
    b = os.path.join(d, 'b')
    os.rename(a, b)
    want_errno(errno.ENOENT, os.stat, a, what='stat of the old name')
    want_eq(read_file(b), b'data', 'contents at the new name')


@check('rename', 'rename atomically replaces an existing file')
def rename_replace(d):
    a = write_file(os.path.join(d, 'a'), b'new')
    b = write_file(os.path.join(d, 'b'), b'old')
    old_ino = os.stat(a).st_ino
    os.rename(a, b)
    want_eq(read_file(b), b'new', 'contents after replacing rename')
    want_eq(os.stat(b).st_ino, old_ino, 'the destination name now points at the source inode')


@check('rename', 'the replaced file survives while an fd is open on it')
def rename_replaced_still_open(d):
    a = write_file(os.path.join(d, 'a'), b'new')
    b = write_file(os.path.join(d, 'b'), b'old')
    fd = os.open(b, os.O_RDONLY)
    try:
        os.rename(a, b)
        want_eq(os.pread(fd, 3, 0), b'old', 'reads through the fd of the replaced file')
    finally:
        os.close(fd)


@check('rename', 'rename of a directory, and onto an empty one')
def rename_dirs(d):
    src, dst = os.path.join(d, 'src'), os.path.join(d, 'dst')
    os.mkdir(src)
    write_file(os.path.join(src, 'inner'))
    os.rename(src, dst)
    want(os.path.isfile(os.path.join(dst, 'inner')), 'the directory content moved with it')
    empty = os.path.join(d, 'empty')
    os.mkdir(empty)
    os.rename(dst, empty)  # POSIX: replacing an empty directory is allowed
    want(os.path.isfile(os.path.join(empty, 'inner')), 'rename onto an empty directory')


@check('rename', 'rename refuses the mismatched and impossible cases')
def rename_refusals(d):
    f = write_file(os.path.join(d, 'f'))
    dirp = os.path.join(d, 'dir')
    os.mkdir(dirp)
    write_file(os.path.join(dirp, 'inner'))
    full = os.path.join(d, 'full')
    os.mkdir(full)
    write_file(os.path.join(full, 'inner'))

    want_errno(errno.ENOTDIR, os.rename, dirp, f, what='rename a directory onto a file')
    want_errno(errno.EISDIR, os.rename, f, dirp, what='rename a file onto a directory')
    want_errno(errno.ENOTEMPTY, os.rename, dirp, full,
               what='rename onto a non-empty directory')
    want_errno(errno.EINVAL, os.rename, dirp, os.path.join(dirp, 'below'),
               what='rename a directory into its own subtree')
    want_errno(errno.ENOENT, os.rename, os.path.join(d, 'absent'), os.path.join(d, 'x'),
               what='rename a missing source')


@check('rename', 'rename onto itself is a no-op, not a deletion')
def rename_self(d):
    p = write_file(os.path.join(d, 'f'), b'keep me')
    os.rename(p, p)  # POSIX: same existing file, succeed and change nothing
    want_eq(read_file(p), b'keep me', 'contents after renaming a file onto itself')


# ---- an unlinked file that is still open ---------------------------------------------


@check('unlink-open', 'an unlinked file stays readable and writable through its fd')
def unlink_open(d):
    p = os.path.join(d, 'f')
    fd = os.open(p, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        os.write(fd, b'still here')
        os.unlink(p)
        want_errno(errno.ENOENT, os.stat, p, what='stat of the unlinked name')
        want_eq(os.pread(fd, 10, 0), b'still here', 'read through the fd after unlink')
        os.pwrite(fd, b'STILL', 0)
        want_eq(os.pread(fd, 10, 0), b'STILL here', 'write through the fd after unlink')
        want_eq(os.fstat(fd).st_nlink, 0, 'st_nlink of an unlinked open file')
    finally:
        os.close(fd)


@check('unlink-open', 'the name is gone from readdir (a .nfs* placeholder is allowed)')
def unlink_open_readdir(d):
    p = os.path.join(d, 'gone')
    fd = os.open(p, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        os.unlink(p)
        names = [n for n in os.listdir(d) if not n.startswith('.nfs')]
        want_eq(names, [], 'readdir after unlinking the only (still open) file')
    finally:
        os.close(fd)


# ---- directories ---------------------------------------------------------------------


@check('dir', 'mkdir / rmdir and the directory entries')
def dir_basic(d):
    sub = os.path.join(d, 'sub')
    os.mkdir(sub, 0o755)
    want(stat.S_ISDIR(os.stat(sub).st_mode), 'st_mode of a new directory')
    for name in ('a', 'b', 'c'):
        write_file(os.path.join(sub, name))
    want_eq(sorted(os.listdir(sub)), ['a', 'b', 'c'], 'listdir of a directory')
    for name in ('a', 'b', 'c'):
        os.unlink(os.path.join(sub, name))
    os.rmdir(sub)
    want_errno(errno.ENOENT, os.stat, sub, what='stat after rmdir')


@check('dir', '. and .. resolve, .. of the root of the tree is its parent')
def dir_dots(d):
    sub = os.path.join(d, 'sub')
    os.mkdir(sub)
    want_eq(os.stat(os.path.join(sub, '.')).st_ino, os.stat(sub).st_ino, '"." resolution')
    want_eq(os.stat(os.path.join(sub, '..')).st_ino, os.stat(d).st_ino, '".." resolution')


@check('dir', 'st_nlink counts subdirectories')
def dir_nlink(d):
    sub = os.path.join(d, 'sub')
    os.mkdir(sub)
    base = os.stat(sub).st_nlink
    want(base >= 2, f'a fresh directory has st_nlink {base}, want at least 2 (itself and .)')
    os.mkdir(os.path.join(sub, 'child'))
    want_eq(os.stat(sub).st_nlink, base + 1, 'st_nlink after creating a subdirectory')
    os.rmdir(os.path.join(sub, 'child'))
    want_eq(os.stat(sub).st_nlink, base, 'st_nlink after removing the subdirectory')


@check('dir', 'a large directory lists completely and without duplicates')
def dir_large(d):
    sub = os.path.join(d, 'many')
    os.mkdir(sub)
    expected = {f'entry-{i:05d}' for i in range(500)}
    for name in expected:
        write_file(os.path.join(sub, name), b'')
    got = os.listdir(sub)
    want_eq(len(got), len(set(got)), 'listdir returned duplicate names')
    want_eq(set(got), expected, 'listdir of a 500-entry directory')


# ---- permissions ---------------------------------------------------------------------


def _need_non_root():
    if os.geteuid() == 0:
        raise Skip('running as root: permission bits are not enforced against uid 0')


@check('perm', 'chmod is durable and visible in st_mode')
def perm_chmod(d):
    p = write_file(os.path.join(d, 'f'), mode=0o644)
    for mode in (0o600, 0o755, 0o444, 0o640):
        os.chmod(p, mode)
        want_eq(stat.S_IMODE(os.stat(p).st_mode), mode, f'st_mode after chmod {mode:o}')


@check('perm', 'a read-only file cannot be opened for writing')
def perm_readonly(d):
    _need_non_root()
    p = write_file(os.path.join(d, 'f'), b'data', mode=0o444)
    want_errno(errno.EACCES, os.open, p, os.O_WRONLY, what='open a 0444 file for writing')
    read_file(p)


@check('perm', 'a write-only file cannot be opened for reading')
def perm_writeonly(d):
    _need_non_root()
    p = write_file(os.path.join(d, 'f'), b'data')
    os.chmod(p, 0o222)
    want_errno(errno.EACCES, os.open, p, os.O_RDONLY, what='open a 0222 file for reading')


@check('perm', 'a directory without +x cannot be traversed, without +r cannot be listed')
def perm_dir_bits(d):
    _need_non_root()
    sub = os.path.join(d, 'sub')
    os.mkdir(sub, 0o755)
    inner = write_file(os.path.join(sub, 'inner'))
    os.chmod(sub, 0o644)  # r-- : listable, not traversable
    try:
        want_errno(errno.EACCES, os.stat, inner, what='stat below a directory without +x')
        os.chmod(sub, 0o311)  # --x : traversable, not listable
        os.stat(inner)
        want_errno(errno.EACCES, os.listdir, sub, what='listdir of a directory without +r')
    finally:
        os.chmod(sub, 0o755)


@check('perm', 'access() agrees with what open() does')
def perm_access(d):
    _need_non_root()
    p = write_file(os.path.join(d, 'f'), mode=0o400)
    want(os.access(p, os.R_OK), 'access(R_OK) on a 0400 file')
    want(not os.access(p, os.W_OK), 'access(W_OK) said yes on a 0400 file')
    os.chmod(p, 0o600)
    want(os.access(p, os.W_OK), 'access(W_OK) on a 0600 file')


@check('perm', 'the sticky bit restricts deletion to the owner')
def perm_sticky(d):
    sub = os.path.join(d, 'sticky')
    os.mkdir(sub, 0o1777)
    got = stat.S_IMODE(os.stat(sub).st_mode)
    want(got & stat.S_ISVTX, f'the sticky bit did not survive mkdir 1777 (mode {got:o})')


# ---- timestamps ----------------------------------------------------------------------


def _mtime_ns(path):
    return os.stat(path).st_mtime_ns


@check('times', 'writing a file moves its mtime and ctime forward')
def times_write(d):
    p = write_file(os.path.join(d, 'f'), b'a')
    before = os.stat(p)
    time.sleep(1.1)  # coarse for a filesystem with 1 s timestamp granularity
    write_file(p, b'b')
    after = os.stat(p)
    want(after.st_mtime_ns > before.st_mtime_ns,
         f'mtime did not advance on write ({before.st_mtime_ns} -> {after.st_mtime_ns})')
    want(after.st_ctime_ns >= before.st_ctime_ns, 'ctime went backwards on write')


@check('times', 'creating and removing an entry moves the directory mtime')
def times_dir(d):
    sub = os.path.join(d, 'sub')
    os.mkdir(sub)
    before = _mtime_ns(sub)
    time.sleep(1.1)
    p = write_file(os.path.join(sub, 'f'))
    created = _mtime_ns(sub)
    want(created > before, 'directory mtime did not advance when an entry was created')
    time.sleep(1.1)
    os.unlink(p)
    want(_mtime_ns(sub) > created,
         'directory mtime did not advance when an entry was removed')


@check('times', 'chmod moves ctime but not mtime')
def times_chmod(d):
    p = write_file(os.path.join(d, 'f'))
    before = os.stat(p)
    time.sleep(1.1)
    os.chmod(p, 0o600)
    after = os.stat(p)
    want_eq(after.st_mtime_ns, before.st_mtime_ns, 'chmod changed mtime')
    want(after.st_ctime_ns > before.st_ctime_ns, 'chmod did not move ctime')


@check('times', 'utimensat sets the exact times it is given')
def times_utime(d):
    p = write_file(os.path.join(d, 'f'))
    atime_ns, mtime_ns = 1_000_000_000_000_000_000, 1_100_000_000_000_000_000
    os.utime(p, ns=(atime_ns, mtime_ns))
    st = os.stat(p)
    # A filesystem with 1 s granularity truncates the sub-second part; compare seconds.
    want_eq(st.st_mtime_ns // 10**9, mtime_ns // 10**9, 'mtime after utimensat')
    want_eq(st.st_atime_ns // 10**9, atime_ns // 10**9, 'atime after utimensat')


# ---- sparse files --------------------------------------------------------------------


@check('sparse', 'SEEK_HOLE and SEEK_DATA find the hole made by a gap')
def sparse_seek(d):
    p = os.path.join(d, 'f')
    fd = os.open(p, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        os.write(fd, b'head')
        os.lseek(fd, 1 << 20, os.SEEK_SET)
        os.write(fd, b'tail')
        os.fsync(fd)
        try:
            hole = os.lseek(fd, 0, os.SEEK_HOLE)
            data = os.lseek(fd, 4096, os.SEEK_DATA)
        except OSError as e:
            if e.errno in (errno.EINVAL, errno.ENOTSUP, errno.EOPNOTSUPP):
                raise Skip('SEEK_HOLE/SEEK_DATA not supported here') from None
            raise
        want(0 < hole <= 1 << 20, f'SEEK_HOLE from 0 answered {hole}, want inside the file')
        want(data >= hole, f'SEEK_DATA answered {data}, want at or after the hole at {hole}')
        want_eq(os.lseek(fd, 0, os.SEEK_END), (1 << 20) + 4, 'SEEK_END after a sparse write')
    finally:
        os.close(fd)


@check('sparse', 'a hole reads back as zeroes')
def sparse_zeroes(d):
    p = os.path.join(d, 'f')
    fd = os.open(p, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        os.lseek(fd, 65536, os.SEEK_SET)
        os.write(fd, b'z')
        want_eq(os.pread(fd, 4096, 4096), b'\0' * 4096, 'a hole did not read back as zeroes')
    finally:
        os.close(fd)


# ---- byte-range locks ----------------------------------------------------------------


@check('lock', 'fcntl byte-range locks conflict between processes')
def lock_conflict(d):
    import fcntl
    import struct

    p = write_file(os.path.join(d, 'f'), b'\0' * 4096)
    fd = os.open(p, os.O_RDWR)
    try:
        lock = struct.pack('hhqqi', fcntl.F_WRLCK, os.SEEK_SET, 0, 1024, 0)
        try:
            fcntl.fcntl(fd, fcntl.F_SETLK, lock)
        except OSError as e:
            if e.errno in (errno.ENOLCK, errno.EOPNOTSUPP, errno.ENOTSUP, errno.EINVAL):
                raise Skip(f'byte-range locks unavailable here '
                           f'({errno.errorcode.get(e.errno, e.errno)}) — an NFSv3 mount has '
                           f'none, lightnfs implements no NLM/NSM') from None
            raise

        # A second process must see the conflict.
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(r)
            code = b'?'
            try:
                cfd = os.open(p, os.O_RDWR)
                try:
                    fcntl.fcntl(cfd, fcntl.F_SETLK,
                                struct.pack('hhqqi', fcntl.F_WRLCK, os.SEEK_SET, 0, 512, 0))
                    code = b'T'  # took a conflicting lock: wrong
                except OSError as e:
                    code = b'C' if e.errno in (errno.EACCES, errno.EAGAIN) else b'?'
                finally:
                    os.close(cfd)
            except Exception:  # noqa: BLE001 - the child must never raise out
                code = b'?'
            os.write(w, code)
            os._exit(0)
        os.close(w)
        got = os.read(r, 1)
        os.close(r)
        os.waitpid(pid, 0)
        want_eq(got, b'C', 'a second process taking an overlapping write lock')

        # A non-overlapping range must be grantable.
        r2, w2 = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(r2)
            code = b'?'
            try:
                cfd = os.open(p, os.O_RDWR)
                try:
                    fcntl.fcntl(cfd, fcntl.F_SETLK,
                                struct.pack('hhqqi', fcntl.F_WRLCK, os.SEEK_SET, 2048, 512, 0))
                    code = b'T'
                except OSError:
                    code = b'C'
                finally:
                    os.close(cfd)
            except Exception:  # noqa: BLE001
                code = b'?'
            os.write(w2, code)
            os._exit(0)
        os.close(w2)
        got = os.read(r2, 1)
        os.close(r2)
        os.waitpid(pid, 0)
        want_eq(got, b'T', 'a second process taking a non-overlapping lock')
    finally:
        os.close(fd)


@check('lock', 'closing any fd on the file drops that process’s locks')
def lock_close_releases(d):
    import fcntl
    import struct

    p = write_file(os.path.join(d, 'f'), b'\0' * 4096)
    lock = struct.pack('hhqqi', fcntl.F_WRLCK, os.SEEK_SET, 0, 1024, 0)
    fd = os.open(p, os.O_RDWR)
    try:
        fcntl.fcntl(fd, fcntl.F_SETLK, lock)
    except OSError as e:
        os.close(fd)
        if e.errno in (errno.ENOLCK, errno.EOPNOTSUPP, errno.ENOTSUP, errno.EINVAL):
            raise Skip('byte-range locks unavailable here') from None
        raise
    os.close(fd)

    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(r)
        code = b'?'
        try:
            cfd = os.open(p, os.O_RDWR)
            try:
                fcntl.fcntl(cfd, fcntl.F_SETLK, lock)
                code = b'T'
            except OSError:
                code = b'C'
            finally:
                os.close(cfd)
        except Exception:  # noqa: BLE001
            code = b'?'
        os.write(w, code)
        os._exit(0)
    os.close(w)
    got = os.read(r, 1)
    os.close(r)
    os.waitpid(pid, 0)
    want_eq(got, b'T', 'the lock was still held after every fd on the file was closed')


# ---- data integrity ------------------------------------------------------------------


@check('data', 'a multi-megabyte write reads back byte for byte')
def data_roundtrip(d):
    p = os.path.join(d, 'f')
    blob = bytes((i * 7 + 13) & 0xFF for i in range(1 << 20)) * 4  # 4 MiB, non-repeating
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        off = 0
        while off < len(blob):
            off += os.write(fd, blob[off:off + 128 * 1024])
        os.fsync(fd)
    finally:
        os.close(fd)
    want_eq(os.stat(p).st_size, len(blob), 'size of a 4 MiB file')
    want(read_file(p) == blob, 'a 4 MiB file did not read back byte for byte')


@check('data', 'overwrites at odd offsets land exactly')
def data_odd_offsets(d):
    p = write_file(os.path.join(d, 'f'), b'\0' * 100000)
    fd = os.open(p, os.O_RDWR)
    try:
        for off in (1, 511, 4095, 4096, 65535, 99999):
            os.pwrite(fd, bytes([off & 0xFF]), off)
        for off in (1, 511, 4095, 4096, 65535, 99999):
            want_eq(os.pread(fd, 1, off), bytes([off & 0xFF]), f'byte written at offset {off}')
    finally:
        os.close(fd)


@check('data', 'fsync on a file and on its parent directory succeed')
def data_fsync(d):
    p = write_file(os.path.join(d, 'f'), b'durable')
    fd = os.open(p, os.O_WRONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
    dfd = os.open(d, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(dfd)
    except OSError as e:
        if e.errno not in (errno.EINVAL, errno.EBADF):
            raise Fail(f'fsync of the parent directory failed: {e}') from None
    finally:
        os.close(dfd)


@check('data', 'statvfs reports a usable filesystem')
def data_statvfs(d):
    st = os.statvfs(d)
    want(st.f_bsize > 0, f'f_bsize is {st.f_bsize}')
    want(st.f_blocks > 0, f'f_blocks is {st.f_blocks}')
    want(st.f_namemax >= 255, f'f_namemax is {st.f_namemax}, want at least 255')


# ---- runner --------------------------------------------------------------------------


def uname_context(path):
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        buf = ctypes.create_string_buffer(4096)
        # struct statfs is architecture-dependent; f_type is the first field on Linux.
        if libc.statfs(os.fsencode(path), buf) == 0:
            f_type = int.from_bytes(buf.raw[:8], sys.byteorder)
            return {0x6969: 'nfs', 0x01021994: 'tmpfs', 0xEF53: 'ext2/3/4',
                    0x58465342: 'xfs', 0x9123683E: 'btrfs'}.get(f_type, f'0x{f_type:x}')
    except Exception:  # noqa: BLE001 - purely informational
        pass
    return 'unknown'


def main():
    groups = []
    for g, _, _ in CHECKS:
        if g not in groups:
            groups.append(g)
    ap = argparse.ArgumentParser(
        description='POSIX filesystem semantics checker.',
        epilog='groups: ' + ', '.join(groups))
    ap.add_argument('dir', metavar='DIR', help='a directory on the filesystem under test')
    ap.add_argument('--only', default='', help='comma-separated groups to run')
    ap.add_argument('--skip', default='', help='comma-separated groups to skip')
    ap.add_argument('-v', '--verbose', action='store_true', help='print every check')
    args = ap.parse_args()

    only = {g for g in args.only.split(',') if g}
    skip = {g for g in args.skip.split(',') if g}
    for g in only | skip:
        if g not in groups:
            print(f'posix_semantics: unknown group {g!r}; known: {", ".join(groups)}',
                  file=sys.stderr)
            return 2

    root = os.path.abspath(args.dir)
    if not os.path.isdir(root):
        print(f'posix_semantics: {root} is not a directory', file=sys.stderr)
        return 2
    base = os.path.join(root, f'.posix-semantics-{os.getpid()}')
    try:
        os.mkdir(base)
    except OSError as e:
        print(f'posix_semantics: cannot create {base}: {e}', file=sys.stderr)
        return 2

    print(f'posix_semantics: {root} ({uname_context(root)}), '
          f'uid={os.geteuid()}, {len(CHECKS)} checks')
    passed = failed = skipped = 0
    failures = []
    try:
        for i, (group, name, fn) in enumerate(CHECKS):
            if (only and group not in only) or group in skip:
                continue
            work = os.path.join(base, f'{i:03d}-{group}')
            os.mkdir(work)
            try:
                fn(work)
            except Skip as e:
                skipped += 1
                print(f'SKIP {group}: {name} — {e}')
            except Fail as e:
                failed += 1
                failures.append((group, name, str(e)))
                print(f'FAIL {group}: {name} — {e}')
            except OSError as e:
                failed += 1
                what = f'unexpected {errno.errorcode.get(e.errno, e.errno)}: {e}'
                failures.append((group, name, what))
                print(f'FAIL {group}: {name} — {what}')
            else:
                passed += 1
                if args.verbose:
                    print(f'ok   {group}: {name}')
            finally:
                rmtree(work)
    finally:
        rmtree(base)

    print(f'posix_semantics: {passed} passed, {failed} failed, {skipped} skipped')
    if failures:
        print('failed checks:')
        for group, name, why in failures:
            print(f'  {group}: {name} — {why}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
