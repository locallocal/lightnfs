# 共享导出清单（11 册）——收尾项

> 清单的实施步骤文档（原 `docs/design/12-shared-export-catalog-steps.md`）的 A1–E2 已全部实现并
> 合并，随之与 09 / 10 的步骤文档同样撤下，每步的改动点与实现注见 git 历史。本文件按其 §E2 的
> 约定收口未闭环项，供后续处理。设计与实现见
> [../design/11-shared-export-catalog.md](../design/11-shared-export-catalog.md)，运维视角见
> [../deployment.md](../deployment.md) §6.1，配置 / 热重载口径 / 指标 / ctl 见
> [../design/08-config-observability.md](../design/08-config-observability.md)。

## 1. 导出退出清单后，`fs/<fsid>/*` 共享状态无人回收

E1 在这里抓到过一个真 bug 并修掉了**消费侧**（`server/cluster_controller.cpp` 的
`migration_target`：owner 记录指向的节点若已不在该导出的 `nodes` 里，就不算迁移目标，退回按
节点顺位的普通规则；回归测试
`FsClusterController.StaleOwnerRecordDoesNotResurrectAnUnlistedNode`）。**生产侧仍未闭环**：
`fs/<fsid>/owner`（连同 `fs/<fsid>/epoch`、`fs/<fsid>/clients/`）比导出本身活得久——一个导出被
`export remove` 移出清单时，没有任何一方清理它在共享目录里的这几个键。每个曾经存在过的 fsid
在 `shared_dir/fs/` 下留一份，只增不减。

今天没有正确性后果（消费侧已加护栏，陈旧 owner 记录不会让不在名单里的网关抢回导出），代价是
共享目录的条目无限增长，以及 `cluster status` 的属主视图在删除后的短时间内仍可能显示一个不会
接手的网关。

要决定的是"谁清、何时清"：

- **最后一个排空该 fsid 的网关顺手删**——简单，但"排空"与"从清单里消失"是两件事（`disabled`
  的导出也排空，但 fsid 还在清单里，将来可能 `--disabled=false` 回来，届时 `epoch` 应当延续）；
- **按 fsid 不在任何历史版本里判定**——语义干净（历史保留 32 版，超出窗口的 fsid 才算真正退役），
  但要有一个"谁来跑"的约定，而 lightnfs 不选主；
- 或者干脆**只做 `lightnfs-ctl catalog gc --shared-dir <dir>`**（离线 / 手动），把决定权留给运维，
  与"已下线节点的 `exports.<node>` 需运维手动删除"（08 §8.1）同一口径。

第三条最贴合现有取舍，但需要先钉死"哪些 fsid 可以删"的判据。`catalog.<node>` 也有同样的问题：
下线一台网关后它的登记文件留在共享目录里，`cluster catalog status` 会一直列出一台
`alive=no` 的网关。

## 2. fsid 复用仍靠写入侧规则，句柄里没有导出代际

句柄 HMAC 覆盖 fsid + ObjId，不含"导出代际"（11 §11.8）。删掉 fsid 3 之后再以别的 path / backend
新增 fsid 3，旧客户端缓存的句柄可能解码到新后端的对象且 HMAC 仍然通过。现在的防线全在**写入
侧**：历史 32 版内出现过、且 `path` / `backend` / 集群键不同的 fsid 拒绝复用，`--force` 可越过
（文案里明示风险）。

两个缺口：`--force` 之后没有任何运行期保护；历史窗口（32 版）之外的复用不再被识别。彻底解法是
把导出代际折进 HMAC 派生（`core/file_handle.cpp` 的 `key.bind`），代价是句柄不再跨"删了再加"
稳定——本来也不该稳定，但要一并处理 v3 的 ESTALE 口径与升级期的句柄兼容。留作演进。

## 3. 主备（failover）形态的清单验收只有单元测试

清单在 `mode = "failover"` 下是支持的（`ClusterController` 的围栏线程做同一件轮询 + 投递，
standby 也应用），也有单元测试覆盖，但**跨进程验收只在多活脚本里跑过**：
`scripts/accept_active_active_local.sh` 把导出模式做成外层循环
（`LNFS_EXPORTS="local catalog"`），`scripts/accept_failover_local.sh` 没有对应的 catalog 轮。
补的话是同样的做法：本地文件去掉 `[[export]]`、离线 `catalog import --from-local` 引导 v1、
在线 `export add` 后确认主备两台都换版、`rollback` 回 v1；主备下没有 referral 可断言，验收点是
"活动网关在线增删导出、standby 的表在激活后就是最新集"。

## 4. 真内核客户端（root / VM）未跑

与 10 册的同名收尾项（`multi-gateway-active-active-followups.md` §1）一样：`v4catalog` 段用的是
`lnfs_accept_client`（本仓库自己的 v4.1 客户端），在本机 loopback + local 后端上 Release 与 ASAN
各一轮通过。真 Linux 内核客户端下的"在线加导出 → `ls /mnt` 看到新目录 → 进去自动建子挂载 →
`export set --disabled` 后旧挂载点回 ESTALE"没有跑过：本机无 root、无 `mount.nfs`。留待有 root
的 CI / VM，与 10 册那一项一起做。

## 5. 退休条目卡住只有日志，没有指标

`ExportTable` 的退休队列里如果有条目超过 `10 × [protocol] lease`（默认 90s，即 15 分钟）仍被
引用（某个在途请求或控制器还持着 `shared_ptr`），`CatalogApplier::retire_exports` 每隔一个周期打一条 WARN
（"N removed export(s) still referenced after S s"），但**没有对应的指标**——运维只能从日志发现
后端一直没 `stop()`。加一个 `lightnfs_cluster_catalog_retire_pending` 量表（当前待退休条目数）
即可，与既有的 `lightnfs_cluster_catalog_*` 同组注册。

## 6. 全仓库既有 clang-format / clang-tidy 漂移

沿用 09 / 10 实施时的处理：本册新增代码的 format / tidy 无新增问题，全仓库既有漂移未在本特性里
顺手改（见 [multi-gateway-failover-followups.md](multi-gateway-failover-followups.md) §3 与
[multi-gateway-active-active-followups.md](multi-gateway-active-active-followups.md) §4）。
