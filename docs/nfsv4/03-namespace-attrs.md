# 3. 命名空间与属性模型

## 3.1 伪文件系统（pseudo-fs）：MOUNT 协议的替代品

v4 服务器把所有导出组织成**一棵以"伪根"为顶的单一命名空间**。客户端 `PUTROOTFH` 拿到伪根句柄，然后像走普通目录一样 LOOKUP 到导出点。

```
服务器导出 /vol/home 和 /vol/data/pub 时的伪文件系统：

        (伪根 "/")           ← PUTROOTFH 到这里
         └── vol/            ← 伪目录：只读、合成的、只含通往导出的路径
              ├── home/      ← 真实导出（跨入真文件系统，fsid 变化）
              └── data/
                   └── pub/  ← 真实导出
```

- 伪目录是服务器**合成**的：只读（修改返回 NFS4ERR_ROFS），只列出通向导出点的名字，属性最小化（fsid 独立、fileid 合成）。
- 客户端靠 **fsid 属性变化**察觉"跨过了文件系统边界"（mount 命令据此在本地建独立挂载点，`crossmnt` 自动化了这件事）。
- 未导出的兄弟路径 LOOKUP 返回 NFS4ERR_NOENT（不可见）——伪 fs 天然实现了"只暴露导出"。
- Linux 服务器用 `fsid=0`/`fsid=root` 指定伪根（历史遗留：早期要手工构建伪根；现代 nfsd 自动合成）。
- 安全协商：不同导出可要求不同 flavor，跨边界时客户端用 SECINFO/SECINFO_NO_NAME 询问（见 [09-security.md](09-security.md)）。

**lightnfs 提示**：单导出场景的伪 fs 可以退化为"伪根=导出根"或"伪根下一个名字"；但 fsid 区分伪根与真实 fs、伪根只读这两点仍要做对，否则 Linux 客户端 `mount server:/ /mnt` 行为诡异。

## 3.2 文件句柄：128 字节，允许易失

```c
#define NFS4_FHSIZE 128
typedef opaque nfs_fh4<NFS4_FHSIZE>;
```

v4 承认"有些服务器做不到永久句柄"（用户态服务器、重导出、HA 切换），引入句柄类型（属性 `fh_expire_type`）：

| 类型 | 位 | 语义 |
|------|----|------|
| FH4_PERSISTENT | 0x0 | 永久有效（v3 语义），**推荐** |
| FH4_NOEXPIRE_WITH_OPEN | 0x1 | 打开期间不失效 |
| FH4_VOLATILE_ANY | 0x2 | 任何时刻可能失效（NFS4ERR_FHEXPIRED） |
| FH4_VOL_MIGRATION / FH4_VOL_RENAME | 0x4/0x8 | 仅迁移/改名时失效 |

- 易失句柄失效后客户端凭**缓存的路径**重新 LOOKUP 恢复；恢复不了（组件被改名）就报错给应用。
- 易失句柄是给受限实现的逃生门，代价是客户端恢复逻辑复杂、边角场景（打开中被改名）语义变差。**能做持久句柄就做持久句柄**。

## 3.3 bitmap 属性模型

v4 属性不再是定长结构。GETATTR/READDIR/SETATTR/VERIFY 都用 `bitmap4`（uint32 数组，位号=属性号）选择属性，值区是按位号升序连续 XDR 编码的 opaque：

```c
typedef uint32_t bitmap4<>;      /* 位图，可多字 */
struct fattr4 { bitmap4 attrmask; attrlist4 attr_vals; /* opaque<> */ };
```

服务器**只需支持强制属性**，推荐属性按能力提供；不支持的位在 `supported_attrs` 中不置位，客户端请求了也直接忽略（GETATTR 对不支持的位不报错、不返回）。SETATTR 对不支持/只读属性则返回 NFS4ERR_ATTRNOTSUPP / NFS4ERR_INVAL。

### 强制（REQUIRED）属性——必须全部实现

| # | 属性 | 说明 |
|---|------|------|
| 0 | supported_attrs | 本 fs 支持的属性位图 |
| 1 | type | 对象类型（REG/DIR/LNK/BLK/CHR/SOCK/FIFO/ATTRDIR/NAMEDATTR） |
| 2 | fh_expire_type | 句柄易失性（见 3.2） |
| 3 | **change** | **单调变化计数**，替代 mtime 做缓存一致性（重中之重，见 3.4） |
| 4 | size | 字节大小 |
| 5 | link_support | fs 是否支持硬链接 |
| 6 | symlink_support | fs 是否支持符号链接 |
| 7 | named_attr | 该对象是否有命名属性 |
| 8 | fsid | 文件系统标识 {major, minor} |
| 9 | unique_handles | 句柄是否与对象一一对应 |
| 10 | lease_time | **租约时长（秒）**——客户端必须读它来定续租节奏 |
| 11 | rdattr_error | READDIR 中单项属性获取失败时的错误码 |
| 19 | filehandle | 句柄本身（READDIR 里当属性取，等价 v3 READDIRPLUS 的句柄） |

### 常用推荐（RECOMMENDED）属性

| # | 属性 | 说明 |
|---|------|------|
| 12/13 | acl / aclsupport | NFSv4 ACL（见 3.6） |
| 15 | cansettime | 是否支持 SET_TO_CLIENT_TIME |
| 16/17 | case_insensitive / case_preserving | 大小写语义 |
| 20 | fileid | inode 号 |
| 21–23 | files_avail/free/total | inode 统计（v3 FSSTAT） |
| 24 | fs_locations | 迁移/复制位置（NFS4ERR_MOVED 配套） |
| 27–31 | maxfilesize/maxlink/maxname/maxread/maxwrite | 上限（v3 FSINFO/PATHCONF） |
| 33 | mode | UNIX 权限位 |
| 36/37 | **owner / owner_group** | **`user@domain` 字符串**（见 3.5） |
| 41 | rawdev | 设备号 |
| 42–45 | space_avail/free/total/used | 空间统计（v3 FSSTAT） |
| 47 | time_access（48 …_set） | atime（set 变体仅 SETATTR 用） |
| 51 | time_delta | 时间戳精度（v3 FSINFO） |
| 52 | time_metadata | ctime |
| 53 | time_modify（54 …_set） | mtime |
| 55 | mounted_on_fileid | 挂载点交界处"下面那个目录"的 fileid（客户端 readdir 需要） |

4.1 增补：dir_notif_delay、layout 系列（62 layout_type、64 layout_blksize 等）、mdsthreshold(68)、retention 系列、suppattr_exclcreat(75)。4.2 增补：clone_blksize(77)、space_freed(78)、change_attr_type(79)、sec_label(80)、（RFC 8275）mode_umask(81)、（RFC 8898）xattr_support(82)。

实现最小集参考：强制 13 个 + mode/owner/owner_group/fileid/rawdev/space_*/time_*/maxread/maxwrite/mounted_on_fileid ——这是 Linux 客户端正常工作实际依赖的集合。

## 3.4 change 属性：v4 缓存一致性的基石

```c
typedef uint64_t changeid4;
```

- 语义：对象内容或元数据**每次变化后必须不同**；只需"变了就不同"，不要求可读出含义。客户端缓存重验只比较 change 是否变化，彻底摆脱 v3 依赖 mtime 粒度的问题（见 nfsv3 分册 7.3）。
- 实现来源（优劣递减）：文件系统原生变更计数（ext4/xfs 的 i_version，需挂载开启）> ctime 纳秒拼接 > 自维护计数器。**用秒级 ctime 凑 change 会复现 v3 的漏检 bug**。
- 4.2 的 `change_attr_type` 属性可声明其单调性类别（单调递增/仅保证不同/…），帮助客户端做更强推理。
- WRITE/SETATTR 等修改操作的结果里带 `change_info4 { atomic; before; after; }`（目录操作对目录），语义与 v3 的 WCC 相同：atomic=TRUE 且 before 匹配缓存 ⇒ 缓存可无缝推进。

## 3.5 身份表示：owner@domain 字符串

v4 的 owner/owner_group 不是 uid/gid 数字，而是 UTF-8 字符串：`"alice@example.com"`、`"staff@example.com"`。

- 动机：跨管理域时数字 uid 无意义；配合 Kerberos principal 天然对齐。
- 双方靠 **idmap 服务**（Linux: nfsidmap/rpc.idmapd，配置 `/etc/idmapd.conf` 的 Domain）做 名字↔本地 uid 映射。**域名不一致是 v4 部署第一大坑**：映射失败时文件属主显示为 nobody/4294967294。
- 妥协惯例：AUTH_SYS 场景下允许纯数字字符串 `"1000"`（RFC 7530 §5.9 允许服务器接受；Linux 客户端 `nfs4_disable_idmapping=1` 默认走此路径）。**lightnfs 建议：AUTH_SYS 下直接用数字字符串**，省掉 idmap 依赖；做 Kerberos 时再上真映射。
- 注意鉴权用的仍是 RPC 凭证（AUTH_SYS 的 uid/gid 或 GSS principal）；owner 字符串只是**属性的表示形式**。

## 3.6 ACL（可选但规范完整）

NFSv4 ACL 是 Windows NT 风格的 ACE 列表，表达力超过 POSIX ACL：

```c
struct nfsace4 {
    acetype4 type;        /* ALLOW / DENY / AUDIT / ALARM */
    aceflag4 flag;        /* 继承标志：FILE_INHERIT/DIRECTORY_INHERIT/…，组标志 IDENTIFIER_GROUP */
    acemask4 access_mask; /* READ_DATA/WRITE_DATA/APPEND_DATA/EXECUTE/DELETE/
                             READ_ACL/WRITE_ACL/WRITE_OWNER/SYNCHRONIZE/… 共 17 位 */
    utf8str_mixed who;    /* user@domain 或特殊主体 OWNER@ / GROUP@ / EVERYONE@ */
};
```

- ACE 顺序敏感（首个匹配的 ALLOW/DENY 决定该位）。
- 与 mode 的联动规则（RFC 7530 §6.4）繁琐：改 mode 要重写 OWNER@/GROUP@/EVERYONE@ 对应 ACE，改 ACL 要折算 mode。
- `aclsupport` 属性声明支持哪些 ACE 类型；**完全不支持 ACL 是合法的**（不置 supported_attrs 第 12 位），Linux 客户端会自动退回 mode 位。4.1 还定义了 dacl/sacl（属性 58/59），实践少见。
- lightnfs 建议：第一阶段不支持 ACL，只做 mode——完全合规。

## 3.7 命名属性（named attributes / OPENATTR）

每个对象可挂一个隐藏"属性目录"（OPENATTR 进入），内为若干命名属性文件（类型 NF4NAMEDATTR），可 READ/WRITE。约等于 xattr 的重型版。实践中几乎无人用（Linux 客户端不支持）；4.2 之后 RFC 8276 定义了真正的 xattr 操作（GETXATTR/SETXATTR/LISTXATTRS/REMOVEXATTR，操作号 72–75）。lightnfs 可安全忽略两者（OPENATTR 返回 NFS4ERR_NOTSUPP）。

## 3.8 文件名与国际化

- 文件名/路径类型是 `utf8str_cs`（大小写敏感 UTF-8）。RFC 3530 曾强制服务器校验 UTF-8（违规回 NFS4ERR_INVAL）；RFC 7530 放宽为"SHOULD"，承认大量部署把名字当字节串。Linux 服务器默认不校验。
- `.`/`..` 不允许作为 LOOKUP/CREATE 的名字（NFS4ERR_BADNAME）——父目录导航用专门的 LOOKUPP。
- 大小写不敏感 fs 通过 case_insensitive/case_preserving 属性声明。
