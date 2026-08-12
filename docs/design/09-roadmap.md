# 9. 路线图与里程碑

合并调研分册的里程碑（nfsv3/09 §9.9 的 M1–M4、nfsv4/11 §11.7 的 M5–M8）并对齐本设计的模块划分。每个里程碑以**真实客户端验收**为完成定义（DoD），不以"代码写完"为准。

## 阶段 0：基建（无协议）

- runtime（Task/uring reactor/offload/同步原语）+ 三层基准（02 分册 2.8）
- xdr 基建 + fuzz 骨架；transport 记录流
- **DoD**：echo/null-RPC 基准达标（单 reactor ≥ 100k rps null-RPC）；TSAN/ASAN 全绿

## 阶段 1（=M1）：v3 只读

- backend API 定稿 + backend_local 只读子集（resolve/getattr/lookup/readdir/read/statfs）
- v3 引擎只读过程 + mountd + rpcbind 注册；core 句柄/导出/errmap
- **DoD**：Linux 客户端 `mount -o vers=3` 后 `ls -lR`/`cat`/`md5sum -c` 全通过；cthon basic 只读部分通过

## 阶段 2（=M2+M3）：v3 读写生产化

- 写路径（UNSTABLE/COMMIT/boot verifier）、创建族全套、DRC、WCC（core::mutate）、fd 缓存完备
- 安全清单（08 分册 8.5 的 1–4、6）落地；`lightnfs-ctl` 最小版
- **DoD**：cthon04 basic/general/special 全过（`-o vers=3`）；fsx 过夜；kill -9 网关重启后客户端无感恢复（重发+verifier 语义验证）

## 阶段 3（=M5）：v4.1 只读

- v4 引擎骨架：COMPOUND 解释器、SEQUENCE/会话层、bitmap 属性层、伪根
- StateMgr 的 client/session 部分；boot epoch/宽限期名单持久化
- **DoD**：`mount -o vers=4.1,ro` 正常；pynfs 4.1 会话组通过；v3/v4 双挂载并发读一致

## 阶段 4（=M6）：v4.1 读写 + 状态

- OPEN/CLOSE/READ/WRITE/COMMIT/SETATTR + 状态表全量、租约回收（courtesy）、grace/reclaim
- **DoD**：cthon basic/general（vers=4.1）；fsx 过夜；服务器重启 reclaim 用例（07 分册 7.5）；租约回收用例（kill 客户端 VM）

## 阶段 5（=M7）：v4 锁与安全完备

- LOCK/LOCKT/LOCKU + 网关 LockMgr；SECINFO；4.4 分册错误白名单全覆盖
- **DoD**：cthon lock 组（vers=4.1）；pynfs 锁相关组；08 分册 8.5 全项验收

## 阶段 6（=M8）：甜点与后端扩展（按需排序）

- v4.2 低成本特性：SEEK/ALLOCATE/DEALLOCATE、同步同服 COPY、CLONE（宣告 minorversion=2）
- 读委托 + 回传通道发送侧（nfsv4/05 §5.6 阶段 2）
- 第二后端启动（Lustre 或 GlusterFS，按 06 分册映射表实现）——**接口冻结的真实检验**
- 可选：NLM/NSM（若出现 v3 锁刚需，按 nfsv3/06 §6.6 选项 2）

## 长期观察项（不承诺）

- RPC-over-TLS（RFC 9289）：auth 插槽 + 传输层已预留位置
- 写委托 / 目录委托 / pNFS flexfiles MDS：均有明确的"值得做的前提"（nfsv4 分册相应章节），出现前提再评估
- 多网关一致性：依赖 kNativeChange + kByteLocks 后端（05 分册 5.6/5.8 已定义边界）

## 风险登记

| 风险 | 缓解 |
|------|------|
| 自研 runtime 的正确性 | 阶段 0 独立交付 + fake ring 时序测试 + 三层基准回归（02 分册 2.8） |
| v4 状态机复杂度失控 | 严格按 nfsv4/11.4 骨架清单裁剪；pynfs 持续集成；每请求摘要日志先行 |
| 后端接口第一次真实扩展时返工 | 06 分册两张映射表作为接口评审的验收材料；5.10 演进规则约束 |
| open_by_handle_at 特权依赖 | 降级模式（6.1）预设计；容器部署文档给 capability 配置 |
| io_uring 内核版本差异 | epoll 兜底构建 + 运行时探测特性（statx/copy_file_range 的 uring 支持） |
