# NFS 协议一致性——收尾项

> 2026-09-15 的协议一致性审计（原 `docs/design/followups/protocol-conformance-followups.md`）对照 RFC 1813 /
> 8881 / 7862 / 5531 与 [../../reference/](../../reference/) 的调研结论，逐条列了 A1–A5、
> B1–B8、C1–C5 共 18 个条目。**全部处理完毕**（下表逐条对应提交），审计正文随之撤下——每条的
> 实际改法、与原提案的差异、以及反向验证（把改动回退后失败的断言数）都写在对应提交的信息里，
> 见 git 历史。本文件按与三册集群收尾文档相同的约定，只留**未闭环项**。
>
> 代码与测试里的 `protocol-conformance-followups.md A3` 这类引用指的就是下表的条目号。
>
> 设计见 [../04-protocol-engines.md](../04-protocol-engines.md)、
> [../07-state-management.md](../07-state-management.md)、
> [../08-config-observability.md](../08-config-observability.md)，运维视角见
> [../../guide/deployment.md](../../guide/deployment.md)。

## 0. 已收口条目索引

「分级」沿用审计的口径：**A = 会产生错误码错误或畸形回复**；**B = 语义偏差 / 安全边界偏差**；
**C = 加固项与已知取舍**。

| # | 级 | 收口内容 | 提交 |
|---|----|----------|------|
| A1 | A | GETATTR 不再为只写属性 `time_access_set`/`time_modify_set` 置位而不带值 | `3ea690c` |
| A2 | A | `mounted_on_fileid`(55) 编码到 `fs_locations_info`(67) 之前；`encode_fattr` 里加了常态的属性升序断言 | `6ef1fe3` |
| A3 | A | v3 超长名字/路径回 NFS3ERR_NAMETOOLONG 而非 RPC GARBAGE_ARGS（连带 C5） | `c20fd45` |
| A4 | A | v4 超长分量回 NFS4ERR_NAMETOOLONG 而非 BADNAME/BADXDR | `3eb22bd` |
| A5 | A | FREE_STATEID 校验 stateid 属主（外来 stateid 与不存在的 stateid 不可区分） | `6cf2cf2` |
| B1 | B | sessionid 末 4 字节改为每会话 `getrandom` 随机量；同条目的另两半复核后判定不做（见 §6） | `1f8db30` |
| B2 | B | `squash = root` 同时压 gid 0 与附加组 0（组 0 是**替换**成 anon，与 knfsd 一致） | `ed30d99` |
| B3 | B | 新增 `[[export]] secure_ports`，默认 `true`，拒绝非特权源端口 | `5a666e5` |
| B4 | B | DRC 键去掉源端口，跨重连的重传不再 miss | `1766fd8` |
| B5 | B | `fh_expire_type` 跟随后端的 `stable_handles`；剩下的半条见 §1 | `206a94c` |
| B6 | B | v3 FSINFO 宣告 FSF3_CANSETTIME | `d0832c8` |
| B7 | B | v3 的写/删/改名召回 v4 读委托，命中回 NFS3ERR_JUKEBOX（走语义正确的方案，不是降级方案） | `fe0b944` |
| B8 | B | 七条较小偏差：CLOSE/OPEN_DOWNGRADE/LOCKU 校验 CFH 与 stateid 同文件、RECLAIM_COMPLETE 句柄检查（per-fs 跟踪见 §3）、属性位图高位 word 读放宽写报错、`share_access` 未定义位、errmap READDIR 收 NOT_SAME、MOUNT DUMP 的 README 口径、MNT 非目录回 NOTDIR | `57ba669` |
| C1 | C | `RecordStream::kMaxFragments = 1024`（恒开）+ `[server] conn_idle_timeout` 空闲连接清扫（默认 0，理由见 §6） | `2900501` |
| C2 | C | 新增 `[[export]] strict_readdir_cookies`（默认 `true`）+ 两个 cookie 指标 | `2fe31e5` |
| C3 | C | AUTH_SYS 校验 verifier（→ AUTH_BADVERF）；AUTH_NONE 走导出的 anon 身份（显式 `Cred::anonymous` 位） | `d38d6a3` |
| C4 | C | `PseudoFs` 的 change 属性注释修正，`boot_epoch` 改名 `change_base`（无行为变更） | `8465ee7` |
| C5 | C | v3 LOOKUP 的非法分量回 NOENT 而非 GARBAGE_ARGS | 随 `c20fd45` |

---

## 1. volatile 句柄失效回 STALE，而不是 FHEXPIRED

B5 让 `fh_expire_type` 变诚实了（local 后端的路径回退模式下宣告 FH4_VOLATILE_ANY），但句柄真
失效时回的仍是 **NFS4ERR_STALE**，而 RFC 8881 §4.2.3 对 volatile 句柄期望 **NFS4ERR_FHEXPIRED**。
`NFS4ERR_FHEXPIRED`(10014) 在 `nfsv4/nfs4_types.hpp:151` 有定义，但全仓没有任何地方返回它。

要改得让 `core/errmap.cpp` 知道「当前这个导出的句柄是不是 volatile」——今天的映射只看 errno，
拿不到导出。那是比 B5 大一档的改动（映射层需要携带导出上下文）。

**现状不比修复前差**：Linux 客户端对 STALE 本来就有恢复路径，而修复前是**既谎报 persistent
又回 STALE**，现在至少宣告是诚实的。所以这条是一致性收尾，不是故障。

## 2. `INVALID_UID` / `INVALID_GID` 未映射到 anon

knfsd 在**任何** squash 模式下都会把 `(uint32_t)-1`（`INVALID_UID`/`INVALID_GID`）映射成
anon。lightnfs 原样透传：`core/config.cpp` 的 `squash_cred()` 里没有对 0xFFFFFFFF 的判断，
`squash = none` 下 0xFFFFFFFF 会交到后端（`local` 的 `setfsuid` 会失败，`kNativeAccess` 的后端
由存储侧判定）。

这会**改变 `squash = none` 的行为**，所以当初没有放进 B2 一起改——B2 只动了 `root` 这一支。
落地时该和 B2 一样走完整链路：`squash_cred()` + 用例 + `config/lightnfs.toml.example` 与
[deployment.md](../../guide/deployment.md) §1 的口径。

## 3. RECLAIM_COMPLETE 的 per-fs 完成状态不跟踪

B8 按 RFC 8881 §18.51.3 补上了「没有当前句柄就回 NOFILEHANDLE」，但 `rca_one_fs = TRUE` 仍然
只记到**每客户端**的完成标记上（`state/state_mgr.cpp:325` 的注释、`nfsv4/engine.cpp:2877-2891`）。
Linux 客户端发的是 FALSE，所以这一支在真实部署里不可见。

真要用上它，得跟**多活的 per-fsid grace** 一起设计——per-fs 的「回收完成」只有在 grace 本身
就是 per-fsid 的时候才有意义。因此这条归到那条线上，见
[multi-gateway-active-active-followups.md](multi-gateway-active-active-followups.md)。

## 4. `scripts/` 里 29 处 `producer | grep -q PATTERN`

`set -o pipefail` 下，`grep -q` 一命中就退出并关掉管道，生产者拿到 SIGPIPE
（`lightnfs-ctl` → 141）或写错误（`curl` → 23），pipefail 把它变成整步失败——**即使模式是匹配
到的**。B3 那轮在 `accept_m2_local.sh` 的 admin-tools 步骤上实测到过（在**干净 HEAD 上同样
失败**，所以不是那轮引入的），只修了挡路的那一处，改成「先落盘再 grep」。

`grep -rn '| *grep -q' scripts/*.sh` 今天还有 29 处。多数在 `[[ ]]` 条件里、不受 pipefail
影响，所以**不能一把 sed**，需要逐处看是否在 `set -o pipefail` 生效的路径上、生产者是否会因
提前关闭管道而非零退出。

## 5. 外部测试套与真内核客户端从未跑过

审计的 18 条全部靠仓内单元测试与 `scripts/accept_m2_local.sh` 验证。两块缺口：

- **pynfs 4.1 / cthon 从未跑过**（本机环境限制见
  [../../testing/](../../testing/) 与三册集群收尾文档的同名条目）。A3 一直没被发现正是因为
  **Linux 客户端按 PATHCONF 的 `name_max` 在本地就把超长名字挡了**——挂载测不到线上行为，只有
  裸 RPC 或外部测试套能测到。这类「客户端替服务器兜住了错误」的缺陷，仓内测试同样看不见。
- **A2 的 referral 端到端**没有覆盖：`fs_locations_info` 只在 `[cluster] mode = active-active`
  下才编码。目前守住它的是 `Nfs4.EncodeFattrFullSetRoundTrips`——直接对编码器断言「属性号严格
  升序 + attrlist 长度等于各属性值长度之和」。这个不变式比逐条用例更值钱（一次守住整类问题），
  但它不能替代一次真实的 referral 探测。

## 6. 复核后判定不做（记录在案，非遗留）

审计里有三项，复核后结论是**不做**，理由记在此处以免被当成漏掉的活：

- **给 stateid 加随机量**（B1 的一半）。knfsd 的 stateid 同样可推
  （`{si_generation, {so_clid, so_id}}`，`so_id` 是每客户端计数器），它靠属主检查防护——那就是
  **A5**，已经修了。要让随机量有意义就得**每 stateid 一份**：共享一个进程级掩码是可逆的
  （攻击者拿自己的两个 stateid 就能解出掩码，因为 counter 近乎连续），每 stateid 独立随机又
  丢掉唯一性保证，只能改成「counter 的带密钥置换」。参考实现都没有这个复杂度，A5 之后收益很薄。
- **SEQUENCE 严格连接绑定**（B1 的另一半）。`sequence_begin` 无条件隐式绑定连接，与 knfsd 在
  **SP4_NONE** 下的 `nfsd4_sequence_check_conn` 一致；严格绑定属于 SP4_MACH_CRED 的范畴。
  原审计把它记成「偏离」是判定过重。
- **`conn_idle_timeout` 默认关**（C1 的 (b) 半）。真正的 DoS 面由恒开的分片上限堵住；回收连接
  本身无害（v3 无状态，v4 会话不随连接消失），但**v4 回传通道就搭在某条连接上**——回收一个持有
  读委托的空闲客户端，CB_RECALL 就得等它回来。这个代价取决于部署形态，不该由服务器替运维决定。

真正敌对的网络仍然是 [09-security](../../reference/nfsv4/09-security.md) §9.5 说的
SP4_MACH_CRED / RPCSEC_GSS / TLS 问题，不是靠这几项能收的。
