# 11. 实现要点与设计建议（面向 lightnfs）

汇总前面各分册，回答三个问题：v4 值不值得做、做哪个小版本、按什么顺序做。v3 分册 [09-implementation-notes.md](../nfsv3/09-implementation-notes.md) 中句柄设计、DRC（对 4.0）、消息内存、安全清单等内容对 v4 同样适用，此处不重复。

## 11.1 工作量对比：v3 vs v4 服务器

| 模块 | v3 | v4.1 |
|------|----|----|
| RPC/XDR 框架 | 需要 | 相同（复用） |
| 协议入口 | 21 个独立过程 | COMPOUND 解释器 + ~40 个操作 |
| 辅助协议 | MOUNT（小）、NLM/NSM（可跳过） | 无（内建） |
| 状态管理 | 无（DRC 而已） | **clientid/会话/槽表/open/lock/租约/宽限期——工作量主体** |
| 属性 | 定长 fattr3 | bitmap 编解码 + ~30 个属性 |
| 回调 | 无（NLM GRANTED 可跳过） | 可宣告不用（不发委托）；lightnfs 已实现并用于读委托召回与 CB_NOTIFY_LOCK |
| 粗略量级 | 1× | 3–5× |

结论：**先做 v3 拿到能用的服务器，再在同一 RPC/VFS 抽象上加 v4.1**，是风险最低的路径。两版共用：传输层、XDR 基建、句柄方案、导出表、底层文件访问层（VFS 抽象）、鉴权/squash。v4 额外新建：COMPOUND 解释器、状态机、会话层、bitmap 属性层。

## 11.2 小版本选择：直接 4.1，跳过 4.0

理由汇总（散见各分册）：

- 4.0 的 owner seqid + OPEN_CONFIRM + RENEW + RELEASE_LOCKOWNER 是一套已被否定的重放/续租设计，实现它们纯属考古；
- 4.0 回调要反向连接，NAT 下委托残废；4.1 回传通道即使不用也不碍事；
- 4.1 槽表让应答缓存有了明确边界（4.0 的 DRC 义务含糊）；
- Linux/macOS/Windows 客户端全部支持 4.1，默认优先协商高版本；
- 4.2 = 4.1 骨架 + 可选甜点，宣告 minorversion=2 后逐操作 NOTSUPP 合法（见 08 分册 8.8）。

**minorversion=0 可以完全不支持**（MINOR_VERS_MISMATCH），客户端会自动降到 v3 或用 4.1。

## 11.3 状态表设计（承 04 分册 4.9，给出结构建议）

```
client_table:   clientid4 → { co_ownerid, verifier, lease_expiry,
                              sessions[], state_protect, 稳定存储记录标志 }
session_table:  sessionid4(16B) → { clientid, fore_slots[], back_slots[],
                                    chan_attrs, bound_conns[] }
slot:           { seqid, cached_reply?, in_flight }
state_table:    other(12B) → { type: OPEN|LOCK|DELEG|LAYOUT, seqid,
                               clientid, fh, ↓type 专有 }
open_state:     { open_owner, share_access, share_deny, lock_states[] }
lock_state:     { lock_owner, open_state↑, 区间锁列表 }
file_state:     fh → { opens[], locks[], deleg?, 冲突裁决入口 }
```

- `other` 编码建议：`{boot_epoch(4B), type(1B), counter(7B)}`——重启后收到旧 epoch 直接 STALE_STATEID，无需查表。
- 锁序：`client → file` 固定顺序；COMPOUND 入口先做会话/租约校验，操作内再做文件级锁。
- 租约用时间轮/最小堆做过期扫描；courtesy client（过期不立即回收，冲突才回收）可后加——lightnfs 已实现（`courtesy_multiplier`，默认 24×lease）。
- 稳定存储最小集：**客户端 co_ownerid 名单**（宽限期 reclaim 准入，见 04 分册 4.7）+ 服务器 boot epoch。每客户端一个文件或一个小 KV 即可。

## 11.4 COMPOUND 解释器要点

- 执行上下文：`{ CFH, SFH, 会话/槽引用, 凭证, minorversion }`；逐操作 dispatch，任何非 OK 即截断。
- **应答大小预算**：执行前无法精确知道 READ/READDIR 结果大小，需在操作执行时对照 ca_maxresponsesize 截断（READDIR）或拒绝（REP_TOO_BIG）；`cachethis=TRUE` 时还要对照 ca_maxresponsesize_cached（超了回 REP_TOO_BIG_TO_CACHE）。
- 操作实现顺序建议（按 Linux 客户端实际依赖）：
  1. 骨架：SEQUENCE, PUTROOTFH/PUTFH/GETFH, LOOKUP/LOOKUPP, GETATTR, ACCESS, READDIR, READLINK；EXCHANGE_ID/CREATE_SESSION/DESTROY_*/RECLAIM_COMPLETE/BIND_CONN_TO_SESSION —— 到这里**只读挂载可用**；
  2. IO：OPEN(CLAIM_NULL/CLAIM_FH/CLAIM_PREVIOUS), CLOSE, READ, WRITE, COMMIT, SETATTR, OPEN_DOWNGRADE, FREE_STATEID/TEST_STATEID；
  3. 目录写：CREATE, REMOVE, RENAME, LINK; VERIFY/NVERIFY, SECINFO/SECINFO_NO_NAME；
  4. 锁：LOCK/LOCKT/LOCKU（先做非阻塞语义：冲突即 DENIED；lightnfs 另在区间释放时向被拒者发 CB_NOTIFY_LOCK）；
  5. 可选甜点：委托（读）、4.2 的 SEEK/ALLOCATE/DEALLOCATE/COPY/CLONE/READ_PLUS——均已实现。
- 所有没实现的操作统一回 NFS4ERR_NOTSUPP，**但骨架清单里的不能缺**——客户端把它们当基础设施，缺一个就挂载失败且报错难懂。

## 11.5 属性层实现

- 用一张 `attr_id → {getter, setter?, 编码器}` 表驱动 GETATTR/SETATTR/READDIR/VERIFY 四个入口，避免四处硬编码。
- supported_attrs 诚实申报；GETATTR 对"支持但此对象取不到"的属性直接不置位返回（不报错）。
- change 属性来源优先级：i_version（statx 的 STATX_CHANGE_COOKIE）> ctime 纳秒 > 自维护计数（见 03 分册 3.4）。
- owner/owner_group：AUTH_SYS 场景直接输出十进制数字字符串、解析时接受数字，与 Linux 默认行为（nfs4_disable_idmapping）对齐——零 idmap 依赖。
- 时间属性 SETATTR 经由 time_access_set/time_modify_set（含 SET_TO_SERVER_TIME4/SET_TO_CLIENT_TIME4 判别），语义同 v3 sattr3。

## 11.6 与真实客户端联调备忘（v4 特有踩坑）

- Linux 客户端挂 v4 根路径写法是 `server:/`（相对伪根）或 `server:/export`；配合 fsid 边界正确性测试 `mount server:/ /mnt` + 遍历。
- 挂载卡在 EXCHANGE_ID/CREATE_SESSION：多半是 chan_attrs 回给客户端的值不合法（如 slots=0、maxrequestsize 太小）；给 16–64 槽、1MB±、ca_maxoperations ≥ 16 起步。
- 客户端每 lease/3 发一次纯 SEQUENCE 心跳——空 COMPOUND{SEQUENCE} 必须支持。
- OPEN 后没有立刻 CLOSE 是常态（页缓存持有），umount -f/客户端崩溃后靠租约回收——测试用例：kill 客户端 VM，观察 lease_time 后状态是否清干净。
- 服务器重启测试：重启后客户端应在宽限期内 reclaim 全部 open/lock 并继续跑 fsx 不出错；grace 期结束前提前放行（全员 RECLAIM_COMPLETE）是加分项。
- `nfsstat -c`/`mountstats` 看客户端操作分布；wireshark 对 v4 COMPOUND 解析完善，按 tag 过滤调试极方便；服务器端建议在 COMPOUND 入口打印 `tag + 操作序列 + 各操作 status` 的单行日志——v4 调试的第一生产力。
- pynfs（4.0/4.1 套件）是 v4 服务器的标准符合性测试；cthon04 四组用 `-o vers=4.1` 全跑；fsx/fsstress 过夜同 v3。

## 11.7 建议里程碑（接 v3 分册 9.9 的 M1–M4 之后；已全部交付）

M5–M8 全部实现并经 pynfs/cthon 验收，现行状态见 README"项目状态"；
M8 之外还落地了 READ_PLUS、cookieverf 语义化、RPC-over-TLS 与三个集群后端。

1. **M5 v4.1 只读**：COMPOUND 解释器 + 11.4 第 1 组操作 + 会话层 + bitmap 属性最小集 + 伪根。验收：`mount -o vers=4.1,ro` 后 `ls -lR`/`cat` 正常，pynfs 会话组通过。
2. **M6 v4.1 读写**：OPEN/CLOSE/READ/WRITE/COMMIT/SETATTR + 状态表 + 租约回收 + 宽限期（含客户端名单持久化）。验收：cthon basic/general、fsx 过夜、服务器重启恢复用例。
3. **M7 锁与安全**：LOCK 系（非阻塞）、SECINFO、squash 完备、导出级校验。验收：cthon lock 组。
4. **M8 甜点**：读委托 + CB 回传通道；4.2 的 SEEK/ALLOCATE/DEALLOCATE/COPY(同步同服)/CLONE；宣告 minorversion=2。

每个里程碑都保持 v3 路径回归通过（共享基建改动会互相影响）。
