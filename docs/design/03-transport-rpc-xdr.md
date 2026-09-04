# 3. 传输与 RPC/XDR 层

协议格式依据：nfsv3 分册 [02-rpc-xdr.md](../nfsv3/02-rpc-xdr.md)。本文只写实现结构。

## 3.1 监听与连接

```cpp
// transport/listener.cpp — 每 reactor 一个 SO_REUSEPORT 监听套接字与各自的 accept 循环
Task<void> accept_loop(Reactor& r, int listen_fd) {
    for (;;) {
        int fd = co_await rt::uring_accept(listen_fd, ...);
        rt::spawn(connection_main(fd, peer), r);  // 连接在被 accept 的 reactor 上服务
    }
}
```

- 内核在各 reactor 的 SO_REUSEPORT 套接字间负载均衡，没有单点 accept 串行化、没有跨 reactor 移交（plan doc 10 §2.3 改造；原设计是单 accept 线程轮转指派）。连接总数/per-peer 上限仍是全局共享计数。
- 2049（NFS v3+v4 共口）与 MOUNT 端口（默认 20048）两组 listener，由 `server/frontend.cpp` 的 `Frontend::start` 连同 TLS 上下文、ctl 套接字、metrics 端点与 rpcbind 注册一起装配；`[server] bind` 指定监听地址（空 = 双栈全接口）。rpcbind 只做**注册**（`server/rpcbind.cpp`，`[server] rpcbind = true`），不内嵌 portmap 服务（配置键 `builtin_portmap` 被解析但没有实现，保留为占位）。
- 连接数上限（默认 4096），超限 accept 后立即关闭并计数告警。
- per-peer 连接数限制（防单客户端耗尽）。

## 3.2 记录流（record marking）

```cpp
// transport/record_stream.cpp
class RecordStream {  // 一条 TCP 连接上的 RPC 记录读写
    Task<Result<BufferChain>> read_record();   // 处理多片段、片段上限、总长上限
    Task<Result<void>>        write_record(SendBuf&&);  // per-conn 发送队列串行化
};
```

- `read_record` 状态机：读 4B 标记 → 校验长度（片段 ≤ `max_fragment`，累计 ≤ `max_request_size`）→ 读体 → 若非 last 片段继续。违规即返回错误 → 连接关闭（协议层无法恢复帧同步）。
- 发送：应答就绪即入队；队列深度按字节计水位，超水位反压该连接的新请求解析（正常不会触发——应答受请求数上限约束）。

## 3.3 连接主循环

```cpp
Task<void> connection_main(int fd, Peer peer) {
    ConnCtx ctx{...};                          // peer、cancel token、v4 会话绑定表
    Semaphore inflight{cfg.max_inflight_per_conn};  // v3 路径的并发上限（[limits] inflight_per_conn）
    for (;;) {
        auto rec = co_await ctx.rs.read_record();
        if (!rec) break;
        co_await inflight.acquire();
        rt::spawn(handle_request(ctx, std::move(*rec), inflight), rt::current_reactor());
    }
    ctx.cancel.request();                      // 见 02 分册 2.6
    co_await ctx.drain();
}
```

请求并发（流水线）是性能关键：解析出一条记录立刻继续读下一条，处理协程各自独立完成。

## 3.4 RPC 层

```cpp
// rpc/dispatch.cpp
Task<void> handle_request(ConnCtx& ctx, BufferChain rec, Semaphore& s) {
    auto call = rpc::parse_call(rec);           // xid/prog/vers/proc/cred/verf
    if (!call) { /* 编码 RPC 层错误应答或丢弃 */ }
    switch (call->prog) {
      case 100003:
        if      (call->vers == 3) co_await v3_engine.dispatch(ctx, *call);
        else if (call->vers == 4) co_await v4_engine.dispatch(ctx, *call);
        else    reply_prog_mismatch(lo, hi);    // 版本范围来自已注册程序表
      case 100005: co_await mountd.dispatch(ctx, *call);      // vers==3
      default:     reply_prog_unavail();                      // 无内嵌 portmap
    }
}
```

分层错误纪律（调研分册 nfsv3/02 的 CALL/REPLY 语义）：

- RPC 头解析失败/rpcvers≠2 → MSG_DENIED(RPC_MISMATCH)；认证失败 → MSG_DENIED(AUTH_ERROR)；
- 参数 XDR 解码失败 → GARBAGE_ARGS；引擎内部异常 → SYSTEM_ERR；
- 语义错误一律在 NFS 层表达（nfsstat3/nfsstat4）。

## 3.5 认证与身份

```cpp
// rpc/auth.cpp
struct Cred { uint32_t uid, gid; SmallVec<uint32_t, 16> gids; AuthFlavor flavor; };
Result<Cred> authenticate(const RpcCall&, const ExportEntry&);  // AUTH_NONE/AUTH_SYS
```

- AUTH_SYS 解析 + 按导出配置 squash（root_squash/all_squash → anon uid/gid）在**此处一次完成**，引擎与后端只见映射后的 `Cred`。
- flavor 插槽：`Authenticator` 接口注册表，未来挂 RPCSEC_GSS 通道属性，接口位置预留、v1 只有两个实现。
  **RPC-over-TLS（RFC 9289）已落地**（plan doc 10 §5.4）：它是传输绑定而非 auth flavor——
  AUTH_TLS(7) 只在 NULL 过程上探测触发 STARTTLS，握手后 `transport::TlsConn` 就在同一连接上
  加密收发，不经 `Authenticator`；`AuthFlavor` 仍只有 NONE/SYS 两个真实身份实现。
- MOUNT/NFS 层导出 IP 校验也在此层入口做（`ExportTable::check_client(peer, export)`），v4 伪根除外（允许任意来源浏览伪根、进导出时校验）。

## 3.6 XDR 基建

```cpp
// xdr/xdr.hpp — 面向缓冲链的游标式编解码
class XdrDec {
    Result<uint32_t>  u32();  Result<uint64_t> u64();
    Result<std::span<const std::byte>> opaque(uint32_t max);  // 字段在单块内时引用输入链，跨块则聚合到解码器暂存
    Result<SmallVec<std::span<const std::byte>>> opaque_spans(uint32_t max); // 跨块也零拷贝：返回分段（WRITE 路径）
    Result<std::string_view>           string(uint32_t max);
    // 越界/超 max 一律 Err(kGarbage)
};
class XdrEnc {
    void u32(uint32_t); void u64(uint64_t);
    void opaque(std::span<const std::byte>);
    void raw_gap(size_t n) -> span;      // 预留后填（COMPOUND 的 status、记录标记长度）
    void attach(Buffer data);            // 零拷贝挂接 READ 数据段（对齐 4B，自动补 pad）
};
```

- v3/v4 消息结构体（`nfs3_types.hpp` / `nfs4_types.hpp`）手写 + 以 RFC .x 文件为注释对照；每类型 `encode/decode` 配对 round-trip 单测 + fuzz 目标（libFuzzer 直喂 `handle_request` 输入）。
- READ 零拷贝路径：引擎先编码头部（含 opaque 长度字段），`attach()` 挂数据 buffer，`write_record` 用 writev 发送——数据从 uring pread 到 socket 无一次 memcpy。
- WRITE 零拷贝路径：解码时 `opaque_spans()` 返回接收链内的分段 span，引擎以 `std::span<const iovec>` 交给后端的向量化 `write()`（本地后端 `IORING_OP_WRITEV`），跨接收块的负载不再拼平（plan doc 10 §2.4）；buffer 引用计数保活到写完成。

## 3.7 DRC（v3 专用，放在 RPC 层与引擎之间）

依据 nfsv3 分册 9.2。v4.1 有会话槽缓存，不经过 DRC。

```cpp
// rpc/drc.cpp
class Drc {   // Sharded；key = {peer_addr, xid, prog, vers, proc, args_checksum}
    enum class Hit { kMiss, kInProgress, kDone };
    Task<std::variant<Miss, Wait, CachedReply>> begin(const Key&);
    void complete(const Key&, SendBuf reply_copy);   // 仅非幂等过程缓存
};
```

- 非幂等过程（SETATTR/CREATE/MKDIR/SYMLINK/MKNOD/REMOVE/RMDIR/RENAME/LINK）才进 DRC；READ/GETATTR 等旁路。
- `kInProgress`：重复请求到达而原请求在途 → 挂 AsyncCondVar 等原应答，绝不并发重执行。
- 容量：LRU，条目 TTL 默认 120s，总内存上限配置；应答副本只存头+小体（超限的大应答不缓存，重放时回退重执行——只对幂等安全的才允许此回退，否则宁可缓存）。

## 3.8 MOUNTv3 与 portmap

- mountd：NULL/MNT/EXPORT 全实现，DUMP/UMNT/UMNTALL 为兼容空实现（nfsv3 分册 05 的建议）；MNT 成功返回根句柄 + `[AUTH_SYS]`。
- portmap：不内嵌。`server/rpcbind.cpp` 在启动/退出时向系统 rpcbind 直发 SET/UNSET（UDP，无 libtirpc 依赖），`[server] rpcbind = false` 关闭（纯 v4 部署，或 v3 客户端用 `port=/mountport=` 直连）。
