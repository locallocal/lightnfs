# 9. NFSv4 安全机制

## 9.1 规范要求 vs 部署现实

RFC 要求：v4 实现**必须实现** RPCSEC_GSS（Kerberos 5 三件套 krb5/krb5i/krb5p）；AUTH_SYS 只是"可以用"。
部署现实：绝大多数 v4 部署仍在跑 **AUTH_SYS**（Kerberos 的 KDC/keytab/时钟同步运维成本高）。方向性补救是 RPC-over-TLS（RFC 9289，`xprtsec=tls` 挂载选项，Linux 6.x 双端已支持）：传输层加密+服务器认证，AUTH_SYS 继续做用户声明——"TLS 保通道、AUTH_SYS 报身份"正在成为新的现实基线。

lightnfs 定位建议与 v3 相同：AUTH_NONE/AUTH_SYS 起步，声明仅限受信网络；架构上给 flavor 留插槽。

**RPC-over-TLS（RFC 9289）已内置**（plan doc 10 §5.4、09 册长期观察项）：客户端 `xprtsec=tls`
触发 AUTH_TLS 探测 → 服务器 STARTTLS 应答 → 同连接 TLS 会话；传输层加密 + 服务器证书认证，
AUTH_SYS 继续报身份。故不再必须前置 stunnel/haproxy（前置仍是可选过渡）。仍不做 RPCSEC_GSS/krb5。

## 9.2 RPCSEC_GSS 要点（实现时再展开）

- flavor=6，上下文建立（RPCSEC_GSS_INIT/CONTINUE_INIT）走 NULL 过程携带 token 往返；数据阶段每请求带 GSS 头（上下文句柄 + gss seq 号防重放），服务级别：
  - **krb5**：仅认证（头部 MIC）；
  - **krb5i**：完整性（参数/结果整体 MIC）；
  - **krb5p**：隐私（参数/结果加密）。
- GSS 主体（`nfs/server.example.com@REALM` 服务主体，用户主体 `alice@REALM`）与 owner@domain 属性天然对齐（见 03 分册 3.5）。
- RPCSEC_GSS v3（RFC 7861）增加了结构化特权/标签断言，为 4.2 sec_label 与跨服拷贝服务；主流部署仍是 v1。

## 9.3 SECINFO / SECINFO_NO_NAME：按导出协商安全

v4 单一命名空间下不同子树可要求不同安全级别，客户端跨越边界时询问：

```
SECINFO(CFH=父目录, name)      → 该名字对象接受的 flavor 列表（含 GSS OID/QOP/服务三元组），按服务器偏好排序
SECINFO_NO_NAME(CFH, style)    → (4.1) 对 CFH 本身查询；style=CURRENT_FH 或 PARENT
```

触发点：客户端收到 **NFS4ERR_WRONGSEC** 时（用错 flavor 访问）→ SECINFO 问明白 → 换凭证重试。服务器实现：伪 fs 与各导出维护 flavor 列表；PUTROOTFH/伪 fs 遍历必须允许任意 flavor（否则客户端进不了门查询）。AUTH_SYS-only 的服务器：SECINFO 恒返回 `[AUTH_SYS]`，永不发 WRONGSEC——简单合规。

## 9.4 身份与授权的分层

| 层 | 载体 | 服务器动作 |
|----|------|-----------|
| RPC 凭证 | AUTH_SYS uid/gid 或 GSS principal | squash 映射 → 得到"操作身份" |
| 授权检查 | 操作身份 × mode/ACL | 每操作执行（ACCESS 供客户端预查询） |
| 属性表示 | owner/owner_group 字符串 | idmap（或数字字符串直通） |
| 导出控制 | 源 IP/网段 × 导出表 | 连接/请求级校验（v3 分册 9.6 同样适用） |

与 v3 相同的红线：**句柄即能力**。v4 虽然多了 OPEN 状态，但 PUTFH+READ(匿名 stateid) 这类无 OPEN 路径仍在，NFS 层的每请求导出校验、句柄防伪（HMAC）依然必要（参见 nfsv3 分册 9.6）。

## 9.5 v4 特有的安全注意点

1. **状态操作的冒充**：恶意方伪造 clientid/stateid 关闭别人的文件、DESTROY_SESSION。AUTH_SYS 下无解（凭证可伪造），SP4_MACH_CRED/SSV 是规范给的答案（见 06 分册 6.5）；现实缓解 = 受信网络 + TLS。
2. **宽限期 reclaim 劫持**：见 04 分册 4.7——必须有稳定存储客户端名单。
3. **回调通道认证**（4.0）：服务器连回客户端时的凭证约定含糊，是 4.0 的历史烂账；4.1 回传通道复用会话安全参数（CREATE_SESSION 的 sec_parms），干净得多。
4. **拒绝服务面**：COMPOUND 允许一次请求打包大量操作 → 限制 ca_maxoperations/ca_maxrequestsize（4.1 天然有协商，4.0 要自设上限 NFS4ERR_RESOURCE）；槽表数量限制并发；READDIR/GETATTR 的 bitmap 解析注意越界。
5. **UTF-8 解析**：名字/owner 字符串解析器是攻击面，长度上限 + 无 NUL/`/` 校验不可省。
