# scripts/ 速查

`scripts/` 里是构建之外的一切自动化：源码门禁、验收脚本、外部测试套件的获取、性能与健壮性
跑批、两个运维脚本。每个脚本的文件头注释是权威说明（写了完整的前置条件与环境变量），本文
是"我该用哪个"的索引。

仓库不带托管 CI，`scripts/ci.sh` 就是 CI——本地或任何调度器上跑同一个入口。

## 一眼看全

| 脚本 | 作用 | 需要 root | 谁调它 |
|------|------|-----------|--------|
| [`ci.sh`](#cish) | 全套构建矩阵 + 所有门禁 | 否 | 手动 / 调度器 |
| [`format_check.sh`](#源码门禁) | clang-format + 注释位置门禁 | 否 | `make format-check`、`ci.sh` |
| [`trailing_comments.py`](#源码门禁) | 行尾 `//` 注释上移 | 否 | `make format`、`format_check.sh` |
| [`tidy.sh`](#源码门禁) | clang-tidy（窄检查集） | 否 | `make tidy` |
| [`gen_errmap_cases.py`](#源码门禁) | 由调研文档生成 v3 错误映射用例 | 否 | ctest `errmap_check`、`ci.sh` |
| [`check_gfapi_abi.sh`](#后端-abi-漂移检查) | libgfapi 函数签名漂移 | 否 | `ci.sh` |
| [`check_llapi_abi.sh`](#后端-abi-漂移检查) | Lustre uapi 常量 / 结构漂移 | 否 | `ci.sh` |
| [`check_cephapi_abi.sh`](#后端-abi-漂移检查) | libcephfs 签名与结构布局漂移 | 否 | `ci.sh` |
| [`accept_m2_local.sh`](#本机验收无-root) | v3 读写 / 崩溃恢复 / 连接风暴 / ctl | 否 | 手动 |
| [`accept_m6_local.sh`](#本机验收无-root) | v4.1 / v4.2 全面 + reclaim + pynfs 子集 | 否 | 手动 |
| [`accept_failover_local.sh`](#本机验收无-root) | 双实例主备接管 + 脑裂 | 否 | 手动 |
| [`accept_active_active_local.sh`](#本机验收无-root) | 三实例多活 + 共享清单 | 否 | 手动 |
| [`test_ctl_offline.sh`](#本机验收无-root) | 离线 `lightnfs-ctl catalog` 端到端 | 否 | ctest `ctl_offline_catalog` |
| [`accept_m2_vm.sh`](#root-vm-验收真实内核挂载) | 真实 `vers=3` 挂载 + cthon + fsx | **是** | 手动 |
| [`accept_m6_vm.sh`](#root-vm-验收真实内核挂载) | 真实 `vers=4.2` 挂载：稀疏 / copy / clone | **是** | 手动 |
| [`accept_failover_vm.sh`](#root-vm-验收真实内核挂载) | 真实挂载下的网关故障切换 | **是** | 手动 |
| [`posix_semantics.py`](#posix-语义) | 对任意目录跑 POSIX 文件系统语义检查 | 否 | 手动 |
| [`posix_semantics_vm.sh`](#posix-语义) | 上面那个的 lightnfs 跑法：起网关、按版本挂载、逐轮跑 | **是** | 手动 |
| [`accept_gluster.sh`](#集群后端验收需要真实存储) | GlusterFS 后端对真实卷 | 否 | 手动 |
| [`accept_lustre.sh`](#集群后端验收需要真实存储) | Lustre 后端对真实挂载 | 否 | 手动 |
| [`accept_cephfs.sh`](#集群后端验收需要真实存储) | CephFS 后端对真实集群 | 否 | 手动 |
| [`fetch_cthon.sh`](#外部测试套件的获取) | 取并编译 cthon04 | 否 | VM 验收脚本 |
| [`fetch_fsx.sh`](#外部测试套件的获取) | 单独编译 xfstests 的 fsx | 否 | VM 验收脚本 |
| [`fetch_pynfs.sh`](#外部测试套件的获取) | 取并准备 pynfs 4.1 套件 | 否 | `accept_m6_local.sh` |
| [`fetch_liburing.sh`](#构建依赖) | 编译随仓库的 liburing 子模块 | 否 | 手动（系统无 liburing-dev 时） |
| [`bench_gate.sh`](#性能与健壮性) | 三层基准对基线的下限门禁 | 否 | `ci.sh nightly`、可选 ctest |
| [`fuzz.sh`](#性能与健壮性) | libFuzzer 跑批与语料管理 | 否 | `ci.sh` |
| [`fault_inject.sh`](#性能与健壮性) | 故障注入跑批（崩溃循环 / fsync EIO / 客户端消失） | 否 | 手动（周跑） |
| [`coverage.sh`](#性能与健壮性) | 单测覆盖率报告 | 否 | 手动 |
| [`cluster_roll.sh`](#运维) | 多活网关的滚动撤空 / 迁回 | 否 | 运维 |
| [`gen_seccomp_allowlist.sh`](#运维) | 重新生成 systemd seccomp 白名单 | 否 | 改动运行时后手动 |

`scripts/pynfs_m5_expected.txt` 不是脚本，是 pynfs 的**已知失败清单**（委托、会话、
CURRENT_STATEID 等本实现明确不做或行为有别的用例），`accept_m6_local.sh` 用它做漂移比对。

## ci.sh

```sh
scripts/ci.sh            # 全矩阵：每次改动的门禁
scripts/ci.sh quick      # 只跑默认构建 + ctest
scripts/ci.sh nightly    # 全矩阵 + 基准下限门禁 + 1 小时 fuzz
```

覆盖 README 承诺的构建矩阵——GCC/Clang × Debug/Release × ASAN+UBSAN × TSAN × epoll 兜底 ×
fuzz regress——再加错误映射的文档漂移门禁、格式门禁、`bash -n` 语法检查、三个后端 ABI 检查。
`LNFS_JOBS` 控制并行度（默认核数一半）。

**它不跑验收脚本**：那些要么耗时很长，要么需要 root 或真实存储，按需手动跑。

## 源码门禁

这四个是"改代码就该过"的，`make` 里有对应目标：

- **`format_check.sh`**——按 `.clang-format` 检查全部受版本控制的 C++ 文件（`third_party/`
  与生成的 `.inc` 除外），外加本仓库的注释位置规则：`//` 注释写在被注释代码的**上一行**，
  不写在行尾。`format_check.sh --fix` 等价于 `make format`。
- **`trailing_comments.py`**——上面那条规则的实现。`--check` 列出违规并以 1 退出，`--fix`
  把它们上移（之后要跑 clang-format）。约定保留的形态：`}  // namespace x` 这类只有右括号
  的行、`#if/#endif  // …`、`// NOLINT…`、`// clang-format on|off`、以及行尾带反斜杠续行的。
- **`tidy.sh [build-dir]`**——用已有构建目录的 `compile_commands.json` 对 `src/` 跑
  clang-tidy，检查集在 `.clang-tidy` 里刻意收窄（bugprone / performance / concurrency，
  不做风格改写）。默认 `build`。
- **`gen_errmap_cases.py`**——从 [`docs/reference/nfsv3/08-errors.md`](../reference/nfsv3/08-errors.md)
  §8.2 的逐过程错误表生成 `tests/errmap_v3_cases.inc`。单测的白名单是**从调研文档派生**的，
  所以文档与代码一旦不一致，ctest 的 `errmap_check` 就失败。改了那张表就重跑一次本脚本。

### 后端 ABI 漂移检查

三个集群后端都在运行期 `dlopen` 库（或直接对内核 uapi 下 ioctl），编译期看不见签名，
所以由这三个脚本在有头文件的机器上补上这道检查：

```sh
scripts/check_gfapi_abi.sh   [include-dir-with-glusterfs/api]
scripts/check_llapi_abi.sh   [include-dir-with-linux/lustre]
scripts/check_cephapi_abi.sh [include-dir-with-cephfs/]
```

把 `Api` 结构里每个函数指针成员与真实声明逐个 `static_assert` 比对；`check_cephapi_abi.sh`
另外校验后端自己定义的 `ceph_statx` / `vinodeno_t` 的实际布局，`check_llapi_abi.sh` 校验
ioctl 号、结构大小与常量。**头文件不存在时退出 0（跳过）**——这是没装对应开发包的机器上的
默认行为；`LNFS_{GFAPI,LLAPI,CEPHAPI}_STRICT=1` 让"跳过"变成退出 2。漂移退出 1。

## 本机验收（无 root）

都在回环 TCP 上打真实的 `lightnfsd`，客户端是自研的用户态 NFSv3/v4.1 客户端
`lnfs_accept_client`（`tests/accept_client.cpp`），所有数据逐字节与后端目录比对。不需要
root、不需要内核挂载、不需要集群。

- **`accept_m2_local.sh [ASAN_STRESS_SECONDS]`**——v3 面：创建 / 三种稳定级别的写 / COMMIT /
  SETATTR / 独占创建重放 / 命名空间操作 / DRC 重传，崩溃恢复（`kill -9` 后 boot epoch 变化、
  客户端重发收敛），万级并发连接与超在途流水线的背压，`lightnfs-ctl` 与 metrics HTTP 口，
  最后同样的读写负载在 ASAN 下再跑一遍并检查无泄漏退出。
- **`accept_m6_local.sh`**——v4 面：v4.2 的 DEALLOCATE/SEEK/ALLOCATE/COPY/CLONE，v4.1 的
  读写与 share reservation、字节锁、OPEN_DOWNGRADE，重启后的 `CLAIM_PREVIOUS` reclaim 与
  grace 门禁，租约过期与 courtesy 客户端，ctl 的状态表与强制回收，以及 pynfs 的若干 4.1
  组（对照 `pynfs_m5_expected.txt` 比对漂移），最后 ASAN 再跑一遍协议阶段。
- **`accept_failover_local.sh`**——两个 `lightnfsd` 共用一个后端树与一个 `shared_dir`：
  A 活动、B 待命，`kill -9` A 后由 `cluster takeover` 让 B 接管，断言 BADSESSION、
  server_owner 不变、clientid epoch +1、`CLAIM_PREVIOUS` 与 `LOCK(reclaim)` 成功、写验证器
  变化后数据重发并逐字节校验、提前退出 grace；另有脑裂段（手工改写围栏文件，旧网关自我
  drain）。Release 与 ASAN 各一轮。设计见
  [design 09](../design/09-multi-gateway-failover.md) §9.11。
- **`accept_active_active_local.sh`**——三个网关（gw1/gw2/gw3）、每台一个端口即其
  `node_address`：属主分布、referral 与迁移（`LEASE_MOVED` + `fs_locations`）、
  `cluster_roll.sh` 的撤空与迁回、猝死后按 `nodes` 顺位分散接管、共享导出清单段（在线增删改
  导出）、单网关退化门。每种构建 × 每种导出来源（`LNFS_EXPORTS="local catalog"`）各一轮。
  设计见 [design 10](../design/10-multi-gateway-active-active.md) §10.13。
  ```sh
  scripts/accept_active_active_local.sh
  LNFS_BUILD_DIRS="build:dbg" scripts/accept_active_active_local.sh   # 用已有构建，不重建
  LNFS_EXPORTS=catalog        scripts/accept_active_active_local.sh   # 只跑清单模式
  ```
- **`test_ctl_offline.sh [/path/to/lightnfs-ctl]`**——离线
  `lightnfs-ctl catalog <sub> --shared-dir <dir>` 的端到端：标志传递、退出码、
  三实例脚本用来发布 v1 的那条引导路径、`--from-local` 对配置与清单文档的区分、history /
  diff / rollback / status 与 `--json`。已注册为 ctest 的 `ctl_offline_catalog`，跟着
  `make test` 跑。

## root VM 验收（真实内核挂载）

需要 root 与 `mount.nfs`，因此**不在 `ci.sh` 里**，在测试 VM 上手动跑。

- **`accept_m2_vm.sh [FSX_OPS]`**——真实 `mount -o vers=3` 读写、cthon04 的
  basic/general/special 三套、fsx，以及 `kill -9` 后免重挂载的恢复检查。默认 5 万次 fsx
  操作（分钟级）；过夜跑把 `FSX_OPS` 调到千万级。
- **`accept_m6_vm.sh`**——真实 `mount -o vers=4.2`，用普通工具走 Linux 客户端的 v4.2 路径：
  `fallocate -p` / `truncate` + `fallocate -l` / `SEEK_HOLE` 的空洞图与后端树比对，
  `cp` 触发的 COPY 与部分 `copy_file_range`，`cp --reflink=always`（XFS reflink / Btrfs 上
  成功，否则客户端按 NOTSUPP 回退），再跑一遍 4.2 下的 cthon。
- **`accept_failover_vm.sh [local|gluster|cephfs|lustre]`**——真实 v4.1 挂载持有 open +
  POSIX 锁 + 未提交写，杀掉网关 A、B 取围栏，同一个挂载重连后 reclaim 回全部状态。
  `local` 轮只需一台 root VM；其余三轮要求导出树与 `shared_dir` 都在对应的集群文件系统上。
  没有 keepalived 时脚本把挂载指向 `$LNFS_VIP`（默认回环）。
  ```sh
  sudo LNFS_VIP=10.0.0.9 scripts/accept_failover_vm.sh local
  ```

## POSIX 语义

- **`posix_semantics.py DIR [--only G] [--skip G] [-v]`**——把一个目录过一遍 POSIX 要求的
  文件系统行为（47 项 / 12 组：errno 表、硬链接与符号链接、rename 的原子替换与拒绝条件、
  删掉仍打开的文件、目录与大目录列举、权限位、时间戳、稀疏、`fcntl` 字节锁、数据完整性）。
  纯标准库，不依赖外部套件。**给它任何目录都行**：先对本地 ext4/tmpfs 跑一遍确认这些期望
  本身是对的，再对 NFS 挂载点跑——那时的 FAIL 才是真差异。一轮约 4.5 秒。
- **`posix_semantics_vm.sh [vers...]`**（需要 root）——起一个网关、按每个版本挂载
  （默认 3、4.1、4.2）、对挂载点各跑一遍，并先跑一遍后端目录作为基线，好把"底下的文件系统
  本来就不满足"和"NFS 这一层丢了语义"分开。v3 轮自动 `-o nolock` 并跳过 `lock` 组
  （lightnfs 不实现 NLM/NSM）。

完整说明见 [posix-semantics.md](posix-semantics.md)。

## 集群后端验收（需要真实存储）

网关侧**不需要 root**：库在运行期加载，导出走回环 TCP，由 `lnfs_accept_client` 驱动
v3 + v4.1 + v4.2 负载。逐字节校验需要同一份存储在本机可见（下面的 `*_MOUNT`）；没有时
依赖挂载的模式跳过，纯协议模式照跑。第一个位置参数是压力阶段的秒数。

```sh
LNFS_GLUSTER_VOLUME=vol0 LNFS_GLUSTER_SERVERS=gs1,gs2:24007 \
  [LNFS_GLUSTER_SUBDIR=/exports/lightnfs] [LNFS_GLUSTER_MOUNT=/mnt/vol0/exports/lightnfs] \
  scripts/accept_gluster.sh [STRESS_SECONDS]

LNFS_LUSTRE_EXPORT=/mnt/lustre/lightnfs-accept [LNFS_LUSTRE_MOUNT=/mnt/lustre] \
  [LNFS_LUSTRE_HSM=1] scripts/accept_lustre.sh [STRESS_SECONDS]

LNFS_CEPH_CONF=/etc/ceph/ceph.conf LNFS_CEPH_ID=lightnfs \
  [LNFS_CEPH_KEYRING=…] [LNFS_CEPH_FS=cephfs] [LNFS_CEPH_MON_HOST=mon1,mon2] \
  [LNFS_CEPH_SUBDIR=/exports/lightnfs] [LNFS_CEPH_MOUNT=/mnt/cephfs/exports/lightnfs] \
  scripts/accept_cephfs.sh [STRESS_SECONDS]
```

要点：三者都以 `squash = "none"` 跑，客户端身份直接透传给存储，所以子目录要对网关身份可写
（CephFS 还要求客户端密钥的 MDS caps 覆盖该 subdir 且不限制 uid/gid）。Lustre 建议挂载带
`-o flock`，原生锁才由 MDS 仲裁；`LNFS_LUSTRE_HSM=1` 且有 HSM 协调器与 copytool 时，脚本
额外 `lfs hsm_release` 一个文件并检查网关先答 JUKEBOX/DELAY、restore 后再正常服务。

## 外部测试套件的获取

三个 `fetch_*` 都**钉死上游 commit**，保证可复现；`LNFS_FETCH_HEAD=1` 改取当前 HEAD
（用于抬版本钉）。

- **`fetch_cthon.sh DEST_DIR`**——取并编译 Connectathon 2004 套件。只打一个补丁：设了
  `CTHON_RO=1` 时 `basic/test5b` 跳过收尾的 unlink，好让只读挂载也能跑读测试；其余保持上游。
- **`fetch_fsx.sh DEST_DIR`**——只把 xfstests 的 `fsx` 单独编出来（浅克隆，用一个最小的
  `global.h` 垫片替换它生成的那份），产物是 `DEST_DIR/fsx`。
- **`fetch_pynfs.sh DEST_DIR`**——取并准备 pynfs 4.1 套件，顺带处理 Python ≥ 3.13 移除
  `xdrlib` 的问题（用 `xdrlib3` backport + 垫片）。

## 构建依赖

**`fetch_liburing.sh`**——编译随仓库的 liburing 子模块（`third_party/liburing`，钉死
tag），供系统没有 `liburing-dev` 的机器使用。必须跑过它 CMake 才会选用子模块：configure
会生成 `compat.h`，没编译过的头文件不可用。

## 性能与健壮性

- **`bench_gate.sh [BUILD_DIR]`**——跑 `lightnfs-ctl bench` 的 echo / nullrpc /
  fullpath(GETATTR, READ4k) 三层，把实测 rps 与 `tools/bench/baseline.txt` × 下限系数比较，
  任一不达标即退出 1。`LNFS_BENCH_FLOOR`（默认 0.5）、`LNFS_BENCH_CALLS`（默认 20000）。
  `ci.sh nightly` 会跑；也可用 `-DLNFS_ENABLE_BENCH_GATE=ON` 注册成 ctest 用例（默认关，
  因为它要 Release 构建和一台安静的机器）。基准工具本身的用法见
  [../testing/benchmarks.md](../testing/benchmarks.md)。
- **`fuzz.sh <smoke|nightly|run <target> [secs]|minimize|regress>`**——构建 clang 的
  libFuzzer 目标并按时间预算跑：`smoke` 120 秒摊到所有目标，`nightly` 1 小时。种子语料在
  `fuzz/seed/<target>/`（入库），新发现进 `fuzz/corpus/<target>/`（不入库）。`regress` 把
  种子与语料用非 clang 构建回放一遍——这条也是 ctest 里那批 `fuzz_regress_*` 用例做的事。
- **`fault_inject.sh [CRASH_ITERATIONS]`**——无 root 的故障注入跑批：N 轮"负载中 `kill -9`
  → 重启 → 客户端重发收敛、验证器变化"；`LNFS_FAULT_FSYNC_EIO=1` 下注入的 fsync 失败要浮到
  NFS3ERR_IO 且是粘性的（不影响别的文件）；v4.1 持有者不发 CLOSE 就消失（courtesy 冲突与
  超时回收，经 ctl 计数）；以及持有 open 状态时 `kill -9` 后的 grace 内 reclaim。默认 5 轮，
  周跑用 20。
- **`coverage.sh [--html]`**——clang source-based coverage 跑单测，构建 `build-cov` 并打印
  行/函数覆盖摘要；`--html` 另外写出 `build-cov/coverage-html/`。是按需报告，不是门禁。

## 运维

这两个是部署环境里会用到的，不是开发门禁：

- **`cluster_roll.sh`**——多活集群的滚动维护，全程只经
  `lightnfs-ctl cluster status --json` 与 `cluster migrate`：
  ```sh
  scripts/cluster_roll.sh --sockets gw1=/run/lightnfs/ctl.sock,gw2=… evacuate gw1 [--to gw2]
  scripts/cluster_roll.sh --sockets …                                restore  gw1
  ```
  `evacuate` 把 `<node>` 正在服务的每个导出逐个迁走——迁到 `--to`，或迁到该导出 `nodes` 列表
  里 `<node>` 之后的下一个活网关（环绕），每步**轮询目标**到 `role=active`，最后等到
  `<node>` 不再服务任何导出；然后由运维重启/升级它。`restore` 把 `nodes[0]` 是 `<node>` 的
  导出迁回。因为 `cluster migrate` 要在**导出当前属主**上执行，脚本需要每个涉及网关的 ctl
  套接字：`--sockets node=path,…` 或环境变量 `LNFS_CTL_SOCKETS`（远端的先转发到本机）；
  `evacuate` 只需被撤空那台的，`restore` 需要每个当前属主的。`--timeout` /
  `LNFS_ROLL_TIMEOUT` 默认 30 秒。全程无客户端重挂载，详见
  [deployment.md](deployment.md) §6。
- **`gen_seccomp_allowlist.sh [BUILD_DIR]`**——用 strace 跟踪一次真实服务端跑完整
  v3 + v4.1（读写 / 锁）+ v4.2（稀疏 / copy / clone）负载，打印排序后的系统调用集合。改动
  运行时（换 io_uring 路径、加系统调用）之后跑一次，与
  `packaging/systemd/lightnfs.service` 里的 seccomp 白名单比对。对应
  [../testing/security-checklist.md](../testing/security-checklist.md) 第 7 项。

## 与 make / ctest 的关系

```
make format        → trailing_comments.py --fix + clang-format
make format-check  → format_check.sh
make tidy          → tidy.sh
make test          → ctest：lnfs_tests、ctl_offline_catalog（test_ctl_offline.sh）、
                     fuzz_regress_*、errmap_check（gen_errmap_cases.py --check）
scripts/ci.sh      → 以上全部 + 构建矩阵 + 三个后端 ABI 检查（+ nightly 的 bench/fuzz）
```

验收脚本（`accept_*`）与 POSIX 语义检查都不在 `make` 与 ctest 里，按需手动跑；跑出来的
结论汇总在 [../testing/test-report.md](../testing/test-report.md)。
