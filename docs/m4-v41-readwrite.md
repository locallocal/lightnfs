# M6：NFSv4.1 读写 + 状态全量（阶段 4）

在阶段 3 的只读 v4.1 栈之上补齐写路径与完整的状态机：StateTable/FileStateIdx 全量、
OPEN 全 claim/create 语义与 share reservation 裁决、CLOSE/OPEN_DOWNGRADE、带 stateid 校验的
READ/WRITE/COMMIT/SETATTR、名字空间 op（CREATE/REMOVE/RENAME/LINK）、VERIFY/NVERIFY、
租约扫描 + courtesy + 回收链、完整 grace/reclaim 门禁、状态指标组与 `lightnfs-ctl` 状态表
dump/强制回收。v3 路径与后端接口不变（写路径落到与 v3 相同的后端调用）。

## 实现要点（设计依据 07 分册）

- **状态表**（07 §7.1）：`StateRec`（other = {boot_epoch(4B)|type(1B)|counter(7B)}，seqid/
  access/deny 为 relaxed 原子）经 `states_`（other → rec）与 `files_`（{fsid,oid} →
  opens 列表，share 冲突裁决入口）双索引；`ClientRec.states` 反向持有 other 集合供回收链
  遍历。四张表均 16 分片 AsyncMutex，**每个操作任一时刻只持一把分片锁**，持锁不做后端
  IO（后端 OpenPtr 的释放一律在最后一把锁释放之后执行）——结构性满足 ①session ②client
  ③objlock ④state 的锁序，并发矩阵单测仍为死锁自由的证明。
- **OPEN**（RFC 8881 §18.16）：claim NULL/FH/PREVIOUS；create UNCHECKED/GUARDED/EXCLUSIVE4/
  EXCLUSIVE4_1（verifier 存时间戳，attrset 回报 time_access/time_modify 位；4_1 的
  createattrs 去掉 size/time 后补 setattr）；UNCHECKED 命中既有文件仅应用 size；
  share reservation 裁决 `(access & deny') || (deny & access')`，冲突 → SHARE_DENIED；
  同 client 同 owner 同文件 → 并集 access/deny、seqid++（不新建记录）；open_owner4 的
  clientid 按 4.1 规定忽略（会话即身份）；打开前对非新建文件做 POSIX 权限检查
  （ACCESS）；目录 ISDIR、符号链接 SYMLINK、设备/管道 WRONG_TYPE；写类 OPEN 于只读导出
  ROFS；委托类 claim NOTSUPP（M8）。
- **IO 路径 stateid 校验顺序**（07 §6.1 / 04 分册 §4.3）：特殊 stateid（全 0 匿名：受
  share deny 约束 → LOCKED；全 1 READ 旁路；grace 内匿名写 → GRACE）→ epoch 不符
  STALE_STATEID（不查表）→ 查表 BAD_STATEID → client 已回收 EXPIRED → client/对象不符
  BAD_STATEID → seqid（0 不校验；旧 OLD_STATEID；超前 BAD_STATEID）→ OPENMODE。
  CLOSE/OPEN_DOWNGRADE 要求精确 seqid（0 → BAD_STATEID）；DOWNGRADE 只允许收窄且 access
  非空（否则 INVAL）。
- **WRITE/COMMIT**：verifier = boot epoch（与 v3 共用一个"重启信号"）；stable 三档原样交给
  后端；SETATTR 带 size 视为写（走 stateid 写校验）；SETATTR4res 恒带 attrsset；fattr4
  可设置集 = size/mode/owner/owner_group/time_access_set/time_modify_set，owner 仅接受数字
  串（非数字 → BADOWNER，引导 Linux 客户端走 nfs4_disable_idmapping 回退），只读属性
  INVAL、ACL 等 ATTRNOTSUPP；`suppattr_exclcreat` 申报同一集合。
- **名字空间 op**：CREATE（DIR/LNK/BLK/CHR/SOCK/FIFO；REG → BADTYPE；按后端 caps 回
  NOTSUPP）、REMOVE（按对象类型选 unlink/rmdir）、RENAME（SFH=源目录，跨导出 XDEV，双目录
  按 ObjId 定序加锁）、LINK（SFH=源文件，目录 ISDIR）；change_info4 在目录排他锁下采样，
  atomic=TRUE；伪根一律 ROFS。VERIFY/NVERIFY 以同掩码编码后字节比较（rdattr_error INVAL、
  不支持位 ATTRNOTSUPP）。
- **租约与 courtesy**（07 §7.4）：`run_lease_scanner` 每秒一轮：到期且持有会话/状态的
  client 进入 courtesy（状态保留，指标 `lease_expirations`）；未确认或空记录直接撤销；
  courtesy 超过 `courtesy_multiplier × lease`（默认 24×）→ 超时回收；OPEN 裁决遇到
  courtesy 方的冲突 → 先回收对方再重新裁决（放行冲突请求）；courtesy client 在回收前
  重新 SEQUENCE → 续约复活（与 Linux nfsd 同策略）。回收链：ClientRec 标记 expired →
  逐 StateRec 摘表 → files_ 反引用摘除 → 会话销毁（唤醒 in-flight 等待者）→ 稳定名单
  删除 → 最后统一释放后端 OpenPtr。客户端重启（新 verifier 经 CREATE_SESSION 确认）走同
  一条链释放旧化身状态（04 分册 §4.8）。
- **grace/reclaim**（07 §7.5）：CLAIM_PREVIOUS 仅在 grace 内、且 co_ownerid 在
  `state_dir/clients/` 名单内、且尚未 RECLAIM_COMPLETE（否则 NO_GRACE/RECLAIM_BAD/
  NO_GRACE）；grace 内普通建状态 OPEN → GRACE，匿名 WRITE → GRACE，读全部放行；名单内
  client 全部 RECLAIM_COMPLETE → 提前出 grace。服务器不持久化 open 状态（红线：稳定存储仅
  boot_epoch/hmac.key/clients 名单），reclaim 即按客户端声明重建。
- **观测**（07 §7.8）：`lightnfs_v4_{clients,sessions,opens,files_with_state,
  courtesy_clients,in_grace,grace_remaining_seconds,seq_*,lease_expirations_total,
  reclaims_total{reason=conflict|timeout|forced},share_denied_total,open_merges_total}`；
  `lightnfs-ctl state` 输出指标行 + client/session/open 三表 dump；
  `lightnfs-ctl expire-client <clientid>` 强制回收。配置新增 `[protocol] lease = "90s"`、
  `courtesy_multiplier = 24`（GETATTR lease_time 随配置申报）。

## 验收（开发计划 §6.3）

**回环半（无 root）**：`scripts/accept_m4_local.sh` 一键——Release/ASAN 两配置单测；
`accept_client v4rw`（OPEN(CREATE)→64KiB 分块 UNSTABLE WRITE→COMMIT verifier 一致→
逐字节读回与后端文件比对→SETATTR 截断→双客户端 share deny（SHARE_DENIED/LOCKED/
OPENMODE/OLD_STATEID）→OPEN_DOWNGRADE 放行→CREATE 目录/RENAME/LINK/REMOVE 并在后端树
逐项验证）；`v4walk`/v3 `walk` 回归；`v4reclaim`（带打开状态 kill -9 重启→同 owner 重建
会话→旧 stateid STALE_STATEID→普通 OPEN GRACE→CLAIM_PREVIOUS 成功、数据无损→继续写→
RECLAIM_COMPLETE 提前出 grace→事后 reclaim NO_GRACE）；`v4courtesy`（持有者断连：租约内
SHARE_DENIED，租约后冲突回收放行，第二个持有者由超时回收，`lightnfs-ctl state` 计数校验）；
ctl 状态 dump + `expire-client` 强制回收；pynfs 4.1 open/rename/verify/courteous 组 +
阶段 3 会话组；ASAN 泄漏检查。

**真实 mount 半（root VM，CI `m4-acceptance`）**：`scripts/accept_m4_vm.sh`——
`mount -o vers=4.1` 读写：cthon04 basic/general/special、fsx（默认 5 万 ops，过夜
`FSX_OPS=2000000`）、带打开文件 kill -9 重启（内核客户端在 grace 内 reclaim，崩溃前后写入
全部落盘）、租约回收两路径、v3/v4 双挂载并发写互见。

## 已知边界

- 无特权回退句柄模式（`handles = "auto"` 且无 CAP_DAC_READ_SEARCH）在**无 STATX_BTIME**
  的文件系统（如 tmpfs）上句柄不跨重启稳定——重启后客户端拿旧 fh 得 STALE，只能重新
  LOOKUP；`v4reclaim` 对此打印提示并按名重解析后继续验证状态语义。生产部署用内核句柄
  或 btime 文件系统即无此限制（06 分册）。
- v3/v4 混布：v3 无 share/锁语义，v4 的 deny 不约束 v3 写（文档明示，VM 脚本验证"互见"
  而非互斥）。
- 字节锁（LOCK/LOCKT/LOCKU）、委托、SECINFO(带名)、ACL 属于阶段 5/6；FREE_STATEID 对仍在
  用的 open stateid 回 LOCKS_HELD。
- pynfs 结果（open/rename/currentstateid/verify/courteous + 阶段 3 全部组，184 用例）：
  162 通过 / 22 失败，失败全部为预期排除（`scripts/pynfs_m4_expected.txt`）：LOCK 依赖
  COUR1/2、CSID2/4（阶段 5）；委托/回传依赖 DELEG1/3/5/6/7、DSESS9002/9003（M8）；CSID7
  为 pynfs 自身 NameError；LKPP1b/c、PUTFH1b/c、RNM1b/c、RNM2b/c、RNM3b/c 需 root 创建块/
  字符设备。命令行排除 EID9（租约到期：以 `v4courtesy`/单测覆盖，避免 90s 睡眠）与
  EID50（SP4_SSV）。
