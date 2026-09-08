# 多网关多活（10 册）——收尾项

> 多活的实施步骤文档（原 `docs/design/12-multi-gateway-active-active-steps.md`）的 A1–E3 已全部
> 实现并合并（每步标 ✅ 2026-09-06），随之与 09 的步骤文档（原 10 册）同样撤下，每步实现记录见
> git 历史。本文件按其 §E3 的约定收口未闭环项，供后续处理。设计与实现见
> [../design/10-multi-gateway-active-active.md](../design/10-multi-gateway-active-active.md)（原 11 册）。

## 1. VM（root）端到端未跑

`scripts/accept_active_active_local.sh` 的四段（referral / migration / 猝死分散接管 / 单网关
退化）在本机 loopback + local 后端上 Release 与 ASAN 各跑一轮通过。原步骤文档 §E1 还提到给
`scripts/accept_failover_vm.sh` 加 `LNFS_MODE=active-active` 轮：真内核客户端
`mount -o vers=4.1` 走入口地址、`ls` 触发子挂载、`cat /proc/self/mountinfo` 断言子挂载目标、
`migrate` 中跑 fsx 不中断。本机无 root、无 `mount.nfs`，未实现；留待有 root 的 CI/VM。

## 2. fs_locations 只带主机名，不带端口

`fs_locations` / `fs_locations_info` 的 server 字段按 RFC 8881 §11.10 是主机名，不含端口
（`src/nfsv4/attrs.cpp` `location_server` = `address_host(node_address)`）。真实部署每网关是不同
IP、标准 2049 端口，无碍；但本机三实例全是 `127.0.0.1`，`fs_locations` 无法区分网关，
`accept_client v4moved` 因此按"存在 location + MOVED / LEASE_MOVED"验证 referral/migration，
属主由"哪个端口真正提供并接管"行为性地钉住。若将来要在 `fs_locations_info` 里带 universal
address（含端口），需扩展编码与该测试。

## 3. 迁移是异步接管，客户端/脚本需等待

`cluster migrate <fsid> <node>` 在源端写 owner + 释放围栏后立即返回（"migrate started"，角色
`activating`）；目标端在其下一 tick 才真正接管。其间目标对该 fsid 回 DELAY（activating）或
MOVED（尚未属主）。`scripts/cluster_roll.sh` 的 `migrate()` 因此轮询**目标**网关的 status 到
该导出 `role=active` 才算完成（早期版本只等源端转 remote，会与下一步竞争）。任何自动化编排都
应等目标 active，而非源端 remote。

## 4. 全仓库既有 clang-format/clang-tidy 漂移

沿用 09 实施时的处理：新增代码的 format/tidy 无新增问题，但全仓库既有漂移未在本特性里顺手改
（见 `docs/toto/multi-gateway-failover-followups.md` §3）。`src/core/boot_epoch.cpp` 的
`fscanf`（`bugprone-unchecked-string-to-number-conversion`）等既有告警不属于本册改动。
