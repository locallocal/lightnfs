# 10. NFSv4 错误码

`nfsstat4`。v3 的错误码基本原样继承（值相同），v4 新增了大量状态/会话类错误。COMPOUND 语义下，错误码属于**单个操作**；整体 status = 最后执行的操作的错误（见 02 分册 2.2）。

## 10.1 从 v3 继承（值不变）

PERM=1, NOENT=2, IO=5, NXIO=6, **ACCESS=13**（v4 改名，v3 叫 ACCES）, EXIST=17, XDEV=18, NOTDIR=20, ISDIR=21, INVAL=22, FBIG=27, NOSPC=28, ROFS=30, MLINK=31, NAMETOOLONG=63, NOTEMPTY=66, DQUOT=69, STALE=70, BADHANDLE=10001, BAD_COOKIE=10003, NOTSUPP=10004, TOOSMALL=10005, SERVERFAULT=10006, BADTYPE=10007, **DELAY=10008**（即 v3 的 JUKEBOX，v4 用途扩大：委托召回中、资源暂缺等"稍后重试"都用它）。

注意消失的：NOT_SYNC（v3 SETATTR guard 专用，v4 用 change_info/VERIFY 替代）、BAD_COOKIE 保留但 cookieverf 在 v4 弱化。

## 10.2 v4.0 新增——按类别

### 条件执行 / COMPOUND 结构

| 值 | 名称 | 含义 |
|----|------|------|
| 10009 | SAME | NVERIFY：属性相同（中止后续） |
| 10027 | NOT_SAME | VERIFY：属性不同（中止后续） |
| 10020 | NOFILEHANDLE | 操作需要 CFH/SFH 而它为空 |
| 10030 | RESTOREFH | RESTOREFH 时无 SFH |
| 10021 | MINOR_VERS_MISMATCH | 小版本不支持（整个 COMPOUND 拒绝） |
| 10044 | OP_ILLEGAL | 非法操作码 |
| 10036 | BADXDR | 参数 XDR 解码失败（操作级；RPC 级仍可 GARBAGE_ARGS） |
| 10018 | RESOURCE | （4.0）COMPOUND 太长等资源不足；4.1 被更精细的错误取代 |

### 状态 / 锁 / 打开

| 值 | 名称 | 含义 / 客户端反应 |
|----|------|------------------|
| 10010 | DENIED | LOCK/LOCKT 冲突（带冲突者信息）→ 轮询或等通知 |
| 10011 | EXPIRED | 租约已过期且状态被回收 → 应用层报错 |
| 10012 | LOCKED | READ/WRITE 撞上别人的强制锁/冲突锁 |
| 10013 | GRACE | 宽限期内拒绝非 reclaim 操作 → 等待重试 |
| 10015 | SHARE_DENIED | OPEN 与既有 share deny 冲突 |
| 10022 | STALE_CLIENTID | clientid 无效（服务器重启）→ 重建 clientid |
| 10023 | STALE_STATEID | stateid 来自重启前 → 走恢复 |
| 10024 | OLD_STATEID | stateid 的 seqid 过旧 → 用最新 stateid 重试 |
| 10025 | BAD_STATEID | stateid 不存在/不匹配 |
| 10026 | BAD_SEQID | （4.0）owner seqid 乱序 |
| 10028 | LOCK_RANGE | 解锁/升级区间与持有区间不符（不支持拆分的服务器） |
| 10033 | NO_GRACE | 宽限期外的 reclaim |
| 10034 | RECLAIM_BAD | reclaim 声明与服务器记录矛盾 |
| 10035 | RECLAIM_CONFLICT | reclaim 与已授予的新状态冲突 |
| 10037 | LOCKS_HELD | CLOSE 时还有锁未释放（客户端应先 LOCKU） |
| 10038 | OPENMODE | 用只读 stateid 写之类的模式错配 |
| 10045 | DEADLOCK | 服务器检测到死锁（阻塞锁） |
| 10046 | FILE_OPEN | 操作被"文件正被打开"阻止（如某些平台的 REMOVE） |
| 10047 | ADMIN_REVOKED | 状态被管理员/超时吊销 |
| 10048 | CB_PATH_DOWN | （4.0）回调不可达（RENEW 时告知） |

### 命名 / 属性 / 安全

| 值 | 名称 | 含义 |
|----|------|------|
| 10014 | FHEXPIRED | 易失句柄过期 → 按缓存路径重查 |
| 10016 | WRONGSEC | flavor 不被该对象接受 → SECINFO 协商 |
| 10017 | CLID_INUSE | 客户端 id 字符串被别的主体占用（凭证不同） |
| 10019 | MOVED | 文件系统已迁移 → 查 fs_locations 跟走 |
| 10029 | SYMLINK | LOOKUP/OPEN 撞到符号链接（客户端自行解析；对比 v3 直接返回内容的思路） |
| 10031 | LEASE_MOVED | （4.0）部分状态随 fs 迁移走了 |
| 10032 | ATTRNOTSUPP | SETATTR 不支持的属性 |
| 10039 | BADOWNER | owner@domain 字符串无法映射 |
| 10040 | BADCHAR | 名字含非法字符（UTF-8 校验） |
| 10041 | BADNAME | 名字非法（如 "."/".."） |
| 10042 | BAD_RANGE | LOCK 区间非法（offset+length 溢出等） |
| 10043 | LOCK_NOTSUPP | 不支持请求的锁升降级 |

## 10.3 v4.1 新增（会话 / pNFS）

| 值 | 名称 | 含义 |
|----|------|------|
| 10052 | BADSESSION | sessionid 无效 → CREATE_SESSION 重建 |
| 10053 | BADSLOT | slotid 越界 |
| 10063 | SEQ_MISORDERED | 槽 seqid 既非重放也非+1 |
| 10064 | SEQUENCE_POS | SEQUENCE 不在第一个位置 |
| 10071 | OP_NOT_IN_SESSION | 需要会话的操作没跟在 SEQUENCE 后 |
| 10081 | NOT_ONLY_OP | EXCHANGE_ID 等必须单独成 COMPOUND 却带了别的操作 |
| 10065/10066 | REQ_TOO_BIG / REP_TOO_BIG | 超出会话协商的请求/应答大小 |
| 10067 | REP_TOO_BIG_TO_CACHE | 应答太大没法缓存而客户端要求 cachethis |
| 10068 | RETRY_UNCACHED_REP | 重放命中未缓存的槽 |
| 10070 | TOO_MANY_OPS | 超出 ca_maxoperations |
| 10054 | COMPLETE_ALREADY | 重复 RECLAIM_COMPLETE |
| 10055 | CONN_NOT_BOUND_TO_SESSION | 连接未绑定会话 |
| 10074 | CLIENTID_BUSY | DESTROY_CLIENTID 时还有会话/状态 |
| 10076 | SEQ_FALSE_RETRY | 假重放（seqid 相同但请求内容不同） |
| 10077 | BAD_HIGH_SLOT | highest_slotid 非法 |
| 10078 | DEADSESSION | 会话已宣告死亡（不可再用） |
| 10087 | DELEG_REVOKED | 委托已被吊销 |
| 10058–10062, 10075, 10080, 10086 | LAYOUTTRYLATER / LAYOUTUNAVAILABLE / NOMATCHING_LAYOUT / RECALLCONFLICT / UNKNOWN_LAYOUTTYPE / PNFS_IO_HOLE / PNFS_NO_LAYOUT / RETURNCONFLICT | pNFS 布局类（不做 pNFS 用不到） |
| 10083 | WRONG_TYPE | 操作用在错误对象类型上（4.1 起的通用型错） |
| 10082 | WRONG_CRED | 状态操作凭证与建立时不符（state protection） |

## 10.4 v4.2 新增

| 值 | 名称 | 含义 |
|----|------|------|
| 10088 | PARTNER_NOTSUPP | 跨服拷贝：对端不支持 |
| 10089 | PARTNER_NO_AUTH | 跨服拷贝：对端拒绝授权 |
| 10090 | UNION_NOTSUPP | READ_PLUS 等的联合类型不支持 |
| 10091 | OFFLOAD_DENIED | 异步拷贝被拒 |
| 10092 | WRONG_LFS | 安全标签格式（LFS）不支持 |
| 10093 | BADLABEL | 标签非法 |
| 10094 | OFFLOAD_NO_REQS | COPY 要求（consecutive/sync）无法满足 |
| 10095/10096 | NOXATTR / XATTR2BIG | （RFC 8276）xattr 不存在 / 太大 |

## 10.5 客户端恢复语义速查（服务器要配合的行为）

| 错误 | 客户端标准反应 | 服务器义务 |
|------|---------------|-----------|
| DELAY | 指数退避重试同一请求 | 只在"稍后真会好"时用；召回进行中的 OPEN 用它 |
| GRACE | 等待后重试 | 宽限期结束后放行 |
| OLD_STATEID | 取最新 seqid 重试 | 保证 stateid seqid 单调 |
| BAD/STALE_STATEID、STALE_CLIENTID、BADSESSION、EXPIRED | 逐级重建（状态→clientid→会话）| 错误分类必须准确——**乱返错会把客户端带进错误的恢复路径**，表现为挂载卡死或死循环恢复，是 v4 服务器最难调的 bug 类型 |
| WRONGSEC | SECINFO 换 flavor | SECINFO 必须可达 |
| MOVED | 查 fs_locations | 不做迁移就永远别返回它 |

实现纪律与 v3 相同的部分（错误映射兜底 SERVERFAULT、不泄露信息等）见 nfsv3 分册 08；v4 额外的纪律：**每个操作只能返回其规范表格允许的错误码**（RFC 8881 §15.2 有完整的 per-op 错误表，实现时对照），客户端对表外错误的行为未定义。
