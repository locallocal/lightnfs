# 多网关无感故障切换——待决事项与后续项

> 背景：多网关主备的实现计划（原 `docs/design/10-multi-gateway-failover-steps.md`）的
> 阶段 A–E 已全部完成并合入（2026-09-05）。那份逐步拆分文档在完成后撤下，**每步的详细
> 实现记录保留在 git 历史里**；当前的实现总览见 [09 册](design/09-multi-gateway-failover.md) §9.10 改动清单，
> 设计动机见 [09 册](design/09-multi-gateway-failover.md)，部署与运维见 [deployment.md §5 多网关主备](deployment.md)。
>
> 本文只收口三类还没闭环的东西：**刻意不做的取舍**、**依赖目标环境未验证的部分**、
> **本机开发环境跳过的门槛**。

## 1. 刻意不做的取舍（原 §10.3）

- **接管时 epoch 的 32 位截断**：clientid 高 32 位与 stateid.other 前 4 字节只取 epoch 低
  32 位（`src/state/state_mgr.cpp`）。全局 epoch 每次接管 +1，2^32 次接管内不会回绕，
  不处理。**决定：不修**。
- **围栏用墙钟（NTP 依赖）**：围栏租约用 `CLOCK_REALTIME` 跨节点比较，容忍 500ms 偏差
  （`kFenceSkewTolerance`）。因此所有网关**必须开 NTP**（部署文档已写明）。若日后把
  `ClusterStore` 换成"经后端 API 写入导出树"的实现（09 §9.4），`ClusterStore` 接口不变，
  可去掉墙钟依赖。**决定：接口已预留，暂不实现**。
- **Standby 已 `start()` 后端**：Standby 阶段后端已连接——CephFS 是一个无 uuid 的普通
  会话（占一个 MDS 会话槽、不持 caps），Gluster/Lustre 同理。若运维希望 Standby 完全不连
  集群，可加 `[cluster] standby_connect = false`（把后端 `start()` 延到 Activating，代价是
  接管时间加上后端连接时间）。**决定：暂不实现，留作配置项后续项**。
- **多活 / 计划内迁移（09 §9.9）**：控制器按"每进程一个角色"实现。演进到"每导出一个
  角色"时，把 `ClusterStore` 的键空间加 `fsid` 前缀即可，接口已预留。**决定：不在本轮范围**。

## 2. 依赖目标环境、本机未验证

- **root VM 端到端（`scripts/accept_failover_vm.sh`）**：人工触发、不进 CI。`local` 轮可在
  任意 root VM 上跑（真实内核 `mount -o vers=4.1` + 两网关共享本地目录）；
  `gluster`/`cephfs`/`lustre` 三轮需真实集群，配置由 `LNFS_*` 环境变量提供，**本机无 root、
  无内核挂载、无集群，仅做过 `bash -n` 语法校验**。发布前应在目标环境三后端各跑一轮。
- **CephFS 会话回收（D2）的 MDS 侧行为**：`ceph_start_reclaim(uuid, RESET)` 立即驱逐旧
  会话、释放 caps/锁——本机用 `tests/cephapi_fake.cpp` 假实现覆盖了后端逻辑与状态层
  DELAY→成功路径，**真实 MDS 的驱逐时序未验证**。真实库仅核对了 `libcephfs.so.2`（20.2.0）
  导出这三个符号、签名与 `cephfs/libcephfs.h` 一致（`scripts/check_cephapi_abi.sh`）。
  发布前应跑 `scripts/accept_cephfs.sh` 或 VM 脚本的 cephfs 轮。
- **Gluster/Lustre 超时释放**：`gfapi_fake`/真实 OFD 锁模拟了"锁在 N ms 后释放"，验证了
  grace 内 DELAY 重试等到释放。真实 `network.ping-timeout` / `obd_timeout` 的实际时序需在
  目标环境确认（部署文档要求 Gluster `ping-timeout ≤ grace/2`）。

## 3. 本机开发环境跳过的门槛（发布前必须补跑）

- **格式 / 静态检查**：`scripts/format_check.sh`（clang-format）与 `scripts/tidy.sh`
  （clang-tidy，bugprone/performance/concurrency）在本机**未安装工具，每一步都跳过**。
  这是合并门槛之一（原 §10.2），**必须在装有 clang-format/clang-tidy 的 CI 或环境中补跑**
  后才算过门槛。
- 回归门槛在本机已跑：`ctest`（Release + ASAN）全过、`scripts/accept_m6_local.sh` 全过、
  `scripts/accept_failover_local.sh`（Release + ASAN）全过。

## 4. 指针

| 想找 | 去哪 |
|------|------|
| 实现总览（改动清单 + 代码位置） | [09 册](design/09-multi-gateway-failover.md) §9.10 |
| 设计动机与协议语义 | [09 册](design/09-multi-gateway-failover.md) |
| 部署拓扑、keepalived、运维 | [deployment.md §5](deployment.md) |
| 配置键与指标 | [08 册](design/08-config-observability.md) |
| 每步的详细实现记录（A1–E3） | git 历史中的 `docs/design/10-multi-gateway-failover-steps.md`（已撤下） |
