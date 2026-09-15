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
| A3 | A | `nfsv3/nfs3_types.cpp:20,200`、`mountd/mount3.cpp:114` | v3 名字 >255B、symlink 目标 >1024B、MNT 路径 >1024B 一律 RPC GARBAGE_ARGS | 应为 NFS3ERR_NAMETOOLONG；`ln -s <1KB+ 目标>` 在 v3 上直接 EIO |
| A4 | A | `nfsv4/engine.cpp:789` 等 | v4 名字 256B → BADNAME，≥257B → BADXDR | 应为 NFS4ERR_NAMETOOLONG；`errmap` 白名单里的 NAMETOOLONG 无路径可达 |
| A5 | A | `state/state_mgr.cpp:1595`、`nfsv4/engine.cpp:2765` | FREE_STATEID 不校验 stateid 属主 | 叠加 B1 的可推 stateid → 可释放别人的 lock stateid |
| B1 | B | `state/state_mgr.cpp:141,549,734` | sessionid / clientid / stateid 全无随机分量；SEQUENCE 接受任意连接 | 网段内可枚举并冒用别人的会话与状态 |
| B2 | B | `core/config.cpp:1089` | `squash = root` 只压 uid，不压 gid 0 与附加组 0 | 组 root 可写的文件仍可被写 |
| B3 | B | 全路径缺失 | 无特权源端口（"secure"）检查 | 受信主机上的**普通用户**即可声称任意 uid |
| B4 | B | `rpc/drc.hpp:39` | DRC 键含源端口 | 跨重连的重传 miss → 非幂等过程重放（REMOVE 回 NOENT 等） |
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

### A3 v3 超长名字/路径被降级成 RPC GARBAGE_ARGS

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

### A4 v4 超长名字回 BADNAME / BADXDR

v4 的分量解码是 `dec.string(kMaxName + 1)` = 256 字节上界（`src/nfsv4/engine.cpp:786,
1722, 2520, 2581-2582, 2654, 3001` 等）。于是：

- 恰好 256 字节 → 解码通过 → `core::valid_component()` 因 `kTooLong` 失败 → **BADNAME**（`:789`）
- ≥ 257 字节 → 解码失败 → **BADXDR**

RFC 8881 §18.10.3（LOOKUP）、§18.16.3（OPEN）、§18.4.3（CREATE）等都要求
**NFS4ERR_NAMETOOLONG**。`core/errmap.cpp:221,245` 的白名单同样留了位置但不可达。

**修法**：与 A3 同源，同一次改动。`verdict_status4()`（`src/nfsv4/engine.cpp:65-71`）要为
`NameCheck::kTooLong` 单独返回 NAMETOOLONG（现在只区分 `kEmpty` → INVAL，其余 → BADNAME）。

### A5 FREE_STATEID 不校验 stateid 属主

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

## B. 语义偏差与安全边界偏差

### B1 会话与状态标识全可推，且 SEQUENCE 不做连接绑定

两件事叠在一起才构成风险，所以合成一条。

**(a) 标识没有随机分量。**

```
// state_mgr.cpp:549-553
std::memcpy(session->id.data(),      &client->clientid, 8);   // = epoch32 | client counter32
std::memcpy(session->id.data() + 8,  &counter,          4);   // 全局 session counter
std::memcpy(session->id.data() + 12, &epoch32,          4);
```

clientid 自身是 `{boot_epoch(32) | counter(32)}`，stateid.other 是
`{boot_epoch(4B) | type(1B) | counter(7B)}`（`:141-150`）。**三者都不含 `getrandom` 的
随机量**，搜索空间只有"boot epoch + 两个小计数器"。

**(b) SEQUENCE 无条件接受任意连接。**

```
// state_mgr.cpp:734
session.bound_conns.insert(conn_id);      // 注释：implicit bind (trunking-lenient)
```

`NFS4ERR_CONN_NOT_BOUND_TO_SESSION` 在代码里定义了，但只有 DESTROY_SESSION 用到
（`:649`）。RFC 8881 的"bind before use"要求：除了发 CREATE_SESSION 的那条连接自动绑定，
其他连接必须先 BIND_CONN_TO_SESSION，否则 SEQUENCE 应回 CONN_NOT_BOUND_TO_SESSION。

**合起来的后果**：`clients` 白名单网段内的任一主机，可以枚举出一个有效 sessionid，在**自己
的连接上**发 SEQUENCE 冒用别人的会话。因为 `ctx.clientid` 是从 sessionid 前 8 字节直接取
的（`src/nfsv4/engine.cpp:361`），`check_io` 里 `rec->client->clientid != clientid` 这道校验
也一并被绕过——于是同样可推的 open/lock stateid 可以拿来做 IO；DESTROY_SESSION 可以无声打断
别人的会话。文件句柄有 SipHash HMAC，伪造不了，但攻击者可以自己 PUTROOTFH/LOOKUP 走一遍
命名空间拿到合法句柄；导出的 CIDR 白名单仍然生效。

**这不等价于 AUTH_SYS 本来就有的风险**。deployment.md §1 的信任边界说的是"网络内任意主机可
声称任意用户身份"——那是身份层面的。抢占别人的**打开状态与字节锁**、无声中断别人的**会话**，
不在那条边界覆盖范围内：即使两个客户端用的是同一个 uid，NFS 的状态模型也承诺它们的 open/lock
互不干扰。

**修法**（两处都很便宜）：
1. sessionid 的后 8 字节、stateid.other 的 counter 段混入一个每进程的 `getrandom` 随机量。
   **保留** `epoch32` 前缀——`epoch_of(sid.other)` 的重启判定（`state_mgr.cpp:1445` 等多处）
   依赖它，且 clientid 高 32 位的 epoch 语义也要留。
2. SEQUENCE 对未绑定连接回 CONN_NOT_BOUND_TO_SESSION；若要保留 trunking 的宽松度，
   至少要求新连接的对端地址与建会话时一致（`ConnCtx::peer` 已有）。

### B2 `squash = root` 只压 uid

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

### B3 不检查特权源端口

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

### B4 DRC 键含源端口，跨重连重传失效

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

### C5 v3 LOOKUP 的非法分量也回 GARBAGE_ARGS

`nfsv3/engine.cpp:287` 把 `!core::valid_component(args->name, true)` 与 XDR 解码失败合并成
一条 `reply_garbage_args`。"." / ".." 是放行的，但含 `/` 或 NUL 的分量会变成 RPC 层错误而不是
NFS3ERR_ACCES / NOENT。与 A3 同源，同一次改动里一起收。

---

## 建议的修复顺序

1. ~~**A1**、**A2**~~ —— 均已修复（见上）。畸形回复这一类目前已清空。
2. **A5、B1** —— 跨客户端的状态隔离。B1 的两个子项独立，可分别落。
3. **A3、A4、C5、B6、B5** —— 错误码与属性宣告的一致性，一次改动可以一起收
   （`check_component` 的 `kTooLong` 打通到两个引擎 + 两个属性位）。
4. **B2、B3** —— 身份压缩与源端口，动的是安全默认值，需要同步文档与配置样例。
5. **B4、B7** —— DRC 键与委托一致性。B4 是删一个字段；B7 建议先上"有 v3 导出则不授委托"的
   一行版本，再决定要不要做完整的 v3 召回。
6. **B8 / C 类** —— 按需。B8.5（errmap 白名单）与 C4（过期注释）属于"改一行防将来踩"。

## 验证建议

- **A4**（A1/A2 已由 `Nfs4.WriteOnlyAttrsAreNotReadable`、
  `Nfs4.EncodeFattrNeverEmitsValuelessAttrs`、`Nfs4.EncodeFattrOrdersReferralAttrsAscending`、
  `Nfs4.EncodeFattrFullSetRoundTrips` 四条覆盖）：pynfs 4.1 的 GETATTR / 属性组能覆盖 A4；原先提到 A2 需要一个 referrals
  开启的用例（`[cluster] mode = active-active`），或者像本次审计一样在
  `tests/test_nfs4.cpp` 里直接对 `encode_fattr` 断言"属性号严格升序 + attrlist 长度等于各
  属性值长度之和"——后者是能一次性守住整类问题的不变式，建议常态化。
- **A3**：v3 侧补两条裸 RPC 用例——256 字节分量的 LOOKUP（期望 NAMETOOLONG）与 2000 字节
  symlink 目标的 SYMLINK（期望成功）。注意**不能**用 Linux 客户端挂载来测：客户端按
  PATHCONF 的 `name_max` 在本地就挡了，测不到线上行为，这也是这条一直没被发现的原因。
- **A5 / B1**：`tests/test_nfs4.cpp` 里补"客户端 B 操作客户端 A 的 stateid / sessionid"的
  跨客户端隔离用例组（FREE_STATEID、READ、DESTROY_SESSION 各一条）。
- **B4**：DRC 用例里换一个源端口重发同一 xid，断言命中缓存（`tests/test_rpc.cpp` 已有 DRC
  测例可扩展）。
- **B7**：在 `tests/` 里搭一个 v3 引擎 + v4 引擎共享同一 `MemoryBackend` 与 `StateMgr` 的
  夹具，断言 v3 WRITE 触发 CB_RECALL。
