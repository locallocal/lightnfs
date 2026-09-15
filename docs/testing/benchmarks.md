# 性能基准工具使用指南

lightnfs 自带三层基准（设计 02 分册 §2.8），全部内置在 `lightnfs-ctl bench` 子命令里：
一个进程内起服务端栈，再用阻塞 socket 的客户端线程打满流水线，报告吞吐（rps）。
它们不依赖任何挂载、rpcbind 或 root 权限，是"每次改动跑一下"的回归工具；真实挂载上的
fio/mdtest 属于验收（见 §6）。

| 层 | 子命令 | 覆盖路径 | 回答的问题 |
|----|--------|----------|-----------|
| L1 传输 | `bench echo` | accept → 记录标记帧读 → 零拷贝原样写回 | 传输层 + 记录流 + buffer 池的每记录开销 |
| L2 RPC | `bench nullrpc` | L1 + RPC 头解析、AUTH_SYS、程序分发、应答编码（NULL 过程） | RPC 层开销；阶段 0 出口门禁 = 单 reactor ≥ 100k rps |
| L4 全链路 | `bench fullpath` | L2 + v3 引擎 + core（句柄 HMAC、对象锁、errmap）+ **memory 后端**（零延迟） | 协议栈在 RPC 之上的额外开销：GETATTR（元数据路径）与 READ 4k（数据路径，`attach()` 零拷贝段） |

后端是 memory（`src/backend/memory/`），所以 fullpath 数字是"协议栈上限"，不含磁盘或集群。

## 1. 构建

基准随 `lightnfs-ctl` 编译，用 Release 构建：

```sh
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j$(( $(nproc) / 2 )) --target lightnfs-ctl
```

Debug/ASAN/TSAN 构建下的数字没有意义（`scripts/ci.sh` 只在 `build-rel` 上跑门禁）。
ring 后端跟随构建默认（`LNFS_RING=auto` → 探测 io_uring）；要对比 epoll 兜底路径，用
`-DLNFS_RING=epoll` 单独配置一个构建目录（仓库惯例 `build-epoll`），输出里的 `ring=`
字段会标明实际用的是哪一个。

## 2. 命令与参数

三个子命令都是位置参数，省略取默认值：

```
lightnfs-ctl bench echo     [reactors=1] [conns=8] [per_conn=20000] [pipeline=32] [payload=128]
lightnfs-ctl bench nullrpc  [reactors=1] [conns=8] [per_conn=50000] [pipeline=64]
lightnfs-ctl bench fullpath [reactors=1] [conns=8] [per_conn=50000] [pipeline=64] [proc=getattr|read]
```

| 参数 | 含义 |
|------|------|
| `reactors` | 服务端 reactor 线程数（连接经 `SO_REUSEPORT` 由内核分配到各 reactor） |
| `conns` | 客户端连接数 = 客户端线程数 |
| `per_conn` | 每连接发送的请求数；总请求数 = `conns × per_conn` |
| `pipeline` | 每连接同时在途的请求数（流水线深度）；1 = 严格请求/应答串行 |
| `payload` | （echo）每条记录的字节数 |
| `proc` | （fullpath）`getattr`（默认）或 `read`：READ 4096 字节，文件 `/bench.bin` 由基准预先放入 memory 后端 |

`lightnfs-ctl help bench` / `lightnfs-ctl help bench <name>` 打印同样的用法。

## 3. 输出与退出码

每个基准在结束时打印一行，`scripts/bench_gate.sh` 只解析最后一行的 `= N rps`：

```
bench_echo:     ring=uring reactors=1 conns=4 pipeline=32 payload=128 -> 80000 records in 0.213s = 375000 rps
bench_nullrpc:  ring=uring reactors=1 conns=4 pipeline=32 -> 80000 calls in 0.229s = 350000 rps
bench_fullpath: proc=GETATTR ring=uring reactors=1 conns=4 pipeline=32 -> 80000 calls in 0.232s = 345000 rps
bench_fullpath: proc=READ4k  ring=uring reactors=1 conns=4 pipeline=32 -> 80000 calls in 0.291s = 275000 rps
```

- 基准跑完直接 `_exit()`（reactor 可能还停在 accept 上，不走正常关闭序）。
- `bench nullrpc` 在 `reactors=1` 且结果低于 100k rps 时在行尾追加
  `[BELOW 100k SINGLE-REACTOR TARGET]` 并以退出码 **2** 结束——这就是阶段 0 的出口门禁。
- 其余情况退出码 0；监听失败等启动错误打印到 stderr 并非零退出。

## 4. 回归门禁：`scripts/bench_gate.sh`

```sh
scripts/bench_gate.sh [BUILD_DIR]        # 默认 build-rel
LNFS_BENCH_FLOOR=0.2 scripts/bench_gate.sh build-rel
LNFS_BENCH_CALLS=5000 scripts/bench_gate.sh    # 快速冒烟
```

门禁固定用 `reactors=1 conns=4 pipeline=32`（echo 另加 `payload=128`），每连接
`LNFS_BENCH_CALLS`（默认 20000）次调用，跑四项并与 `tools/bench/baseline.txt` 比较：

```
nullrpc       350000
echo          375000
fullpath      345000
fullpath_read 275000
```

任何一项低于 `基线 × LNFS_BENCH_FLOOR` 即失败（退出码 1）。地板默认 **0.5**；共享 CI runner
之类嘈杂环境可降到 0.2。门禁的目的是抓数量级退化（例如误把异步路径改成同步），不是 10%
的漂移——这类变化用 §5 的方法手工测。

接入方式：

- `scripts/ci.sh nightly` 在完整矩阵之后跑一次门禁（`build-rel`）；
- 配置 `-DLNFS_ENABLE_BENCH_GATE=ON` 可把它注册为 ctest 用例 `bench_gate`
  （只在 Release 构建目录上打开，且机器要安静）。

**更新基线的规则**（`baseline.txt` 头部注释同样写明）：只在一次有意为之、已测量的性能
变化之后更新，用同一台机器、同一参数跑三次取中位数；绝不为了让门禁通过而调数字。

## 5. 手工测量建议

- **安静的机器**：关闭其他负载；本仓库的构建/测试约定只用一半核，基准本身请单独跑。
- **先看单 reactor**：`reactors=1` 隔离协议栈开销；再用 `reactors=N` 看扩展性。
  `conns` 至少等于 `reactors`，否则有 reactor 空转。
- **流水线深度**决定客户端能否把服务端打满：`pipeline=1` 测的是往返时延而非吞吐。
- **多跑几次**：8000 次调用只需几十毫秒，噪声很大；用默认的 20000–50000/连接，取中位数。
- **对照 ring**：同一参数分别在 `build-rel`（uring）与 `build-epoll` 上跑，差值就是
  io_uring 带来的收益。
- **定位在哪一层**：三层数字相减——`echo − nullrpc` ≈ RPC 层每请求成本，
  `nullrpc − fullpath` ≈ 引擎 + core 成本；READ4k 与 GETATTR 之差 ≈ 数据段挂接与 4k 应答编码。
- 结果与最近实测见 [test-report.md](test-report.md) §4。

## 6. 真实挂载上的性能验收

三层基准不覆盖后端 IO 与内核客户端行为。需要端到端数字时，在 root 机器上真实挂载
（`scripts/accept_m2_vm.sh` / `accept_m6_vm.sh` 的环境）并用通用工具：

```sh
mount -t nfs -o vers=4.2 server:/srv/data /mnt
fio --name=seqread --directory=/mnt --rw=read  --bs=1M --size=2G --numjobs=4 --direct=1
fio --name=randrw  --directory=/mnt --rw=randrw --bs=4k --size=1G --numjobs=8 --iodepth=16
mdtest -d /mnt/meta -n 10000 -i 3            # 元数据操作（创建/stat/删除）
```

跑负载的同时从服务端拿协议层时延分布（固定桶直方图，可算 p99）：

```sh
lightnfs-ctl metrics | grep -E 'lightnfs_(v3_duration|v4_op_duration|v4_compound_duration)_seconds'
lightnfs-ctl metrics | grep -E 'lightnfs_(offload_queue_depth|reactor_loop_duration_seconds)'
```

`[server] slow_request_ms` 超阈值的请求会带逐 op 耗时分解落 warn 日志，是定位单笔慢
请求的第一工具（08 分册 §8.4）。集群后端（gluster / cephfs）全部调用走 offload 池，
`offload_threads` 是它们的吞吐上限旋钮，先看 `lightnfs_offload_queue_depth` 再调。
