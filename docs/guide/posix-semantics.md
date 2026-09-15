# POSIX 语义测试

`scripts/posix_semantics.py` 把一个目录过一遍 POSIX 要求的文件系统行为，报告它**没做到**
的部分：47 项检查，分 12 组，纯 Python 3 标准库，不依赖任何外部套件。
`scripts/posix_semantics_vm.sh` 是它的 lightnfs 跑法——起一个网关、按每个 NFS 版本挂载、
对挂载点各跑一遍，并与后端目录的结果对照。

## 为什么是"对着一个目录跑"

POSIX 语义只在**真实内核挂载**这个视角上存在：`rename()` 的原子替换、删掉仍打开的文件、
`fcntl` 字节锁、`O_APPEND` 忽略偏移——这些都是 syscall 行为，是内核 NFS 客户端把 NFS
操作组合出来的结果。本仓库自带的验收客户端 `lnfs_accept_client` 说的是 NFS 操作本身
（COMPOUND、OPEN、LOCK……），看不到这一层，所以这套检查必须走真实挂载。

于是检查器本身写成**文件系统无关**的：给它任何目录都行。这带来两个用法：

- **先对本地目录跑一遍**，确认"这些期望本身是对的"——它们在 ext4 / tmpfs / XFS 上必须全绿。
  这也是改动检查器之后的自检方式。
- **再对 NFS 挂载点跑**，此时的 FAIL 就是 lightnfs（或内核客户端）与 POSIX 的真实差异。

`posix_semantics_vm.sh` 把两者串起来：先跑后端目录作为基线，再跑挂载点，这样"底下的文件
系统本来就不满足"和"NFS 这一层丢了语义"能分开。

## 快速开始

无 root，验证检查器本身：

```sh
scripts/posix_semantics.py /tmp
```

```
posix_semantics: /tmp (tmpfs), uid=1000, 47 checks
posix_semantics: 47 passed, 0 failed, 0 skipped
```

有 root 的测试 VM，对 lightnfs 跑：

```sh
sudo scripts/posix_semantics_vm.sh            # vers=3、4.1、4.2 各一轮
sudo scripts/posix_semantics_vm.sh 4.2        # 只跑一个版本
```

对一个已经挂好的挂载点跑（自己管挂载时）：

```sh
scripts/posix_semantics.py /mnt/lightnfs -v
```

一轮约 4.5 秒（其中 3.3 秒是 `times` 组为了跨过 1 秒时间戳粒度而故意睡的）。

## 用法

```
scripts/posix_semantics.py DIR [--only GROUPS] [--skip GROUPS] [-v]
```

| 选项 | 作用 |
|------|------|
| `DIR` | 被测文件系统上的任意目录；检查在它下面建一个 `.posix-semantics-<pid>/` 临时目录，跑完删掉 |
| `--only a,b` | 只跑这些组 |
| `--skip a,b` | 跳过这些组 |
| `-v` | 每项都打印（默认只打印 FAIL / SKIP 与汇总） |

退出码：`0` 全部通过，`1` 有 FAIL，`2` 用法或准备阶段出错（目录不存在、组名拼错、建不了
临时目录）。每项检查在自己的干净子目录里跑，互不影响；跑完的清理能容忍 silly-rename 留下的
`.nfsXXXX` 文件。

`posix_semantics_vm.sh` 的环境变量：`LNFS_NFS_PORT`（默认 12099）、`LNFS_MOUNT_PORT`
（12098）、`LNFS_POSIX_ARGS`（透传给检查器的额外参数，例如 `-v` 或 `--skip times`）。

## 十二个组，检查什么

| 组 | 项数 | 覆盖 |
|----|------|------|
| `basic` | 7 | 创建 / stat / 读回 / unlink；`O_EXCL` 独占；`O_TRUNC` 清空；`O_APPEND` **忽略文件偏移**；`pread`/`pwrite` 不移动偏移；越过 EOF 写出的空洞读回全零；`truncate` 增缩（增长时补零） |
| `errno` | 2 | 每种误用的 errno：`ENOENT`、非目录中间路径与文件名末尾加 `/` 的 `ENOTDIR`、写打开目录与 unlink 目录的 `EISDIR`、`ENOTEMPTY`、`EEXIST`、256 字节分量的 `ENAMETOOLONG`、符号链接成环的 `ELOOP` |
| `link` | 4 | 硬链接共享 inode 与 `st_nlink`；透过一个名字写、另一个名字可见；unlink 递减链接数、最后一个才释放名字；对目录 / 缺失源 / 已存在目标的拒绝 |
| `symlink` | 4 | `readlink` 原样返回目标；悬空链接 `stat` 报 ENOENT 而 `lstat` 正常；`stat` 与 `open` 跟随链接；空目标被拒 |
| `rename` | 6 | 改名后旧名消失；**原子替换**已存在文件（目标名指向源 inode）；被替换的文件在仍有 fd 打开时继续可读；目录改名、改到空目录上；目录↔文件不匹配、非空目录、**把目录改到自己子树里**（`EINVAL`）；改名到自身是 no-op 而非删除 |
| `unlink-open` | 2 | 删掉仍打开的文件后，透过 fd 仍可读可写、`st_nlink` 为 0；名字从 readdir 消失（**允许出现 `.nfs*` 占位**——那是内核客户端的 silly-rename） |
| `dir` | 4 | mkdir/rmdir 与目录项；`.` 与 `..` 的解析；`st_nlink` 随子目录增减；500 项的大目录完整列出且无重复（readdir cookie 的稳定性） |
| `perm` | 6 | chmod 持久且反映在 `st_mode`；0444 不能写打开、0222 不能读打开；目录缺 `+x` 不能穿越、缺 `+r` 不能列举；`access()` 与 `open()` 一致；sticky 位存活 |
| `times` | 4 | 写移动 mtime/ctime；**目录**的 mtime 随条目增删移动；chmod 只动 ctime 不动 mtime；`utimensat` 精确落值 |
| `sparse` | 2 | `SEEK_HOLE`/`SEEK_DATA` 找得到空洞；空洞读回全零 |
| `lock` | 2 | `fcntl` 字节锁跨**进程**冲突（重叠区间被拒、不重叠区间可获）；关掉该文件上的所有 fd 即释放本进程的锁 |
| `data` | 4 | 4 MiB 非重复数据逐字节读回；奇数偏移（1 / 511 / 4095 / 4096 / 65535 / 99999）的覆写精确落位；文件与父目录的 `fsync`；`statvfs` 的 `f_bsize` / `f_blocks` / `f_namemax ≥ 255` |

## SKIP 是设计的一部分

检查器区分"不满足"（FAIL）和"这里问不了"（SKIP）。会 SKIP 的情况：

- **以 root 运行**：uid 0 绕过权限位，`perm` 组里靠 EACCES 判定的四项无意义，自动跳过。
  想真正测权限，用普通用户跑。
- **v3 挂载没有字节锁**：lightnfs **不实现 NLM/NSM**（决策 D8，见
  [deployment.md](deployment.md) §7 与 [design 07](../design/07-state-management.md)），
  所以 v3 上没有字节锁。`posix_semantics_vm.sh` 在 v3 轮用 `-o nolock` 挂载并
  `--skip lock`——不这么做内核会一直阻塞在锁请求上。直接对一个 v3 挂载点跑检查器时，
  `lock` 组会自己识别出 `ENOLCK` / `EOPNOTSUPP` / `EINVAL` 并 SKIP。
- **文件系统不支持 `SEEK_HOLE`/`SEEK_DATA`**：`sparse` 组里那一项 SKIP（"空洞读回全零"
  那项不依赖它，照跑）。

## 已知的、合法的 NFS 差异

这些不是 bug，检查器已经把它们编进期望里：

- **删掉仍打开的文件会留下 `.nfsXXXX`**。POSIX 要求"名字立刻消失、fd 继续可用"，NFS 服务器
  没有"无名文件"的概念，于是内核客户端把文件 rename 成 `.nfsXXXX`，等最后一个 fd 关闭再
  删。所以 `unlink-open` 那项在比对 readdir 时忽略 `.nfs*` 前缀的条目。
- **时间戳粒度**。`times` 组每次比较前睡 1.1 秒，因为服务端文件系统可能只有 1 秒粒度；
  `utimensat` 那项也按秒比较，不比较纳秒。
- **atime 不测**。`relatime` / `noatime` 让 atime 不可靠，任何基于它的断言都会假阳性。
- **close-to-open 一致性**。检查器在单个客户端内做所有事，不测跨客户端可见性——那是 NFS
  缓存语义（`CTO`）的范畴，属于 cthon / fsx 与本仓库自己的验收脚本。
- **`fsync` 父目录**可能返回 `EINVAL`，检查器接受。

## 它与其他测试的关系

| | 看什么 | 怎么跑 |
|--|--------|--------|
| **posix_semantics** | POSIX **syscall 语义**：errno、原子性、链接计数、锁、时间戳 | 任何目录；对 lightnfs 用 `posix_semantics_vm.sh` |
| cthon04（`accept_m2_vm.sh` / `accept_m6_vm.sh`） | NFS 社区的传统一致性套件，覆盖面更广但失败信息更粗 | root VM |
| fsx（`accept_m2_vm.sh`） | 随机读写 / 截断 / mmap 的**数据一致性**长跑 | root VM |
| pynfs（`accept_m6_local.sh`） | NFSv4.1 **协议**一致性（op 级） | 本机，无 root |
| `lnfs_accept_client`（`accept_m*_local.sh`） | 本仓库自己的 NFS 操作级验收，数据逐字节比对 | 本机，无 root |

posix_semantics 与它们不重叠的地方是**失败信息**：它一项一句地说清"哪条 POSIX 要求、期望
什么、实际什么"，适合拿来定位；cthon/fsx 更适合拿来广撒网。

## 没有把它注册成 ctest 用例

有意为之。它跑的是**运行 CMake 的那台机器上的构建目录所在文件系统**，与 lightnfs 无关；
在某些文件系统上（缺稀疏支持、时间戳粒度异常）会出现与代码无关的 FAIL 或大量 SKIP，
把 `make test` 变成不可信的门禁。需要自检时手动跑一次即可：

```sh
scripts/posix_semantics.py /tmp && echo "checker itself is sane"
```

改动检查器之后至少对 tmpfs 与一个磁盘文件系统（ext4/XFS）各跑一遍——两者在稀疏、时间戳
粒度上的行为不同，都必须全绿。

## 加一项检查

```python
@check('rename', 'rename onto itself is a no-op, not a deletion')
def rename_self(d):
    p = write_file(os.path.join(d, 'f'), b'keep me')
    os.rename(p, p)
    want_eq(read_file(p), b'keep me', 'contents after renaming a file onto itself')
```

`d` 是这一项专用的空目录（跑完自动删）。断言用 `want(cond, what)` / `want_eq(got, want,
what)` / `want_errno(EXXX, fn, *args, what=...)`；`raise Skip('原因')` 表示"这里问不了"。
`what` 会原样进 FAIL 行，所以写成"哪条要求"而不是"assert failed"。

**新检查必须先在本地 ext4 与 tmpfs 上全绿**——检查器的信誉全靠这一点：它绿的时候没人看，
它红的时候必须是文件系统真的错了。
