# 2. 协程运行时与并发模型

## 2.1 选型：自研薄运行时（决策 D1）

候选对比：

| 方案 | 优点 | 排除理由 |
|------|------|----------|
| Boost.Asio awaitable | 成熟、生态好 | 文件 IO 支持弱（io_uring 集成不完整）、缓冲区所有权模型与零拷贝路径别扭、拖入 Boost 依赖 |
| Seastar | shard-per-core 性能极致 | 框架侵入性极强（自带内存分配/网络栈），后端库（libgfapi 等阻塞库）难以共存 |
| **自研薄运行时** | 完全掌控缓冲区生命周期与 uring 提交；代码量可控（`src/runtime/` 现约 3k 行）；后续可换实现 | 需要自己写对、自己测（用成熟模式抄，风险可控） |

自研范围刻意压到最小：`Task<T>`、reactor（io_uring/epoll）、offload 池、少量同步原语、定时器。**不做**：泛化 executor 概念、work-stealing、通用 channel 库。

## 2.2 核心类型

```cpp
namespace lnfs::rt {

// 惰性协程任务：co_await 时才挂到当前 reactor 执行
template <class T> class Task {
public:
    using promise_type = /* ... */;
    auto operator co_await() &&;   // 移动即消费，禁止拷贝
};

// 分离启动（连接主循环等顶层协程）
void spawn(Task<void> t, Reactor& r);

// 当前线程的 reactor（TLS）
Reactor& current_reactor();

// io_uring 封装：全部返回 Task，错误以负 errno 表达，由上层包成 Result
Task<int>     uring_read (int fd, std::span<std::byte> buf, uint64_t off);
Task<int>     uring_write(int fd, std::span<const std::byte> buf, uint64_t off);
Task<int>     uring_fsync(int fd, bool datasync);
Task<int>     uring_writev(int fd, std::span<const iovec> iov, uint64_t off);
Task<int>     uring_recv (int fd, std::span<std::byte> buf);
Task<int>     uring_sendv(int fd, std::span<const iovec> iov);
Task<int>     uring_accept(int listen_fd, sockaddr* peer, socklen_t* len);
Task<int>     uring_statx(int dirfd, const char* path, int flags, unsigned mask, struct statx* out);
Task<int>     uring_openat(int dirfd, const char* path, int flags, mode_t mode);
Task<int>     uring_close(int fd);
Task<int>     uring_cancel_fd(int fd);
// （示意签名；以 runtime/io.hpp 为准）

// 无 uring 原语或第三方阻塞库：切到 offload 池执行，完成后切回原 reactor。
// 两个作业类（kLight 默认 / kHeavy = fsync、fallocate、copy 一类），各有线程配额与
// 队列上限（[server] offload_heavy_threads / offload_queue_cap）
enum class OffloadClass { kLight, kHeavy };
template <class F> Task<std::invoke_result_t<F>> offload(F fn, OffloadClass cls = OffloadClass::kLight);

// 定时与超时
Task<void> sleep_for(std::chrono::nanoseconds d);
template <class T> Task<std::optional<T>> with_timeout(Task<T> t, std::chrono::nanoseconds d);

} // namespace lnfs::rt
```

约定：

- `Task<T>` 惰性启动、单消费者、在 await 者所在 reactor 恢复；`offload()` 是**唯一**跨线程点，其恢复必定回到发起 reactor —— 由此，除显式分片结构外，业务代码可当单线程写。
- 协程帧分配：promise 定制 `operator new` 走线程局部按大小分级的空闲链表（`runtime/frame_alloc.hpp`，64B 粒度到 4KB，跨线程释放只迁移槽位），避免全局 malloc 争用——已落地。
- 异常策略：运行时内部不用异常表达 IO 错误（负 errno）；业务层用 `Result<T>`；协程内未捕获异常终止进程前打印任务链（fail-fast，宁崩不静默错）。

## 2.3 Reactor

```cpp
class Reactor {
    io_uring ring_;               // 主 uring；SQPOLL 可配
    TimerWheel timers_;
    MpscQueue<std::coroutine_handle<>> remote_wakeups_;  // offload 完成/跨分片唤醒
    void run();                   // 提交 SQE → 等 CQE → 恢复对应协程 → 处理到期定时器
};
```

- 每 CQE 携带 `user_data = 等待协程句柄`，完成即 `resume`——经典 proactor。
- epoll 兜底实现（老内核）：同一接口（`RingOps`），socket 走 readiness + 非阻塞调用，文件 IO 全部 offload。两种 ring 总是同时编译，运行时按 `[server] ring = auto|uring|epoll` 探测/指定（CMake `LNFS_RING` 只改默认探测顺序）。
- reactor 间通信仅两种：`spawn_on(reactor, task)` 与 `remote_wakeups_`；不做任意跨线程 await。

## 2.4 同步原语（协程版）

共享结构（v4 状态表、DRC、fd 缓存、per-object 锁）需要不阻塞线程的互斥：

```cpp
class AsyncMutex {          // FIFO 公平；持锁跨 co_await 合法
    Task<Lock> lock();
};
class AsyncSharedMutex {    // per-object 读写锁（WCC 原子采样用）
    Task<SharedLock> lock_shared();
    Task<Lock>       lock();
};
class AsyncCondVar;         // 宽限期结束等待、DRC in-progress 等待
class Semaphore;            // offload 池容量、每连接在途请求数
```

**分片纪律**：所有全局表一律 `Sharded<T, N>`（key hash 选片，每片一个 AsyncMutex）。锁顺序全局规约：`client 片锁 → 文件 objlock → 状态表片锁`，禁止反向（见 07 分册死锁矩阵）。

## 2.5 per-object 串行化（NFS 语义的落点）

v3 WCC 与 v4 change_info 要求"before/after 原子采样"（调研分册 nfsv3/07、nfsv4/03）：

```cpp
// core/obj_lock.hpp — 以 (fsid, ObjId) 为粒度的读写锁注册表（64 分片 + weak_ptr，空锁即回收）
class ObjLockRegistry {
    std::shared_ptr<rt::AsyncSharedMutex> get(uint32_t fsid, const ObjId&);
    // 调用方：get(fsid, oid)->lock_shared()（READ/GETATTR…）或 ->lock()（WRITE/SETATTR/目录修改…）
};
```

- 修改类操作模板：`exclusive(obj) → before=getattr → 后端操作 → after=getattr → 放锁`。RENAME 双目录按 ObjId 字典序取锁。
- 读多写少，共享锁让并发 READ 不互斥；同文件重叠 WRITE 天然串行（顺带满足"重叠写保序"）。
- 这是网关级锁，只保护"采样+操作"的原子性，不等于 NLM/v4 字节锁（那在 07 分册的 LockMgr）。

## 2.6 取消、超时与关闭

- 取消模型：协作式。每请求上下文携带 `CancelToken`（连接断开时置位）；长链路点（offload 前、循环内）检查。**不做**协程强制销毁——uring 在途操作用 `IORING_OP_ASYNC_CANCEL` 尽力取消，等不到就等自然完成后丢弃结果。
- 连接关闭：置 token → 停收 → 在途请求自然收敛（应答写入被丢弃）→ 关 fd。v4 会话不随连接死（见 07 分册）。
- 全局关闭：Reactor 停止接受新 spawn，drain 有界时间。

## 2.7 缓冲区管理

- `Buffer`：引用计数的定长块（64KiB 级）+ `BufferChain`；接收侧 record_stream 直接组链，XDR 解码器在链上游走（大 opaque 字段返回链内 span，**不拷贝**）。
- 发送侧：应答头编码进小 buffer，READ 数据独立大 buffer（uring pread 直写），`writev` 拼接——READ 全路径零拷贝（详见 03 分册 3.6）。
- 池化：每监听器一个 `BufferPool`——大小分级 4K/64K/128K/256K/1M，线程局部 magazine（每类 16 块）之上是带锁的全局空闲链表，水位 `max_free_bytes` 内置 64MiB（无配置键；`lightnfs_buffer_pool_free_bytes{listener}` 可观测）。

## 2.8 测试策略

- 运行时单测不依赖真实 uring：`Reactor` 之下抽一层 `RingOps` 供 fake 注入（时序穿插、注入 EINTR/短读）。
- 压测基准：echo 服务器（传输层）、null-RPC（L2）、伪后端（L4 以上全链路，memory 后端零延迟）——三层基准锁定各层开销预算；随 `lightnfs-ctl bench echo|nullrpc|fullpath` 交付，`scripts/bench_gate.sh` 对照 `tools/bench/baseline.txt` 做地板门禁（用法见 [performance/benchmarks.md](../performance/benchmarks.md)）。
- TSAN/ASAN 全量跑单测（`scripts/ci.sh` 矩阵）；协程生命周期错误（悬垂 frame、双恢复）依赖 sanitizer 构建捕获，没有单独的 frame 哨兵机制。
