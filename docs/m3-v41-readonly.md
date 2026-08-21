# M5：NFSv4.1 只读（阶段 3）

在 v3 读写服务器之上加入 v4.1 栈：COMPOUND 解释器、会话层（EXCHANGE_ID/CREATE_SESSION/
SEQUENCE 槽表精确一次语义）、bitmap 属性层、伪文件系统命名空间，以及只读操作集。
minorversion=0 恒拒（MINOR_VERS_MISMATCH）；v3 路径完全不受影响（共享 core/backend）。

## 实现要点

- **COMPOUND**：顺序执行遇错即停；CFH/SFH 双句柄寄存器；应答预算——READ/READDIR 按
  剩余预算预裁剪（保零拷贝），其余 op 经暂存缓冲，超限替换为 REP_TOO_BIG（cachethis
  时为 REP_TOO_BIG_TO_CACHE）；sessionless op（EXCHANGE_ID 等）必须独占 COMPOUND，
  会话内 BIND_CONN 拒绝、DESTROY_SESSION 只能是末位操作。
- **会话/槽**（07 分册 §7.3）：每槽 (seq, in-flight, cached_reply) 三分支：新请求执行、
  重放原字节应答（未缓存 → RETRY_UNCACHED_REP）、乱序 → SEQ_MISORDERED；in-flight
  重复到达挂 AsyncCondVar 等原执行完成。槽数/应答尺寸在 CREATE_SESSION 钳制
  （≤32 槽、缓存应答 ≤8KiB）；cachethis 请求整体按缓存预算编码，因此恒可缓存。
  SEQUENCE 兼租约续期：ClientRec.lease_expiry 原子 store，无锁。
- **EXCHANGE_ID 记录语义**（RFC 8881 §18.35 全表）：每 owner 保 confirmed/unconfirmed
  双记录；同 verifier 同 principal → 返回既有（CONFIRMED_R）；verifier 变化（客户端
  重启）→ 新 unconfirmed，旧状态存活至 CREATE_SESSION 确认；principal 冲突有状态 →
  CLID_INUSE、无状态 → case-3 替换；UPD_CONFIRMED_REC_A → NOENT/NOT_SAME/PERM 判定。
  CREATE_SESSION 以 clientid+sequence 做自身重放保护，确认时销毁旧化身的会话。
- **伪文件系统**：fsid=0 保留；按导出路径合成只读前缀树；`mount server:/` 从伪根浏览，
  LOOKUP 跨入导出时执行 CIDR 校验；READDIR 中导出交界子项直接呈现导出根属性
  （fsid 变化 + mounted_on_fileid = 伪节点 id），Linux 客户端据此识别文件系统边界。
- **属性层**：单表驱动，13 个 REQUIRED + mode/owner(数字串,免 idmap)/numlinks/times/
  maxread/maxwrite/space_*/files_*/mounted_on_fileid 等实际消费集；supported_attrs
  诚实申报。
- **最小 open-state（计划外前移）**：Linux 客户端 `cat` 必经 OPEN→READ→CLOSE，因此只读
  里程碑即实现 CLAIM_NULL/CLAIM_FH 只读 OPEN、CLOSE、stateid 表（other =
  {epoch|type|counter}，跨重启 STALE_STATEID 零成本判定）、READ 的 stateid↔对象校验、
  FREE/TEST_STATEID。写类 OPEN → ROFS；CLAIM_PREVIOUS 按 grace 门禁回 NO_GRACE/
  RECLAIM_BAD（完整 reclaim 属阶段 4）。
- **grace 骨架**：启动读 `state_dir/clients/` 名单进入 grace（时长=lease）；读路径放行；
  RECLAIM_COMPLETE 全到齐提前出 grace；one_fs=TRUE 为 fs 级完成不置全局旗。

## 验收（开发计划 §5.4）

**回环半（无 root）**：`scripts/accept_m3_local.sh` 一键——两配置单测；
`accept_client v4walk`（用户态 4.1 客户端：EXCHANGE_ID/CREATE_SESSION/RECLAIM_COMPLETE、
伪根路径穿越、递归 READDIR 逐字节校验、OPEN/READ/CLOSE、槽重放字节级一致、
minorversion=0 拒绝）；v3 `walk` + v4 `v4walk` 双读一致；pynfs 4.1 会话组
（`fetch_pynfs.sh` 自动处理 Python≥3.13 的 xdrlib 兼容）；ASAN 泄漏检查。

**真实 mount 半（root VM，CI `m3-acceptance`）**：`scripts/accept_m3_vm.sh`——
`mount -o vers=4.1,ro` 后 ls -lR/cat/md5sum -c；v3 与 v4.1 同时挂载并发读 +
`diff -r` + 双 md5 一致（4.8 锚点）；只读强制。

pynfs 已知排除：EID9（租约到期回收，阶段 4 交付）、EID50（SP4_SSV，设计不支持）、
需要 OPEN(CREATE) 写支持的用例（阶段 4）。
