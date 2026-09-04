# 4. 协议核心：v3/v4 双引擎与共享语义层

## 4.1 结构总览

```
nfsv3 (22 procs) + mountd        nfsv4 (COMPOUND 解释器 + op_*)
        │                                 │
        └────────────┬────────────────────┘
                     ▼
              core/（协议无关语义中枢，一组头文件级组件而非单个类）
   导出表 / 句柄编解码 / Cred+权限 / ObjLockRegistry /
   属性采样(WCC & change_info 统一实现) / verifier / 游标簿记
                     ▼
              backend::Backend（05 分册）
```

原则：**引擎只做"协议格式 ↔ core 调用"的翻译**。任何一处业务判断（权限、句柄、原子采样、错误映射）都必须落在 core，防止 v3/v4 语义漂移。两个引擎、MOUNT、DRC、per-object 锁注册表与 StateMgr 由 `server/protocol_stack.{hpp,cpp}` 的 `ProtocolStack` 按依赖顺序构造并挂到同一个 RPC dispatcher（`CoreState` 提供导出表、句柄密钥与 boot epoch）。

## 4.2 core 的关键服务

最初设计为一个 `NfsCore` 类 + `OpCtx` 上下文；实现落地为 `core/` 下一组独立组件，
引擎持有它们的引用（每请求上下文是 `transport::ConnCtx` + 引擎内的局部结构，v4 的
逐 op 计时在 `CompoundCtx::OpSpan`）：

```cpp
namespace lnfs::core {
// 句柄 ↔ 对象（core/file_handle.hpp）
class FileHandleCodec {            // SipHash-2-4 HMAC，密钥 state_dir/hmac.key
    Result<Decoded> decode(span fh, peer);      // 校验 HMAC / 导出 / 来源 IP → {ExportEntry&, ObjId}
    Result<Decoded> decode_v4(span fh, peer);   // 伪根 fsid=0 分支
    FhBuf           encode(const ExportEntry&, const ObjId&);
    Inspect         inspect(span fh);           // lightnfs-fh 工具用
};
// 原子采样（core/mutate.hpp）：v3 WCC 与 v4 change_info 的唯一实现点
class MutateGuard;                 // precheck(readonly→名字) → enter(squash→排序取锁→before) → finish(after)
struct ChangeSample;               // before/after 的 {change, mtime, ctime, size} 采样
// 名字校验（core/names.hpp）：check_component / valid_component / valid_utf8
// caps/limits → 协议属性（core/fs_props.hpp）：fs_props()
// per-object 锁（core/obj_lock.hpp）：ObjLockRegistry::get(fsid, oid)
// 导出表 / squash（core/config.hpp）：ExportTable::{find, check_client, squash_cred}
// write verifier / stateid epoch（core/boot_epoch.hpp）：boot_epoch()
// 权限（backend/api.hpp）：Object::access() 默认实现 = getattr + mode 位；kNativeAccess 后端覆写
}
```

- `mutate()` 封装 02 分册 2.5 的"exclusive 锁 → before → 后端 op → after"模板，双目录（RENAME/LINK）传 secondary，按 ObjId 排序取锁。实现落地为 `core::MutateGuard`（`src/core/mutate.hpp`）：precheck（readonly → 名字校验）→ enter（squash → 排序取锁 → before 采样）→ finish（after 采样），v3/v4 引擎只做编码。
- 权限模型：core 在后端操作前做**协议层检查**（导出只读→ROFS、squash 后的 uid 对 mode 位的快速判定、属主放宽惯例 nfsv3/04 §6），后端返回的 EACCES/EPERM 仍是最终权威（Lustre/Gluster 有服务端 ACL）。ACCESS 过程直接问后端 `access()`（能力位支持时）或用协议层判定兜底。

## 4.3 文件句柄（两版通用，决策 D3）

```
布局（总长 ≤ 64B，v3 上限，v4 天然兼容）：
┌────────┬─────────┬──────────────┬─────────────┐
│ ver(1) │ fsid(4) │ backend_oid  │ hmac(8)     │
│ =0x01  │ 导出 id │ ≤ 51B 变长    │ SipHash-2-4 │
└────────┴─────────┴──────────────┴─────────────┘
```

- `backend_oid` 即 05 分册的 `ObjId::bytes`——**后端负责其持久性与唯一性**，core 只透传。
- HMAC 密钥：持久化于状态目录（首次启动生成）；校验失败 → BADHANDLE；fsid 不在导出表 → v3 STALE / v4 STALE；导出存在但 peer 无权限 → ACCES（NFS 层导出校验，nfsv3 分册 9.6 红线）。
- v4 伪根：fsid=0 保留，`ObjId` 为伪节点编号；伪 fs 由 core 内置（只读合成目录，见 nfsv4 分册 03），不经后端。

## 4.4 v3 引擎

- 22 个过程（含 NULL）由一张 `Proc` 索引的成员函数指针表分发，签名 `Task<void> proc_xxx(ConnCtx&, RpcCall&, const rpc::Cred&, Capture*)`；READDIR/READDIRPLUS 与 FSSTAT/FSINFO/PATHCONF 各共用一个处理器。
- 全部过程实现（nfsv3 分册 04），要点：
  - READDIR/READDIRPLUS 共用 core 游标簿记 + 后端 `readdir()`（cookie 语义契约见 05 分册 5.7）；cookieverf = 目录 change 属性，cookie≠0 时校验，不符回 BAD_COOKIE 让客户端从头重列（原设计"恒 0"已弃）。
  - WRITE：stable 三档直通后端 `write()+commit()`；verifier 用 `boot_verf()`。
  - CREATE EXCLUSIVE：verifier 存 atime/mtime（后端 `setattr` 原子带入），语义按 nfsv3/04 §8。
  - 失败分支尽量带 post_op_attr/WCC（core `mutate` 失败路径同样采样 after）。
- mountd 直接调 core：`decode_path → backend.lookup 链 → encode_fh`。

## 4.5 v4 引擎

```cpp
// nfsv4/engine.cpp（单文件：COMPOUND 解释器 + 全部 op_* 成员函数）
Task<void> Engine::dispatch(ConnCtx& conn, RpcCall& call) {
    CompoundCtx c{ .cfh = {}, .sfh = {}, .minor = args.minorversion, ... };
    if (args.minorversion == 0) return encode_error(MINOR_VERS_MISMATCH);   // 决策 D5
    // SEQUENCE 前置校验（会话/槽/重放，07 分册）……重放命中直接回缓存
    for (auto& op : args.ops) {
        auto st = co_await ops_table[op.code](c, op, enc);
        if (st != NFS4_OK) break;                       // 遇错即停
    }
    // 槽缓存回填（cachethis）、租约续期
}
```

- 每操作一个 `op_xxx` 成员函数（同在 `nfsv4/engine.cpp`），签名统一 `Task<nfsstat4>(CompoundCtx&, ArgView, XdrEnc&)`；CFH/SFH 在 `CompoundCtx` 中。
- 实现集 = nfsv4 分册 11.4 的骨架清单 1–5 组全部：会话/状态/IO/目录写/锁，加读委托（DELEGRETURN、回传通道 CB_RECALL/CB_NOTIFY_LOCK，07 分册 7.7）与 v4.2 的 SEEK/ALLOCATE/DEALLOCATE/COPY/CLONE/READ_PLUS（宣告 minorversion=2）；其余 NOTSUPP（RFC 8276 的 xattr op 72–75 在 minor 2 回 NOTSUPP、minor 1 回 OP_ILLEGAL）。属性层 = `attr_id → {getter, setter?, encoder}` 注册表（`nfsv4/attrs.cpp`），getter 从 core `Attr`/`FsProps` 取值。
- OPEN：claim NULL/FH/PREVIOUS；share reservation 检查在 StateMgr（07 分册）；创建路径复用 core `mutate` + 后端 `open(CREATE)`。
- READ/WRITE stateid 校验顺序：特殊 stateid → 状态表查询 → 模式检查（OPENMODE）→ 落到与 v3 相同的后端 IO 调用。
- 应答大小预算：`XdrEnc` 带 `limit`；READDIR 按剩余预算截断，超限操作回 REP_TOO_BIG（nfsv4/11.4）。

## 4.6 错误映射（决策 D6）

```cpp
// core/errmap.hpp
nfsstat3 to_v3(Errno e, ProcId p);   // 按过程白名单过滤（nfsv3 分册 08 表）
nfsstat4 to_v4(Errno e, OpId op);    // 同上（RFC 8881 §15.2 表）
```

- 后端只说 POSIX errno（`Errno` 强类型），引擎边界一次映射 + 白名单校验：映射结果不在该过程允许集内 → 记 warning 并降级为 IO/SERVERFAULT。白名单表从调研分册的错误表生成（constexpr 数组 + 单测对照）。
- `Errno::kJukebox`（后端可返回，如 Lustre HSM 上线）→ v3 JUKEBOX / v4 DELAY。

## 4.7 readdir 游标簿记

后端契约要求 cookie 稳定（05 分册 5.7），core 只做两件事：

1. cookie 0/1/2 保留段处理：合成 `.`/`..` 项（属性来自 getattr），后端 cookie 从 3 起映射（后端原生 cookie 若与保留段冲突，core 做 +3 偏移的双向换算——对 telldir 型后端偏移在 core 统一处理，后端不感知）。
2. READDIRPLUS/v4 READDIR 逐项 fh/attr 组装与预算控制（dircount/maxcount 双预算，nfsv3/04 §17）。

## 4.8 v3/v4 行为一致性清单（回归测试锚点）

| 语义 | 唯一实现点 |
|------|-----------|
| 句柄编解码/校验 | core::decode_fh |
| WCC 与 change_info 采样 | core::MutateGuard / core::ChangeSample（core/mutate.hpp） |
| 名字合法性校验 | core::check_component（core/names.hpp） |
| caps/limits → 协议属性推导 | core::fs_props（core/fs_props.hpp） |
| write verifier | core::boot_verf |
| squash 与属主放宽 | core 权限层 |
| readdir cookie 空间 | core 游标簿记 + 后端契约 |
| errno 映射 | core::errmap（两张白名单表） |

VM 验收脚本（`scripts/accept_m2_vm.sh` vers=3、`accept_m6_vm.sh` vers=4.2）跑同一 fsx/cthon 负载，加上回环脚本里的 v3/v4 双挂载对比与混合版本写，验证最终文件状态一致（`scripts/ci.sh` 本身不跑 fsx/cthon）。
