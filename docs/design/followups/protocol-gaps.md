# 协议一致性缺口与语义风险审计（2026-09-15）

> **范围**：把仓库里已实现的北向协议面整体对照 RFC 1813 / RFC 8881 / RFC 7862 / RFC 5531
> 与本仓库的 [协议调研](../../reference/) 结论读了一遍，收口**协议上的遗漏**与**语义存在
> 问题或风险**的点。逐文件读过：`src/nfsv3/`、`src/nfsv4/`、`src/mountd/`、`src/rpc/`、
> `src/transport/record_stream.cpp`、`src/state/state_mgr.cpp`、`src/core/{names,errmap,
> mutate,readdir,pseudofs,file_handle,fs_props,config}`。
>
> **不在范围**：四个后端内部的存储语义（05/06 册各自的契约）、集群三册的多网关时序（见同
> 目录另三份 followups）、性能与运行时。
>
> **证据口径**：A1、A2 两条用临时探针测例在 `build/lnfs_tests` 上实测过（探针已回滚，不留
> 在仓库里），实测输出抄在条目里；其余条目给出文件行号，结论只从代码本身得出。
>
> **分级**：**A = 会产生错误码错误或畸形回复**（客户端可见故障，应修）；
> **B = 语义偏差 / 安全边界偏差**（行为与 RFC 或与 knfsd 惯例不一致）；
> **C = 加固项与已知取舍**（记录在案，按需）。

## 0. 一页结论

| # | 级 | 位置 | 问题 | 客户端可见后果 |
|---|----|------|------|----------------|
| A1 | A | `nfsv4/attrs.cpp:57,102` | GETATTR 请求只写属性 `time_access_set`/`time_modify_set` 时，attrmask 置位但 attrlist 不带值 | fattr4 自相矛盾，客户端 XDR 解析失败（Linux → EIO）**——已修复** |
| A2 | A | `nfsv4/attrs.cpp:196` | `fs_locations_info`(67) 编码在 `mounted_on_fileid`(55) 之前，违反属性升序 | referral 探测时属性错位解析（仅 active-active）**——已修复** |
| A3 | A | `nfsv3/nfs3_types.cpp:20,200`、`mountd/mount3.cpp:114` | v3 名字 >255B、symlink 目标 >1024B、MNT 路径 >1024B 一律 RPC GARBAGE_ARGS | 应为 NFS3ERR_NAMETOOLONG；`ln -s <1KB+ 目标>` 在 v3 上直接 EIO**——已修复（连带 C5）** |
| A4 | A | `nfsv4/engine.cpp:789` 等 | v4 名字 256B → BADNAME，≥257B → BADXDR | 应为 NFS4ERR_NAMETOOLONG；`errmap` 白名单里的 NAMETOOLONG 无路径可达**——已修复** |
| A5 | A | `state/state_mgr.cpp:1595`、`nfsv4/engine.cpp:2765` | FREE_STATEID 不校验 stateid 属主 | 叠加 B1 的可推 stateid → 可释放别人的 lock stateid**——已修复** |
| B1 | B | `state/state_mgr.cpp:549` | sessionid 全无随机分量，网段内可枚举并冒用别人的会话**——已修复**；同条目下的 stateid 可推（A5 覆盖）与 SEQUENCE 隐式绑定连接（与 knfsd 在 SP4_NONE 下一致）**原判定过重，已更正** | 见 B1 正文 |
| B2 | B | `core/config.cpp:1089` | `squash = root` 只压 uid，不压 gid 0 与附加组 0 | 组 root 可写的文件仍可被写**——已修复** |
| B3 | B | 全路径缺失 | 无特权源端口（"secure"）检查 | 受信主机上的**普通用户**即可声称任意 uid**——已修复（新增 `secure_ports`，默认 true）** |
| B4 | B | `rpc/drc.hpp:39` | DRC 键含源端口 | 跨重连的重传 miss → 非幂等过程重放（REMOVE 回 NOENT 等）**——已修复** |
| B5 | B | `nfsv4/attrs.cpp:133` | `fh_expire_type` 恒为 FH4_PERSISTENT，无视 `kStableHandles` | fallback 句柄模式下向客户端谎报句柄永久有效 |
| B6 | B | `nfsv3/engine.cpp:584` | v3 FSINFO 不宣告 FSF3_CANSETTIME | 看 properties 的客户端不用 SET_TO_CLIENT_TIME |
| B7 | B | `nfsv3/engine.hpp`（无 StateMgr） | v3 的写/删/改名不召回 v4 读委托 | v4 客户端**无限期**读到缓存旧内容 |
| B8 | B | 见正文清单 | 7 条较小的一致性偏差 | 各条见正文 |
| C1–C5 | C | 见正文 | 加固项与注释过期 | — |

---

## A. 会产生错误码错误或畸形回复

### A1 GETATTR 拿只写属性 → 畸形 fattr4（已修复）

`supported_attrs()` 把 `time_access_set`(48) 与 `time_modify_set`(54) 也放进了集合
（`src/nfsv4/attrs.cpp:57-58`）。这本身没错——它们对 SETATTR 确实支持。问题是
`encode_fattr()`（`:102-227`）**没有这两个位的编码分支**，而 `actual = wanted ∩ supported`
照样把位置上了：

```
实测（探针 GETATTR{size(4), time_access_set(48), time_modify_set(54)}）：
  PROBE compound status=0
  PROBE mask words=2 w0=00000010 w1=00410000 bit48=1 bit54=1 bit4=1
  PROBE attrlist len=8      ← 只有 size 的 8 字节
```

attrmask 宣告了三个属性，attrlist 只装了一个。客户端按 mask 逐个解码会读穿 attrlist，
Linux 客户端在 `decode_getfattr` 阶段报 buffer 越界并把请求变成 EIO。同一缺陷也经
`op_readdir` 的 `emit()`（`src/nfsv4/engine.cpp:1523`，直接把客户端的 `*wanted` 传给
`encode_fattr`）和 `op_verify`（`:2372`，`sup.test(bit)` 对 48/54 为真，于是放行）触发。

RFC 8881 §18.7.3 的口径是：GETATTR 请求只写属性应答 **NFS4ERR_INVAL**。

**修法**：`op_getattr` / `op_readdir` / `op_verify` 入口判断 `wanted` 是否命中
{48, 54}，命中回 INVAL；同时在 `encode_fattr` 里把这两位从 `actual` 中剔除做兜底
（防御纵深，也让未来新增只写属性不再复发）。`supported_attrs()` 不要动。

**已修复**（本轮）：
- `nfsv4/attrs.cpp` 新增 `write_only_attrs()` / `wants_write_only()`，`encode_fattr` 的
  `actual` 掩码里再 `& ~write_only`，从编码器层保证 attrmask 与 attrlist 永远一致；
- `op_getattr`、`op_readdir` 在解出掩码后命中即回 INVAL，`op_verify`/`op_nverify` 的
  逐位循环里与 `rdattr_error` 同一支处理；
- `supported_attrs()` 与 `settable_attrs()` **未动**——SETATTR 侧行为不变；
- 回归测例：`Nfs4.WriteOnlyAttrsAreNotReadable`（GETATTR 单独/混合请求、READDIR、
  VERIFY/NVERIFY 各一条，另断言 `supported_attrs` 仍宣告这两位、SETATTR 仍能设
  `time_modify_set`）与 `Nfs4.EncodeFattrNeverEmitsValuelessAttrs`（编码器层不变式：
  attrmask 不含无值位、attrlist 长度恰等于各值之和且被读尽）。
  撤掉修复后两条测例分别 7 / 2 处失败——修复前 VERIFY 回 NOT_SAME、NVERIFY 回 OK，
  即"拿截断的 attrlist 做了一次错误比较"，比单纯解析失败更隐蔽。

### A2 `fs_locations_info` 破坏属性升序（已修复）

fattr4 的 attrlist 必须按属性号升序排列。`encode_fattr` 的编码顺序里
`kFsLocationsInfo`(67) 写在 `src/nfsv4/attrs.cpp:196`，而 `kMountedOnFileid`(55) 在
`:216`——67 排到了 55 前面。

```
实测（直接调 encode_fattr，referrals=true，mask={mounted_on_fileid, fs_locations_info}，
      mounted_on_fileid=0xbbbb，lease=90）：
  PROBE first value after mask = 0x5a      ← fli_flags(0) + fli_valid_for(90=0x5a)
                                              正确应为 0xbbbb
```

触发条件是 `referrals`（`[cluster] mode = active-active`）开启且客户端同时请求这两个
属性——Linux 客户端做 referral 探测时请求 `fs_locations` + `mounted_on_fileid` 是常见
组合，所以这条在多活模式下是实打实会踩的。

**修法**：把 `kFsLocationsInfo` 整块移到 `kMountedOnFileid` 之后、`kSuppattrExclCreat`
之前。顺便建议给 `encode_fattr` 加一个 debug 断言：每写一个属性，校验其编号严格大于
上一个。

**已修复**（本轮）：
- `encode_fattr` 里把 `kMountedOnFileid`(55) 移到 `kFsLocationsInfo`(67) 之前；整张表现在
  严格升序（0,1,2,3,4,…,53,55,67,75,79），A2 是唯一一处错位；
- 按本节建议加了常驻守卫：`ok()` 是每个属性的**唯一**访问点、且按源码顺序被调用（无论该位
  是否置位），于是在 `ok()` 里断言 `id` 严格递增。挪错位置的代码块在第一次编码时就 abort，
  而不是发出一个客户端会解析到错字段的 attrlist（debug 构建生效，release 零开销）；
- 回归测例两条：`Nfs4.EncodeFattrOrdersReferralAttrsAscending` 按 fattr4 逐字段解
  {fsid, fs_locations, mounted_on_fileid, fs_locations_info}，断言 55 的值出现在 67 之前且
  attrlist 恰好读尽；`Nfs4.EncodeFattrFullSetRoundTrips` 是本节建议的那条不变式——请求
  `supported_attrs()` 的**全集**（referrals 关/开各一轮），按每个属性的线格式走完整条
  attrlist，断言掩码枚举顺序与值的顺序一致、长度恰等于各值之和、读完正好到尾。这张形状表
  刻意重复了编码器的知识：新增属性必须同步这里，否则测例会失败。
  两条测例都不解引用未校验的解码结果、也不信任解出来的数组长度——错位之后的读全是垃圾，
  测例必须"干净地失败"，而不是崩在 `Result::value()` 的断言上或在垃圾计数上打转。
- 反向验证：把顺序改回去（并去掉断言）后两条分别失败 12 / 3 处（`referrals = false` 那轮
  不受影响，符合预期）。

### A3 v3 超长名字/路径被降级成 RPC GARBAGE_ARGS（已修复）

解码层就把长度当成 XDR 上界：

- `Diropargs::decode` → `dec.string(kMaxName)`，`kMaxName = 255`（`src/nfsv3/nfs3_types.cpp:20`）
- `SymlinkArgs::decode` → `dec.string(kMaxPath)`，`kMaxPath = 1024`（`:200`）
- MNT 路径 → `call.args.string(1024)`（`src/mountd/mount3.cpp:114`）

`XdrDec::string(max)` 对 `len > max` 直接回 `Errno::kGarbage`（`src/xdr/xdr.hpp:80-88`），
引擎于是走 `reply_garbage_args`。后果两条：

1. **错误码错**。256 字节分量的 LOOKUP/CREATE/MKDIR/… 得到 RPC 层 GARBAGE_ARGS，客户端
   翻成 EIO，而不是 ENAMETOOLONG。本仓库自己的
   [nfsv3/08-errors.md](../../reference/nfsv3/08-errors.md) 把 NAMETOOLONG 列进了 LOOKUP /
   CREATE / REMOVE / RENAME / LINK 的合法集合，`core/errmap.cpp:79,106-128` 的白名单也留了
   位置，但 `ENAMETOOLONG` 永远到不了那里（网关在解码期就挡了，后端也收不到长名字）。
   之所以 `scripts/posix_semantics.py` 的 `ENAMETOOLONG` 检查现在能过：Linux 客户端按
   PATHCONF 的 `name_max` 在本地先挡下来了，压根没上线。裸 RPC 客户端、cthon 与 pynfs 的
   裸测例会暴露。
2. **功能缺失**（影响更实际）。symlink 目标在 1025..4095 字节之间是完全合法的 POSIX 值
   （PATH_MAX = 4096），RFC 1813 的 `nfspath3` 也没有 1024 这个限制。现在 `ln -s <长目标>`
   在 v3 挂载上回 EIO。v4 侧没有这个问题（`kMaxSymlink = 4096`）。

**修法**：解码上界放宽到"协议/实现能承受的上界"（name 给一个宽松上界，path 给 4096+），
把长度判定交给已有的 `core::check_component()`（它已经有 `NameCheck::kTooLong`，只是
`valid_component()` 把它和其他失败一起折成 bool 丢掉了），让 v3 把 `kTooLong` 映射成
NFS3ERR_NAMETOOLONG、mountd 映射成 MNT3ERR_NAMETOOLONG（`map_error` 已有这一支）。

**已修复**（本轮，连带 C5）：
- 解码上界与语义上界分开：`nfs3_types.hpp` 新增 `kNameWireMax = 4096` / `kPathWireMax = 16384`
  作为**纯 DoS 天花板**（`filename3`/`nfspath3` 在 RFC 1813 §2.5 里本就是 `string<>`），
  `kMaxPath` 从 1024 改成 POSIX 的 4096 当语义上界，原来那个 `kMaxName = 255` 删掉——
  名字的语义上界不再是硬编码，而是**该导出后端的 `limits().max_name`**，也就是 PATHCONF /
  v4 `maxname` 宣告的那个数（`local` 后端从 `fpathconf(_PC_NAME_MAX)` 探出来，见
  `local.cpp:435`，此前与硬编码的 255 可能不一致）；
- `MutateGuard::precheck` 改用 `check_component(name, exp.backend->limits().max_name)`，
  v3 的 `verdict_status()` 为 `NameCheck::kTooLong` 单独返回 NFS3ERR_NAMETOOLONG（其余名字
  失败仍是 ACCES，RMDIR 的 "." / ".." 分支不动）；
- v3 SYMLINK 目标 >`kMaxPath` 回 NAMETOOLONG；1025..4096 字节的目标现在正常创建；
- LOOKUP 的名字检查从"与 XDR 失败合并成 GARBAGE_ARGS"改成解出句柄后再判：`kTooLong` →
  NAMETOOLONG，空名字/含 `/` 或 NUL → NOENT（这就是 C5）。顺带把 STALE 的优先级摆正了——
  坏名字不再抢在句柄校验之前。
- mountd：`dirpath` 解码放宽到 `kMountPathWireMax = 8192`，超过 `kMaxMountPath = 1024`
  （RFC 1813 §5.1.1 的 MNTPATHLEN）回 MNT3ERR_NAMETOOLONG；路径里某个分量超长同样回
  NAMETOOLONG 而不是原来的 MNT3ERR_INVAL。
- 回归测例三条：`Nfs3.OverlongNamesAnswerNametoolong`（LOOKUP / REMOVE / RMDIR / CREATE /
  MKDIR / SYMLINK / RENAME / LINK 各一条超长名字，外加 255 字节名字仍是 NOENT、`a/b` 与
  空名字是 NOENT、RMDIR 的 "." / ".." 仍是 INVAL / EXIST、超过解码天花板仍是
  GARBAGE_ARGS）、`Nfs3.LongSymlinkTargetIsAccepted`（2000 字节目标创建成功并 READLINK
  读回，>PATH_MAX 回 NAMETOOLONG）、`Mount3.OverlongPathAnswersNametoolong`。
  三条都不裸解引用回复里的字段：**GARBAGE_ARGS 回复根本没有 body**，直接读会崩在
  `Result::value()` 的断言里而不是给出失败——这正是这几条测例要检查的那种输入。
- 反向验证（保留新常量、只回退行为）：三条分别失败 15 / 1 / 3 处，失败值恰是修复前的行为
  （accept_stat = 4 即 GARBAGE_ARGS、status 读不出来、MOUNT 分量超长回 22 即 MNT3ERR_INVAL）。

### A4 v4 超长名字回 BADNAME / BADXDR（已修复）

v4 的分量解码是 `dec.string(kMaxName + 1)` = 256 字节上界（`src/nfsv4/engine.cpp:786,
1722, 2520, 2581-2582, 2654, 3001` 等）。于是：

- 恰好 256 字节 → 解码通过 → `core::valid_component()` 因 `kTooLong` 失败 → **BADNAME**（`:789`）
- ≥ 257 字节 → 解码失败 → **BADXDR**

RFC 8881 §18.10.3（LOOKUP）、§18.16.3（OPEN）、§18.4.3（CREATE）等都要求
**NFS4ERR_NAMETOOLONG**。`core/errmap.cpp:221,245` 的白名单同样留了位置但不可达。

**修法**：与 A3 同源，同一次改动。`verdict_status4()`（`src/nfsv4/engine.cpp:65-71`）要为
`NameCheck::kTooLong` 单独返回 NAMETOOLONG（现在只区分 `kEmpty` → INVAL，其余 → BADNAME）。

**已修复**（本轮）：
- 与 A3 同一套做法：`nfs4_types.hpp` 删掉 `kMaxName`，新增 `kNameWireMax = 4096` /
  `kSymlinkWireMax = 16384` 作为**纯 DoS 天花板**（`component4` / `linktext4` 都是
  `utf8str_cs`，RFC 8881 §3.2 里是 `string<>`），语义上界回到"该导出后端的
  `limits().max_name`"——也就是 `maxname` 属性宣告的那个数；
- 新增 `name_status4(NameCheck)`：`kTooLong` → NAMETOOLONG、`kEmpty` → INVAL、其余
  （"." / ".." / 含 `/` 或 NUL）→ BADNAME。`verdict_status4()` 的 `kBadName` 分支改调它，
  于是 CREATE / REMOVE / RENAME / LINK 四个走 `MutateGuard::precheck` 的 op 一次到位；
- 自己查名字的三个 op（LOOKUP / OPEN CLAIM_NULL / SECINFO）也改调同一个判定。LOOKUP 与
  SECINFO 的检查因此挪到解出句柄之后（长度上界要先知道是哪个文件系统），顺带把 STALE 的
  优先级摆正——与 A3 在 v3 侧做的一样；
- 顺带对齐 v4 与 v3 在**同一个输入**上的分歧：`CREATE(NF4LNK)` 的目标超过 PATH_MAX 现在回
  NAMETOOLONG 而不是 BADXDR（v3 SYMLINK 在 A3 里已经这么改了）。严格说这不在 A4 的条目
  里，但两个引擎对同一个 symlink 目标给不同答案，正是这份审计要消掉的东西。
- 回归测例两条：`Nfs4.OverlongComponentsAnswerNametoolong`（LOOKUP / SECINFO /
  OPEN×2（NOCREATE 与 CREATE）/ CREATE / REMOVE / RENAME / LINK 共 8 处超长名字，外加
  255 字节名字仍是 NOENT、`""` → INVAL、`"."` / `".."` / `"a/b"` → BADNAME、超过解码天花板
  仍是 BADXDR）、`Nfs4.LongSymlinkTargetIsAccepted`（2000 字节目标创建并 READLINK 读回、
  >PATH_MAX → NAMETOOLONG、>天花板 → BADXDR）。
- 反向验证（保留新常量、只回退行为）：两条分别失败 8 / 1 处，失败值全是 10041（BADNAME）
  与 10036（BADXDR），即修复前的行为。注意 2000 字节目标那条在修复前也通过——v4 原本的
  上界就是 4096，那条是回归守卫而不是缺陷复现。

### A5 FREE_STATEID 不校验 stateid 属主（已修复）

```
rt::Task<uint32_t> StateMgr::free_stateid(const Stateid& sid)   // state_mgr.cpp:1595
```

签名里没有 `clientid`，实现里也不比对 `rec->client->clientid`；`op_free_stateid`
（`src/nfsv4/engine.cpp:2765`）手上有 `ctx.clientid` 但不传。对照同文件的
`close_state`（`:1546` 处比对 clientid）、`locku`（`:1602` 处比对）、`delegreturn`
（`:1196` 处比对），这一处是漏的。

单独看影响有限——只有 `type == kLock` 且当前无持锁范围的 stateid 能被释放。但叠加 B1 的
**stateid.other 完全可推**（`new_other()` = `epoch32 | type | counter`，
`state_mgr.cpp:141-150`），任意已建会话的客户端可以按 counter 枚举 type=2 的 stateid 并
释放掉别人的 lock stateid；受害方后续 LOCK/LOCKU 拿到 NFS4ERR_BAD_STATEID，锁流程断掉
（Linux 客户端会走状态恢复，但期间应用层 fcntl 会失败）。

RFC 8881 §18.38.3：不属于本客户端的 stateid → NFS4ERR_BAD_STATEID。

**修法**：`free_stateid(sid, clientid)`，比对不符回 BAD_STATEID。测例：
`tests/test_nfs4.cpp` 里补一条"客户端 B 的会话 FREE_STATEID 客户端 A 的 lock stateid"。

---

**已修复**（本轮）：
- `free_stateid(sid)` → `free_stateid(sid, clientid)`，`rec->client->clientid != clientid`
  回 BAD_STATEID；`op_free_stateid` 传 `ctx.clientid`。**没有留无属主的重载**——内部若要
  无条件丢状态，走 `unlink_state()`，不该从这个入口绕。
- 属主检查**排在类型与持锁检查之前**：别人的 stateid 于是和「不存在的 stateid」给出完全一样
  的回答，回复里不透露调用方无权知道的状态。修复前 B 拿 A 的 **open** stateid 去 FREE 会得到
  LOCKS_HELD——等于确认「这个 stateid 是个活着的 open」，在 `other` 可枚举（B1）的前提下是一个
  可用的探测原语。
- 回归测例两条：`Nfs4.FreeStateidRejectsForeignStateids`（引擎层，两个会话共用一条连接：
  A 开文件、加锁、LOCKU 掉区间；B 的会话 FREE_STATEID A 的 lock stateid / 一个不存在的
  stateid / A 的 open stateid，三者都必须是 BAD_STATEID；换回 A 的会话同一调用成功，A 自己的
  open stateid 仍是 LOCKS_HELD）与 `StateMgr.ByteRangeLocksLifecycle`（状态层直接断言）。
- 反向验证（去掉属主检查）：两条各失败 3 处，失败值把这个洞讲清楚了——
  **B 释放 A 的 lock stateid 返回 0（成功）**，紧接着 A 自己再 FREE 同一个 stateid 得到 10025
  （BAD_STATEID），也就是审计里写的「受害方后续 LOCK/LOCKU 拿到 BAD_STATEID，锁流程断掉」；
  B 拿 A 的 open stateid 返回 10037（LOCKS_HELD），即上面那条信息泄露。

## B. 语义偏差与安全边界偏差

### B1 会话标识可推（已修复）；连接绑定与 stateid 可推（原判定过重，已更正）

> **更正**：本条初版把两件事合成一条，并断言都是对 RFC / 参考实现的偏离。复核后
> **(b) 连接绑定、以及 (a) 里 stateid 那半都站不住**——它们与 knfsd 在 SP4_NONE 下的行为一致，
> 而且本仓库的调研分册早就记过结论。真正成立的只有 **sessionid 可推**这一条，已修。
> 下面按「成立 / 不成立」重写，保留原始论据以便复核。

**(a1) sessionid 可推 —— 成立，已修复。**

```
// state_mgr.cpp:549-553（修复前）
std::memcpy(session->id.data(),      &client->clientid, 8);   // = epoch32 | client counter32
std::memcpy(session->id.data() + 8,  &counter,          4);   // 全局 session counter
std::memcpy(session->id.data() + 12, &epoch32,          4);   // boot epoch 的副本
```

整个 16 字节由「boot epoch + 两个小计数器」决定，**没有任何随机分量**。后果：白名单网段内任一
主机能在个位数次尝试内猜出别人的 sessionid，并在自己的连接上发 SEQUENCE 冒用该会话——
`ctx.clientid` 是从 sessionid 前 8 字节直接取的（`nfsv4/engine.cpp` 的 SEQUENCE 路径），所以
`check_io` 里 `rec->client->clientid != clientid` 那道校验也一并通过：受害者的 open/lock
stateid 可以拿来做 IO，DESTROY_SESSION 也能无声打断它的会话。

**已修复**（本轮）：末 4 字节从「boot epoch 的副本」（无人解析）换成**每会话**的 `getrandom`
随机量，布局变成 `clientid(8) | counter(4) | random(4)`。
- 前 8 字节必须留成 clientid（引擎依赖，见上）；counter 留着，让**唯一性仍是构造保证**而不是
  概率。
- 随机量必须**每会话一份**：一个进程级常量在所有会话间相同，等于没加。
- 32 位意味着在线猜测约 2^31 次失败 COMPOUND，每次都回 BADSESSION 且留日志——从「随手可猜」
  变成「不实际」。**不是**密码学强度：真正敌对的网络仍然是
  [09-security](../../reference/nfsv4/09-security.md) §9.5 说的 SP4_MACH_CRED / TLS 问题，这里
  只是把白送的那一步收掉。
- `getrandom` 失败时退回「只有 counter」并打 warn，而不是退回一个可预测的常量。
- 回归测例 `Nfs4.SessionIdIsNotPredictable`：把旧构造**写进测例**——用
  `clientid | counter(0..7) | epoch` 以及全 0 / 全 1 尾巴重建 10 个候选 id，断言没有一个等于
  活着的会话、且都回 BADSESSION；再断言两个会话的尾 4 字节不同（进程级随机量会让它们相同）、
  counter 仍在递增（唯一性）、前 8 字节仍等于 clientid（引擎依赖）。
  把尾巴改回 boot epoch 后该测例失败 4 处，关键一条是 `sequence_status(forged) == 0`——
  **伪造的 sessionid 被接受了**。

**(a2) stateid 可推 —— 不成立（作为「偏离」）。**

knfsd 的 stateid 是 `{si_generation, {so_clid, so_id}}`，`so_id` 是每客户端计数器——**同样可
推**；它靠 `nfsd4_lookup_stateid` 校验 `cl_clientid`，也就是属主检查。lightnfs 的
`other = {epoch(4B) | type(1B) | counter(7B)}` 与之同构，缺的正是那道属主检查——那是 **A5**，
已经修了。所以这半条的正确结论是「A5 覆盖」，而不是「给 stateid 加随机量」。

补一句为什么不顺手加随机量：要有意义就得**每 stateid 一份**。共享一个进程级掩码是可逆的
（攻击者拿自己的两个 stateid 就能解出掩码，因为 counter 近乎连续），而每 stateid 独立随机又
丢掉唯一性保证，得改成「counter 的带密钥置换」（如 SipHash 截断 56 位 + 建表查重）。这是参考
实现都没有的复杂度，收益在 A5 之后也很薄。**决定：不做**，属主检查就是防线。

**(b) SEQUENCE 隐式绑定连接 —— 不成立（作为「偏离」）。**

`sequence_begin` 无条件 `bound_conns.insert(conn_id)`（`state_mgr.cpp:734`，注释写着
"implicit bind (trunking-lenient)"），`NFS4ERR_CONN_NOT_BOUND_TO_SESSION` 只在
DESTROY_SESSION 用到。初版据此说它违反 RFC 的「bind before use」。复核结论：

- knfsd 的 `nfsd4_sequence_check_conn()` 对未绑定连接的处理是——**只有 `cl_mach_cred`
  （SP4_MACH_CRED）在生效时才回 CONN_NOT_BOUND_TO_SESSION，否则直接把新连接哈希进会话**。
  lightnfs 的 EXCHANGE_ID 只接受 SP4_NONE，所以当前行为与 knfsd 在同等配置下一致。
- 本仓库的 [06-sessions-v41](../../reference/nfsv4/06-sessions-v41.md) §6.3 也早写了：
  「重连 + BIND_CONN_TO_SESSION（**或直接在新连接上发 SEQUENCE，若服务器允许**）即恢复」——
  这是当初就知道的服务器可选项。

改成严格拒绝会比参考实现更严，而且会打断「重连后不发 BIND_CONN_TO_SESSION 就续用会话」的
客户端（Linux 会发，别的实现不保证）。**决定：不改。** 初版提的折中（要求新连接对端地址与
建会话时一致）也不采纳：RFC 8881 §2.10.5 的 session trunking 本就允许多地址，多宿主客户端会
被误伤。sessionid 变成不可推之后，这条通路上剩下的风险就是「能嗅到线上流量的攻击者」——而那种
攻击者在明文 AUTH_SYS 下本来就能直接伪造身份，属于 09-security §9.5 已记录的边界。

### B2 `squash = root` 只压 uid（已修复）

```
// core/config.cpp:1087-1095
if (entry.squash == Squash::kAll || (entry.squash == Squash::kRoot && out.uid == 0)) {
    out.uid = entry.anon_uid;
    out.gid = entry.anon_gid;
    out.groups.clear();
}
```

`uid == 0` 才触发。一个 `uid=1000, gid=0` 的请求（或附加组里带 0 的请求）在默认
`squash = root` 的导出上**保留 group root 身份**。knfsd 的 `root_squash` 是三件事一起做：
uid 0 → anonuid、gid 0 → anongid、附加组里的 0 → anongid。

后果：导出树里 group-root 可写的对象（例如从系统目录复制过来、保留了 `root:root 0664` 的
文件）在"以为已经压过 root"的导出上仍可被改。

**修法**：`kRoot` 分支拆成三个独立判断，与 knfsd 对齐；`config/lightnfs.toml.example` 与
deployment.md §3 的 squash 说明同步。

**已修复**（本轮）：
- `kRoot` 分支拆成三个独立判断，与 exports(5) / knfsd 的 `nfsd_setuser` 对齐：`uid == 0` →
  `anon_uid`，`gid == 0` → `anon_gid`，附加组里每个 0 → `anon_gid`。`kAll` 原样不动（压一切 +
  清空附加组）。
- 附加组里的 0 是**替换**而不是删除，和 knfsd 一致：调用方保留它声称的组数量，只是 0 变成
  anon，其余不动。
- 回归测例 `ExportSet.RootSquashMapsGroupRootToo`：uid 0（原本就能过的情况）、**uid=1000 +
  gid=0**（B2 的那种）、附加组 `{0, 42, 0}`（断言变成 `{anon, 42, anon}`、长度不变）、
  完全无 root 的普通调用方原样通过、`squash = all` 仍然压平一切、`squash = none` 连 root 都不动。
  撤掉修复后失败 3 处，正是 `group_root.gid`、`supp.groups[0]`、`supp.groups[2]`。
- 文档与样例同步：`config/lightnfs.toml.example` 的 `squash` 键上写清三种模式各映射什么、并
  露出 `anon_uid`/`anon_gid` 两个注释键；[deployment.md](../../guide/deployment.md) §1 与 §3
  的口径都改成「`root` 压的是三样」。

**顺带记一条没做的**：knfsd 在**任何** squash 模式下还会把 `INVALID_UID`/`INVALID_GID`
（即 `(uint32_t)-1`）映射成 anon。lightnfs 目前原样透传，`squash = none` 下会把 0xFFFFFFFF
交给后端（`local` 的 setfsuid 会失败，`kNativeAccess` 后端由存储侧判定）。这会改变
`squash = none` 的行为，不在 B2 条目内，**留作独立项**。

### B3 不检查特权源端口（已修复）

全路径没有任何"源端口 < 1024"的判断（`transport/listener.cpp`、`rpc/dispatch.cpp`、
`core/config.cpp` 均无）。knfsd 默认开 `secure`（exports(5)），正是因为 AUTH_SYS 的信任模型
建立在"**只有客户端内核**能发 NFS RPC"这个前提上——客户端内核保证 RPC 里的 uid 是进程真实
uid。

没有这道检查，受信主机上的**任意非特权用户**可以直接 connect 到 2049 自己拼 AUTH_SYS 报文，
声称 uid=0（再经 squash 变 anon）或任意其他 uid，读写导出里那个 uid 能访问的一切。这比
deployment.md §1 写的"网络内任意**主机**可声称任意用户身份"更宽一档：实际是"受信主机内任意
**用户**"。

**修法**（二选一，建议都做）：
- 加 `[[export]] secure_ports`（默认 `true`，与 knfsd 对齐），在 `check_client` 旁边判端口；
- 至少把 deployment.md §1 的口径改准，明确"受信主机上的非特权用户同样能伪造身份，因此导出
  网段里的主机必须是被完整管控的"。

**已修复**（本轮）：
- 新增 `[[export]] secure_ports`，**默认 `true`**（与 knfsd 的 `secure` 一致），判定落在
  `ExportTable::port_allowed()` 里、由 `check_client()` 调用——于是 v3 句柄解码、v4 句柄解码、
  v4 伪根跨越、MOUNT 四条路径**一次到位**，各自沿用已有的 EACCES / NFS4ERR_ACCESS /
  MNT3ERR_ACCES。NULL 过程不查导出，所以健康检查与 rpcinfo 不受影响；`AF_UNIX` 对端（ctl
  socket）没有端口，不参与判定。
- 与其它 per-export 标量一样热更新（`apply()` 原地翻），并走完整链路：TOML 解析、
  `ExportSetBuilder::add`、`apply`。
- 被拒请求计入 `lightnfs_insecure_port_rejected_total`，并在**进程内第一次**发生时打一条带
  端口与键名的 warn——不刷日志，但第一次就能自诊断。

**默认值的取舍（自行判断，需要复核）**：本条建议的是默认 `true`，我照此实现，理由是
knfsd 的 `secure` 默认开、且这道检查正是 deployment.md §1 那句「网络内任意主机可声称任意用户
身份」成立的前提——没有它，边界实际是「受信主机上任意**用户**」。代价是**行为变更**：拿不到
保留端口的客户端升级后会收到 EACCES。缓解是上面那条 warn + 指标，以及部署文档里写明。

**发现的连带影响**（看代码才发现，不是猜的）：`tests/accept_client.cpp` 的 `connect_tcp()`
从不绑保留端口，而本机验收又是无 root 跑的——所以默认开之后**所有验收脚本都会挂**。处理：
给 10 个驱动 `lnfs_accept_client` 的脚本（`accept_m2/m6_local`、`accept_m6_vm`、
`accept_failover_local`、`accept_active_active_local`、`accept_gluster/cephfs/lustre`、
`fault_inject`、`gen_seccomp_allowlist`）生成的导出块加上 `secure_ports = false` 并注明原因；
纯内核挂载的脚本（`accept_m2_vm`、`accept_failover_vm`、`posix_semantics_vm`、`fsperf_vm`）
不动，内核客户端默认用保留端口，那几条顺带成了「默认开」的端到端覆盖。

- 走完整配置链路，不只是本机 TOML：解析、`ExportSetBuilder::add`、`apply` 热更新，**以及
  共享清单**——`core/catalog.cpp` 的 TOML 发射器与变更检测、`ctl_catalog.cpp` 的
  `--secure-ports` 旗标与 text/JSON 两种 dump。漏掉清单那一半会留个陷阱：给容器导出关掉之后，
  下一次 `cluster export set` 重写清单就把它静默恢复成安全默认值。
- 回归测例两条。`ExportSet.SecurePortsRefusesUnprivilegedSourcePorts`：默认即开（配置不写
  也开）；665 通过 / 34567 拒绝；1023 通过 / 1024 拒绝（边界）；IPv6 走同一道闸；保留端口但在
  CIDR 之外仍拒（两半都在）；`AF_UNIX` 对端放行；`secure_ports = false` 热更新后放行、而 CIDR
  仍然生效。`Ctl.ClusterExportCommands` 末尾加了清单往返断言（旗标 → 发射的 TOML → 解析回来），
  放在最后是因为中间插一次提交会把上面所有按版本号写死的期望串位。
- 端到端：`scripts/accept_m2_local.sh` 全程通过（Release + ASAN + ASAN soak，`exit 0`）——
  验收客户端用非保留端口，经 `secure_ports = false` 正常工作。

**修复过程中撞到的一个既有问题（非本条引入）**：`accept_m2_local.sh` 的 admin-tools 步骤在
`set -o pipefail` 下用 `producer | grep -q PATTERN`。`grep -q` 一命中就退出并关掉管道，于是生产者
拿到 SIGPIPE（`lightnfs-ctl` → 141）或写错误（`curl` → 23），pipefail 把它变成整步失败——**即使
模式是匹配到的**。在**干净 HEAD 上同样失败**（141），所以不是 B3 造成的，但它挡住了 B3 的端到端
验证。已改成「先落盘再 grep」。同一写法在 `scripts/` 下还有约 30 处，**本轮只修了挡路的这一处**，
其余留作独立项（多数在 `[[ ]]` 条件里、不受 pipefail 影响，需要逐个看）。


### B4 DRC 键含源端口，跨重连重传失效（已修复）

```
// rpc/drc.hpp:34-44
struct Key {
    std::array<uint8_t, 16> peer_addr{};
    uint16_t peer_port = 0;        // ← 进了 Key
    uint32_t xid = 0; ...
};
```

DRC 存在的**主用例**恰恰是"连接断了，客户端重连后用同一个 xid 重传一个非幂等过程"
（`docs/reference/nfsv3/09-implementation-notes.md` §9.2 的场景）。Linux 客户端在 TCP 重连后
沿用 RPC 任务原有的 xid，但源端口换了新的——键里带端口，于是必然 miss，REMOVE/RENAME/MKDIR
被重新执行一次：第二次 REMOVE 回 NFS3ERR_NOENT、RENAME 回 NOENT、MKDIR 回 EEXIST。这正是
DRC 要消掉的那一类"`rm` 明明成功了却报 No such file"。

knfsd 的 DRC 用 `rpc_cmp_addr()`，**只比地址不比端口**，就是为了这个。

**修法**：从 `Key` 去掉 `peer_port`（`Key::make` 里也不再取 `sin_port`）。`args_hash`
（`rpc/rpc_msg.cpp` 对参数前 256 字节的 FNV-1a）已经承担了"同 xid 不同请求"的区分职责，
去掉端口不会引入误命中。

**已修复**（本轮）：
- `Key` 去掉 `peer_port`，`Key::make` 不再取 `sin_port`/`sin6_port`，`KeyHash` 不再混它。
  身份只剩地址（v4 映射成 v6 形式），与 knfsd 的 `rpc_cmp_addr` 一致。
- 代价写在类型注释里：同一 NAT 地址后面的两个客户端现在可能撞上，但必须 xid、program、
  version、procedure **和参数校验和**全都相同——那时缓存回的应答对应的正是一个和它逐字节相同
  的请求。
- 回归测例 `WritePath.DrcReplaysAcrossAReconnect`：同一地址、**换源端口**、同 xid 重发
  MKDIR，必须逐字节重放且 `replays == 1`、`inserts == 1`；换成另一个**地址**则不重放、MKDIR
  真的回 EEXIST（证明去掉的只是端口、地址仍然是身份）。全程用保留端口——真实客户端就是这样，
  而且 B3 之后非保留端口根本到不了 DRC。
  把端口加回键里后该测例失败 5 处，关键两条是 `replays: 0 vs 1` 与 `inserts: 2 vs 1`——
  重连的重传 miss 了缓存并重新执行了一次。

### B5 `fh_expire_type` 恒为 FH4_PERSISTENT

```
// nfsv4/attrs.cpp:132-133
// FH4_PERSISTENT
if (ok(kFhExpireType)) vals.u32(0);
```

硬编码 0。但 local 后端在 fallback 句柄模式下**明确不宣告** `kStableHandles`
（`src/backend/local/local.cpp:475-482` 只在 kernel 模式下 `caps_.set(Cap::kStableHandles)`，
`:549-553` 的注释写明"最后兜底那条路正是 fallback 模式从不宣告 kStableHandles 的原因"）。
README 的"已知限制"也写了 `handles = "auto"` 回退模式下句柄稳定性取决于文件系统。

于是在没有 `CAP_DAC_READ_SEARCH`、或文件系统不支持 `name_to_handle_at` 的部署里，句柄实际
重启即失效，服务器却告诉客户端"永久有效"。客户端因此不会准备 NFS4ERR_FHEXPIRED 的恢复路径，
只能吃 ESTALE（Linux 客户端能扛，但这是运气而非契约；
[nfsv4/03-namespace-attrs.md](../../reference/nfsv4/03-namespace-attrs.md) §3.2 列的
FH4_VOLATILE_ANY 正是为这个场景存在的）。

**修法**：`AttrSource` 里已经带了 `const core::FsProps* fs`；给 `FsProps` 加一个
`stable_handles`（`fs_props()` 里从 `Cap::kStableHandles` 取，与 `native_change` /
`native_access` 同一写法），`fh_expire_type` 按它选 `0` / `0x2`。伪根恒为
FH4_PERSISTENT（路径哈希 id，稳定）。

### B6 v3 FSINFO 不宣告 FSF3_CANSETTIME

```
// nfsv3/engine.cpp:584-587
uint32_t props = fs.kHomogeneous ? kFsfHomogeneous : 0;
if (fs.link_support) props |= kFsfLink;
if (fs.symlink_support) props |= kFsfSymlink;
```

`kFsfCanSetTime = 0x0010` 在 `nfs3_types.hpp:87` 定义了，**全仓库没有任何地方用它**；
`core::FsProps::kCansettime = true` 也只被 v4 的 `cansettime` 属性消费
（`nfsv4/attrs.cpp:146`）。而 v3 SETATTR 确实支持 `SET_TO_CLIENT_TIME`
（`decode_sattr`，`nfs3_types.cpp:82-101`）。

后果：会看 `properties` 位的客户端（FreeBSD / Solaris / macOS 一系）不会发
SET_TO_CLIENT_TIME，退化成 SET_TO_SERVER_TIME——`utimes()`、`tar -p`、`rsync -t` 之类的
时间戳恢复失真。Linux 客户端不看这个位，所以本机验收发现不了。

**修法**：一行，`if (core::FsProps::kCansettime) props |= kFsfCanSetTime;`。

### B7 v3 侧的改动不召回 v4 读委托

v4 侧的委托一致性是完整的：
- OPEN 带写意图 → `StateMgr::open` 收集并召回该文件上的全部读委托（`state_mgr.cpp:1004-1018`）
- 特殊（匿名）stateid 的写 → `check_io` 同样召回（`:1437-1462`）
- SETATTR / REMOVE / RENAME → 引擎显式调 `deleg_conflict`（`nfsv4/engine.cpp:2331, 2562, 2636`）
- 因此带真实 stateid 的 WRITE/ALLOCATE/COPY/CLONE 也被覆盖（它们的 stateid 必来自一次写 OPEN）

**v3 引擎完全不持有 StateMgr**（`src/nfsv3/engine.hpp` 的成员只有 exports/handles/locks/
verf/drc）。所以：v3 客户端对一个 v4 客户端正持读委托的文件做 WRITE / SETATTR(size) /
REMOVE / RENAME，委托不被召回，v4 客户端会**无限期**继续读它缓存的旧内容——直到租约到期、
客户端自己重新校验，或委托因别的原因被召回。

README 的"已知限制"里写了「v3 写不受 v4 share reservation 与锁约束（documented boundary）」。
这条比它严重一档：share reservation 的缺失影响的是并发控制（应用层本来就要自己协调），
读委托的缺失导致的是**静默读到过期数据**，且客户端侧无从察觉。同理，本地进程或另一台网关的
带外修改也不会召回委托（后者 README 提了"v4 open/deny state stays per gateway"）。

**修法**（任一）：
1. 给 v3 引擎注入 `StateMgr*`（可选，为 null 时行为不变），在 WRITE / SETATTR / CREATE
   （截断分支）/ REMOVE / RMDIR / RENAME 上调 `deleg_conflict`，非 0 时回 NFS3ERR_JUKEBOX
   让客户端重试——`errmap.cpp:96` 的 WRITE 白名单已经收 JUKEBOX，SETATTR/REMOVE 等需要
   补白名单或改用其他策略。
2. 或者退一步：当进程里同时存在 v3 可达的导出（或处于多网关模式）时，不授予读委托——
   `maybe_grant_read_deleg` 加一个 per-export 开关即可，代价是丢掉这些导出上的委托收益。

方案 1 语义正确；方案 2 一行且零风险。建议按部署形态给一个配置项，默认走 2。

### B8 其余较小的一致性偏差

按影响从大到小：

1. **CLOSE / OPEN_DOWNGRADE / LOCKU 不校验 CFH 与 stateid 指向同一文件**
   （`nfsv4/engine.cpp:2095, 2123, 2966`）。三者都只查 `ctx.cfh.empty()`，随后完全按
   stateid 里记的 `fsid/oid` 动作。RFC 要求当前句柄就是该 stateid 的文件，否则
   BAD_STATEID（knfsd 在 `nfs4_preprocess_seqid_op` 里比对）。行为错乱只会伤到发起方自己，
   故列在 B8。
2. **RECLAIM_COMPLETE `rca_one_fs = TRUE` 直接回 OK**（`:2792-2806`），既不检查当前句柄
   （RFC 8881 §18.51.3 要求有 CFH），也不记录任何 per-fs 完成状态。Linux 客户端发的是
   FALSE，所以实际不可见；但多活模式（per-fsid grace）迟早要用到这一支。
3. **`Bitmap::decode` 对第 4 个非零 word 回 BADXDR**（`nfsv4/nfs4_types.cpp:36-48`）。
   RFC 的口径是"不支持的属性位忽略、不回错"。当前 RFC 8881/7862 的属性号都 < 96，所以不可达；
   未来客户端请求 bit ≥ 96 时会拿到 BADXDR。
4. **OPEN 的 `share_access` 未定义位被静默丢弃**（`nfsv4/engine.cpp:1783`，
   `access = *share_access & 0x3`）。`share_access = 0x4`（无定义）会被当成 access=0 → INVAL
   （正确），但 `0x7` 会被当成 BOTH，忽略未定义的 bit 2。`share_deny` 只查上界
   （`deny > kShareBoth`）。
5. **`errmap` 的 READDIR 白名单缺 NOT_SAME**（`core/errmap.cpp:241`，
   `{kNotdir, kBadCookie, kToosmall, kInval}`）。当前无影响——cookieverf 不匹配时引擎直接
   `enc.u32(st(Status::kNotSame))`（`nfsv4/engine.cpp:1458`），不走 `to_v4()`。但哪天把这条
   改成经 errno 映射，会被白名单折成 NFS4ERR_IO。
6. **MOUNTv3 DUMP 恒回空表**（`mountd/mount3.cpp:70-75`：proc 2 只 `enc.boolean(false)`）。
   README 的 feature 表把 "MNT/UMNT/EXPORT/DUMP" 列为已覆盖；DUMP 实为桩（设计上也确实
   不维护权威 rmtab，UMNT 同样是空操作）。建议改 README 口径而不是实现 rmtab。
7. **MNT 不检查目标是否为目录**（`mountd/mount3.cpp:114-152`）。指向一个普通文件的路径会
   拿到该文件的句柄并回 MNT3ERR_OK，而不是 MNT3ERR_NOTDIR。
8. **4.1 里 REQUIRED 但未实现（回 NOTSUPP）**：`BACKCHANNEL_CTL`(40)、`SET_SSV`(54)。
   后者在 EXCHANGE_ID 只宣告 SP4_NONE 的前提下客户端不会发，实际不可达；前者 Linux 客户端
   也不用（它靠 CREATE_SESSION 的 CONN_BACK_CHAN 与 BIND_CONN_TO_SESSION）。其余未实现的
   opcode 都是 OPTIONAL 或仅 pNFS 服务器必需（GETDEVICEINFO / LAYOUT*，且已宣告
   `EXCHGID4_FLAG_USE_NON_PNFS`），回 NOTSUPP 正确；4.0 专属的 MNI 操作
   （OPEN_CONFIRM / RENEW / SETCLIENTID* / RELEASE_LOCKOWNER）落在 3..58 区间同样回 NOTSUPP，
   也正确。**结论：opcode 覆盖面上没有实质遗漏**，这一条只是备案。
9. **minorversion = 1 的 `supported_attrs` 里含 4.2 才有的 `change_attr_type`(79)**
   （`nfsv4/attrs.cpp:60`，`supported_attrs()` 不分小版本）。4.1 客户端不会请求，无害，但
   宣告面比 RFC 8881 宽。

---

## C. 加固项与已知取舍

### C1 记录标记无片数上限、连接无空闲超时

`RecordStream::read_record()`（`transport/record_stream.cpp:56-86`）只有两道闸：单片
`max_fragment_` 与整记录 `max_record_`。**长度为 0 的非末片不计入 `rec.size()`**，于是片数无上界：
一条连接可以无限发 4 字节的 `0x00000000` 片头把协程挂在那里。`ConnRegistry` 只有
`max_connections = 4096`（`transport/connection.hpp:35`），**没有任何 idle / recv 超时**
（`connection.cpp` 里的 `wait_idle` 是关服时用的）。knfsd 有 `svc_age_temp_xprts` 定期回收
空闲连接。

**建议**：单记录片数上限（例如 1024 片）+ 每连接接收超时（无完整记录到达即断，按 lease 量级）。

### C2 v3 cookieverf 严格化的运营边界（已记为取舍）

[nfsv3/07-caching-consistency.md](../../reference/nfsv3/07-caching-consistency.md) §"已删除项
对应的 cookie" 已经把"分页期间目录被改即回 BAD_COOKIE（v4 NOT_SAME），换不重复不漏项的强
保证"写成了明确选择，代码即如此（`nfsv3/engine.cpp:474`、`nfsv4/engine.cpp:1458`）。

补一条运营上的风险备案：在**持续高频变动的大目录**上，客户端每次重列都会再次撞上 verifier
变化，`ls` 可能长时间不收敛（knfsd 在多数文件系统上 verifier 恒 0，从不回 BAD_COOKIE，所以
这是 lightnfs 特有的行为差异）。**建议**：加 `lightnfs_v3_bad_cookie_total` /
`lightnfs_v4_readdir_not_same_total` 指标以便发现，并考虑给导出一个"容忍跳项"的开关，让运维
在这类目录上可以换成宽松策略。

### C3 AUTH 层的两个记录项

`AuthSys::authenticate` 不校验 verifier（RFC 5531 要求 AUTH_SYS 的 verifier 是
AUTH_NONE/零长）；`AuthNone` 直接给 `uid/gid = 65534`（`rpc/auth.cpp:11-17`）而不走导出的
`anon_uid/anon_gid`。按现有信任边界两者都可接受，备案；`AuthNone` 那条若要统一，改成走
`squash_cred` 的 anon 更自洽。

### C4 `pseudofs.cpp` 的注释已过期

`PseudoFs::attr_of` 的注释写「合成树只在重启/重配时变——而那正是 boot epoch 移动的时候」
（`core/pseudofs.cpp:95-98`）。实现上传进来的是 `ExportSet::pseudo_change()` =
`(epoch << 32) | generation`（`core/config.hpp:311-313`），**每次发布都会变**——这是对的
（共享导出清单热更新后，v4 客户端的伪根 READDIR 会因 verifier 变化而重列，位置型 cookie 不会
错位）。注释该改，免得后人按注释去"修"。

### C5 v3 LOOKUP 的非法分量也回 GARBAGE_ARGS（已随 A3 修复）

`nfsv3/engine.cpp:287` 把 `!core::valid_component(args->name, true)` 与 XDR 解码失败合并成
一条 `reply_garbage_args`。"." / ".." 是放行的，但含 `/` 或 NUL 的分量会变成 RPC 层错误而不是
NFS3ERR_ACCES / NOENT。与 A3 同源，同一次改动里一起收。

---

## 建议的修复顺序

1. ~~**A1**、**A2**~~ —— 均已修复（见上）。畸形回复这一类目前已清空。
2. ~~**A5**、**B1**~~ —— 跨客户端的状态隔离，两条都收口了：A5 补上 stateid 属主检查，
   B1 让 sessionid 不可推。B1 里「stateid 加随机量」与「SEQUENCE 严格连接绑定」两项复核后
   **判定为不做**（理由见 B1 正文）。
3. ~~**A3**、**C5**、**A4**~~（均已修复，见上）、**B6**、**B5** —— 错误码与属性宣告的一致性。
   `check_component` 的 `kTooLong` 现在在 v3、mountd、v4 三处都通到了对应的 NAMETOOLONG；
   剩下 B6（FSF3_CANSETTIME）与 B5（fh_expire_type）两条属性宣告。
4. ~~**B2**、**B3**~~ —— 身份压缩与源端口，均已修复；两条都动了安全默认值，文档与配置样例已同步。
5. ~~**B4**~~（已修复，见上）、**B7** —— B7 建议先上「有 v3 导出则不授委托」的一行版本，
   再决定要不要做完整的 v3 召回。
6. **B8 / C 类** —— 按需。B8.5（errmap 白名单）与 C4（过期注释）属于"改一行防将来踩"。

## 验证建议

- **（A4 已由 `Nfs4.OverlongComponentsAnswerNametoolong` 与 `Nfs4.LongSymlinkTargetIsAccepted`
  覆盖）** A1/A2 已由 `Nfs4.WriteOnlyAttrsAreNotReadable`、
  `Nfs4.EncodeFattrNeverEmitsValuelessAttrs`、`Nfs4.EncodeFattrOrdersReferralAttrsAscending`、
  `Nfs4.EncodeFattrFullSetRoundTrips` 四条覆盖）：pynfs 4.1 的 GETATTR / 属性组能覆盖 A4；原先提到 A2 需要一个 referrals
  开启的用例（`[cluster] mode = active-active`），或者像本次审计一样在
  `tests/test_nfs4.cpp` 里直接对 `encode_fattr` 断言"属性号严格升序 + attrlist 长度等于各
  属性值长度之和"——后者是能一次性守住整类问题的不变式，建议常态化。
- **A3**：v3 侧补两条裸 RPC 用例——256 字节分量的 LOOKUP（期望 NAMETOOLONG）与 2000 字节
  symlink 目标的 SYMLINK（期望成功）。注意**不能**用 Linux 客户端挂载来测：客户端按
  PATHCONF 的 `name_max` 在本地就挡了，测不到线上行为，这也是这条一直没被发现的原因。
- 跨客户端隔离目前三条：`Nfs4.FreeStateidRejectsForeignStateids`、
  `StateMgr.ByteRangeLocksLifecycle`（A5）、`Nfs4.SessionIdIsNotPredictable`（B1）。若要再补
  「B 用 A 的 stateid 做 READ / DESTROY_SESSION」，A5 建好的「两个会话共用一条连接」夹具写法
  可直接照抄；注意伪造 sessionid 的 SEQUENCE **不能**让夹具的 slot 计数器前进（用
  `session_body` 的 `force_seq`），否则后续真实调用全部 SEQ_MISORDERED，测的就成了自己的簿记。
- **B4**：DRC 用例里换一个源端口重发同一 xid，断言命中缓存（`tests/test_rpc.cpp` 已有 DRC
  测例可扩展）。
- **B7**：在 `tests/` 里搭一个 v3 引擎 + v4 引擎共享同一 `MemoryBackend` 与 `StateMgr` 的
  夹具，断言 v3 WRITE 触发 CB_RECALL。
