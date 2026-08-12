# 5. MOUNT 协议（版本 3）

程序号 **100005**，版本 3（配 NFSv3；版本 1 配 NFSv2）。定义于 RFC 1813 附录 I。端口动态（注册到 rpcbind），实践中常固定（如 20048）以便防火墙放行。

## 5.1 为什么需要 MOUNT

NFS 的一切访问都从文件句柄出发，但句柄是不透明的——客户端最初的"第一个句柄"从哪来？MOUNT 协议就是**从路径名换取导出点根句柄**的自举机制，同时承担导出列表查询和（名义上的）挂载记录。

NFSv4 取消了 MOUNT（用 well-known 的 root 句柄 + 伪文件系统），这是 v3 与 v4 部署差异的主要来源之一。

## 5.2 数据类型

```c
#define MNTPATHLEN 1024   /* 路径最大长度 */
#define MNTNAMLEN   255   /* 主机名最大长度 */
#define FHSIZE3      64   /* 与 NFS3_FHSIZE 一致 */

typedef opaque fhandle3<FHSIZE3>;
typedef string dirpath<MNTPATHLEN>;
typedef string name<MNTNAMLEN>;

enum mountstat3 {
    MNT3_OK = 0,
    MNT3ERR_PERM = 1,           /* 非 root（保留端口策略等） */
    MNT3ERR_NOENT = 2,          /* 路径不存在 */
    MNT3ERR_IO = 5,
    MNT3ERR_ACCES = 13,         /* 该客户端无权挂载此导出 */
    MNT3ERR_NOTDIR = 20,
    MNT3ERR_INVAL = 22,
    MNT3ERR_NAMETOOLONG = 63,
    MNT3ERR_NOTSUPP = 10004,
    MNT3ERR_SERVERFAULT = 10006
};
```

## 5.3 过程

| # | 过程 | 参数 | 结果 | 说明 |
|---|------|------|------|------|
| 0 | NULL | void | void | 探活 |
| 1 | MNT | dirpath | mountres3 | **核心**：路径 → 根句柄 |
| 2 | DUMP | void | mountlist | 已挂载记录列表（showmount -a） |
| 3 | UMNT | dirpath | void | 移除一条挂载记录 |
| 4 | UMNTALL | void | void | 移除该客户端全部记录 |
| 5 | EXPORT | void | exports | 导出列表（showmount -e） |

### MNT（过程 1）

```c
union mountres3 switch (mountstat3 fhs_status) {
    case MNT3_OK:
        struct mountres3_ok {
            fhandle3 fhandle;
            int      auth_flavors<>;   /* 服务器接受的认证方式列表 */
        };
    default: void;
};
```

- 服务器校验：路径是否在导出表中、请求方 IP 是否被允许、（可选）源端口是否为保留端口（<1024，"secure" 选项）。
- `auth_flavors` 告诉客户端后续 NFS 请求可用的 flavor（如 [AUTH_SYS] 或 [RPCSEC_GSS_KRB5, AUTH_SYS]）。至少返回一个；AUTH_SYS=1。
- 成功后（名义上）把 (客户端主机名, 路径) 记入 rmtab 供 DUMP 查询。**这份记录纯属礼貌性质**：客户端崩溃不会发 UMNT，记录天然不可靠，任何逻辑都不应依赖它。lightnfs 可将 DUMP 实现为返回空表。

### EXPORT（过程 5）

```c
struct exportnode { dirpath ex_dir; groups ex_groups; exportnode *ex_next; };
struct groupnode  { name gr_name; groupnode *gr_next; };
```

返回导出路径及允许的客户端组（字符串形式，如 "*.example.com"、"192.168.0.0/24"）。`showmount -e` 用它。信息披露考量：可以按请求方过滤或返回空组列表。

## 5.4 挂载安全模型与陷阱

1. **MOUNT 检查 ≠ NFS 检查**。传统实现中导出授权只发生在 MNT；NFS 服务对拿着有效句柄的请求不再验证"你是否被允许访问这个导出"。句柄可被嗅探、猜测或离线构造（如果句柄编码可预测）。健壮实现应在 **NFS 层对每个请求做导出级校验**（检查源 IP 是否在该 fsid 的允许列表内），Linux nfsd 就是这样做的。
2. **子目录导出漏洞**：导出 `/export/sub` 而句柄只编码 inode 时，客户端可通过猜句柄或 `..` 遍历访问 `/export` 其他部分（v3 的 LOOKUP ".." 在导出根应被拦住，Linux 有 subtree_check 选项及其著名的 rename 兼容性问题）。**最省心的策略：只按文件系统根导出**。
3. **保留端口检查**（insecure/secure 选项）：只信任源端口 <1024 的请求——在容器和非 UNIX 客户端普及的今天意义有限，可做成开关。
4. UMNT/UMNTALL 无鉴权，任何人可清别人的记录——又一个"rmtab 不可信"的理由。

## 5.5 lightnfs 实现建议

- MNT、EXPORT、NULL 必须实现；DUMP/UMNT/UMNTALL 可实现为兼容性空操作。
- 导出表建议单一配置源（路径 → fsid、允许网段、squash 策略、只读标志），MOUNT 与 NFS 层共享它。
- Linux mount.nfs 在 `vers=3` 时的完整流程依赖 rpcbind 查 MOUNT 端口；若想免 rpcbind，可支持 `mount -o port=2049,mountport=XXXX` 显式指定。
