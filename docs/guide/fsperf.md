# 文件系统性能测试（元数据与数据）

`scripts/fsperf.py` 用真实 syscall 量一个目录的**元数据**与**数据**性能：11 项元数据操作
的 ops/s 与延迟分布，6 类数据操作在多个块大小下的 MiB/s 与 IOPS。纯 Python 3 标准库，
不需要 fio、mdtest、iozone。`scripts/fsperf_vm.sh` 是它的 lightnfs 跑法——起网关、先量
后端目录、再按每个 NFS 版本挂载量一遍，并把挂载点逐项对着后端目录报百分比。

## 它填的是哪个空

仓库里已有两层性能工具，中间是空的：

| | 量什么 | 经过挂载 |
|--|--------|----------|
| `lightnfs-ctl bench`（[benchmarks.md](../testing/benchmarks.md)） | **协议栈自身的上限**：传输 / RPC / v3 引擎 + memory 后端，三层 | 否 |
| **`fsperf.py`** | **应用看到的东西**：open/read/write/stat/rename/unlink 的真实代价，含客户端页缓存 | 是（或任意本地目录） |
| fio / mdtest | 同上，更全面、更可调 | 是，但要额外装 |

`lightnfs-ctl bench` 回答"协议栈能跑多快"，跟真实部署的差距里有内核客户端、网络、后端文件
系统；`fsperf.py` 回答"挂上去之后应用能拿到多少"。两者都需要，量的不是一回事。

和 fio 的关系是**够用与全面**：fsperf 不做 libaio/io_uring 引擎、不做多进程、不做混合读写
比例；它的价值是零依赖、一条命令、输出直接可比。要压榨极限或做复杂 IO 模型，用 fio。

## 快速开始

```sh
scripts/fsperf.py /tmp                          # 默认一轮约 30 秒
scripts/fsperf.py /mnt/lightnfs --threads 8     # 对着一个已挂好的挂载点
sudo scripts/fsperf_vm.sh                       # 起网关，vers=3/4.1/4.2 各量一遍
```

默认参数（`--threads 4 --files 2000 --seconds 3 --bs 4K,64K,1M`）在本地文件系统上约 30 秒；
对 NFS 挂载点会更久，元数据阶段尤其——那 11 个阶段每个要跑 2000 次网络往返。先用
`--files 200 --seconds 1` 摸一遍量级，再跑完整的。

## 用法

```
scripts/fsperf.py DIR [--threads N] [--files N] [--seconds S] [--bs LIST] [--direct]
                      [--only GROUP] [--skip GROUP] [--json OUT]
                      [--compare BASE.json] [--tolerance PCT]
```

| 选项 | 默认 | 说明 |
|------|------|------|
| `DIR` | — | 被测文件系统上的任意目录；在它下面建 `.fsperf-<pid>/`，跑完删掉 |
| `--threads N` | 4 | 并发线程数。每个线程有自己的子目录与数据文件 |
| `--files N` | 2000 | 元数据阶段的文件总数，均分到各线程 |
| `--seconds S` | 3 | 每个数据点跑多久 |
| `--bs LIST` | `4K,64K,1M` | 数据阶段的块大小 |
| `--direct` | 关 | 数据文件用 `O_DIRECT` 打开，绕过客户端页缓存 |
| `--only` / `--skip` | — | `metadata` 或 `data` |
| `--json OUT` | — | 把完整结果写成 JSON（用来做基线） |
| `--compare BASE.json` | — | 与之前的 JSON 逐项比，打印百分比变化 |
| `--tolerance PCT` | 10 | 配合 `--compare`：有操作慢过这个比例就以 1 退出 |

退出码：`0` 正常（或 `--compare` 未超容差），`1` `--compare` 发现超出容差的回退，
`2` 用法或准备阶段出错。

`fsperf_vm.sh` 的环境变量：`LNFS_NFS_PORT`（12099）、`LNFS_MOUNT_PORT`（12098）、
`LNFS_FSPERF_ARGS`（透传给工具的参数，默认 `--threads 4 --files 2000 --seconds 3`）、
`LNFS_FSPERF_OUT`（结果目录）。

## 量了什么

**元数据**（每项 ops/s + p50/p95/p99/max 延迟）：

`create`（`O_CREAT|O_EXCL` + close）、`stat`、`open+close`（已存在的文件）、`chmod`、
`rename`、`lookup-miss`（stat 一个不存在的名字 → ENOENT，量的是**否定查找**的代价，
NFS 上它和命中一样要走一次 LOOKUP）、`readdir(N)`（一次 `listdir` 为一次操作，N 是目录里的
条目数——**listdir/s 只在相同目录规模下可比**，所以条目数写进了操作名）、
`symlink+readlink`、`link`、`mkdir+rmdir`、`unlink`。

**数据**（每项 MiB/s + IOPS + 延迟分布，在每个 `--bs` 下各一组）：

`seq-write`、`seq-read`、`seq-reread`（紧接着再读一遍：本地文件系统上这是页缓存，
`--direct` 或缓存冷的路径上它会重复第一个数字——**两者的差就是缓存命中的收益**）、
`rand-read`、`rand-write`（都用最小的块大小）、`write+fsync`（每次写后 `fsync`，量的是
**同步写**的代价，在 NFS 上就是 WRITE(FILE_SYNC) 或 WRITE+COMMIT 的往返）。

`seq-write` 结束时会 `fsync` 一次，所以它的 MiB/s 含落盘；`write+fsync` 才是"每次都等"。

## 怎么读这些数字

- **一次运行的绝对值没有意义**，只有对比有：同一台机器、同一个文件系统、同一组参数下的
  两次运行才可比。基线用 `--json` 存下来，之后 `--compare`。
- **延迟列自适应单位**：小于 1 毫秒显示微秒（`12.3us`），否则毫秒（`1.45ms`）。
- **`p99` 和 `max` 比平均值有用**。NFS 上的长尾通常是重传、COMMIT、或服务端 offload 队列
  排队；`max` 突然到几十毫秒往往对应一次 `timeo` 重传。
- **线程数在本地文件系统上会骗你**。tmpfs 上一次 `stat` 是亚微秒级，此时 Python 的 GIL 是
  瓶颈，`--threads 4` 反而比 `--threads 1` 慢。网络文件系统上 syscall 阻塞在往返上、GIL 是
  释放的，线程才真正提升并发——所以**对本地目录做基线时用 `--threads 1`，对挂载点用多线程**，
  或者干脆两边用同一个值只看相对变化。
- **`--direct` 是建议不是保证**：有的文件系统接受 `O_DIRECT` 但照样走缓存，有的直接拒绝
  （工具会说 "O_DIRECT rejected here"）。它在 ext4/XFS + NFS 上是有效的。
- **客户端缓存是结果的一部分**，这是故意的：它就是应用看到的东西。想看服务端真实负载，
  用 `--direct`，或者在两轮之间重新挂载。

## 对 lightnfs 跑：`fsperf_vm.sh`

```sh
sudo scripts/fsperf_vm.sh              # vers=3、4.1、4.2
sudo scripts/fsperf_vm.sh 4.2          # 只跑一个
LNFS_FSPERF_ARGS="--threads 8 --files 500 --seconds 5" sudo -E scripts/fsperf_vm.sh 4.1
```

它做三件事：

1. 起一个本地后端的网关，先对**后端目录**量一遍，存成 `backing.json`——这是下限，挂载点
   再快也快不过它。
2. 每个版本挂载一次，量一遍，并用 `--compare backing.json` 打印逐项百分比。这一步的
   `--tolerance` 设成 1000，因为 NFS 比本地慢是**预期**的，我们要的是那个百分比本身，
   而不是让它失败。
3. 打印每轮生效的挂载选项（`findmnt -no OPTIONS`）——`rsize`/`wsize`/`actimeo` 对结果的影响
   比大多数服务端参数都大，不记下来的数字没法复现。

结果目录里每轮一份 `.json` 与 `.txt`（`backing`、`vers3`、`vers4_1`、`vers4_2`），改完代码
之后可以直接 `--compare` 回去。

跑之前值得知道的：v3 轮用 `-o nolock` 挂载（lightnfs 不实现 NLM/NSM，见
[deployment.md](deployment.md) §7）；三轮之间不重新挂载后端，所以后端文件系统的缓存状态
对各轮是一致的。

## 典型的解读

几个在 NFS 上反复出现、值得先有心理准备的形态：

- **元数据比数据更能区分服务端**。`create` / `unlink` / `rename` 每次都是一到两个往返，
  几乎全是延迟；`seq-read` 有预读和大块，容易被带宽掩盖。调服务端时先看元数据列。
- **`lookup-miss` 明显慢于 `stat`** 通常意味着否定结果没有被客户端缓存（`actimeo` 太小），
  或服务端对不存在的名字走了更长的路径。
- **`write+fsync` 与 `seq-write` 差两个数量级**是正常的：前者每次都等 COMMIT 落盘。
  它更接近数据库类负载，`seq-write` 更接近拷贝大文件。
- **`seq-reread` 与 `seq-read` 一样快**说明没有命中客户端缓存——要么用了 `--direct`，
  要么工作集超过了客户端内存，要么 `actimeo=0`。
- **随机 4K 写快于随机 4K 读**在带缓存的路径上很常见（写进页缓存就返回，读必须等）；
  加 `--direct` 之后这个倒置应该消失。

## 没有把它做成回归门禁

`scripts/bench_gate.sh` 是协议栈基准的门禁，它有 `tools/bench/baseline.txt` 这个可复现的
基线，因为它不经过挂载、不经过磁盘。fsperf 的数字依赖内核版本、挂载选项、后端文件系统、
机器负载——把它钉成 CI 门禁只会得到一个经常误报的告警。它的用法是**手动对比**：改动前存
一份 `--json`，改动后 `--compare`。

真要在固定的机器上做长期跟踪，用 `--compare` 加一个宽松的 `--tolerance`（比如 25），并且
只盯元数据那几项——它们的方差比数据项小得多。

## 它与其他性能工具的关系

| | 场景 |
|--|------|
| `lightnfs-ctl bench` + `bench_gate.sh` | 每次改动的协议栈回归门禁（[benchmarks.md](../testing/benchmarks.md)） |
| **`fsperf.py` / `fsperf_vm.sh`** | 挂载之后的端到端性能；调挂载选项、后端参数、对比版本 |
| fio（真实挂载） | 复杂 IO 模型、极限压测；见 benchmarks.md §6 |
| `scripts/posix_semantics.py` | 同样是"对着目录跑"，但看的是**正确性**不是速度（[posix-semantics.md](posix-semantics.md)） |

fsperf 与 posix_semantics 是一对：同一个入口形状（任意目录 / VM 跑法），一个回答"对不对"，
一个回答"多快"。
