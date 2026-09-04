# 5. 委托与回调

## 5.1 委托（delegation）解决什么问题

v3/无委托 v4 的客户端即使独占访问一个文件，也必须不断向服务器重验属性（close-to-open 语义）。委托是服务器对客户端的一个**可召回的授权**："在我收回之前，这个文件归你管"——客户端可以在本地完成 open/close/锁裁决/数据缓存，不与服务器往返。

- **读委托（OPEN_DELEGATE_READ）**：保证没有人在写。客户端可本地满足读打开、读锁、缓存数据不重验。可同时发给多个客户端。
- **写委托（OPEN_DELEGATE_WRITE）**：保证没有其他访问者。客户端可本地满足一切 open/lock，**脏数据可以只留在本地**（连 size/mtime 变化都不必立刻上报）。同一文件全局最多一个。

委托是**纯优化**：服务器可以永远不发（delegation=NONE 合法），客户端不能索求（4.1 的 WANT_DELEGATION 也只是"想要"）。发放策略完全由服务器启发式决定（典型：读打开且回调可用且近期无写冲突 → 给读委托；Linux knfsd 对写委托长期保守，近年版本才逐步启用）。

## 5.2 何时必须召回（recall）

服务器在出现**冲突访问**时召回委托：

| 已发委托 | 触发召回的操作（来自其他客户端或本地） |
|----------|--------------------------------------|
| 读委托 | OPEN(WRITE)、SETATTR(size)、WRITE、REMOVE/RENAME 该文件 |
| 写委托 | 任何 OPEN、READ、GETATTR(size/change 之外通过 CB_GETATTR 代答)、LOCK、REMOVE/RENAME |

召回流程：

```
其他客户端: OPEN(WRITE) ──▶ 服务器发现冲突
                             │ CB_COMPOUND{CB_RECALL(stateid, fh)} ──▶ 持委托客户端
                             │ 对冲突请求方返回 NFS4ERR_DELAY（4.0）
                             │ 或阻塞等待（实现自选）
持委托客户端：flush 脏数据(WRITE+COMMIT)、上报本地建立的 open/lock（CLAIM_DELEGATE_CUR）
             └─ DELEGRETURN ──▶ 服务器
服务器：完成冲突请求方的 OPEN
```

- 客户端不还怎么办：**召回超时（约一个租约）后服务器单方面吊销**（revoke），此后该委托 stateid 返回 NFS4ERR_ADMIN_REVOKED/DELEG_REVOKED。吊销可能造成客户端脏数据无处安放——所以宁可少发写委托。
- `DELEGPURGE`：客户端声明"我重启前的委托都不要了"（配 CLAIM_DELEGATE_PREV 使用，罕见）。
- 服务器资源紧张时也可主动召回（4.1 还有 CB_RECALL_ANY："随便还我 N 个"）。

## 5.3 CB_GETATTR：写委托下的属性代答

持写委托的客户端可能在本地积累了未上报的写入。其他客户端 GETATTR(size/change) 时，服务器不知道真实值，必须先向持委托者发 **CB_GETATTR** 询问其当前 size/change，用应答（或召回后的落盘值）回答询问者。这是"写委托 = 客户端暂管真相"的直接体现，也是实现写委托的主要麻烦。

## 5.4 回调通道

### 4.0：服务器反向连接（问题多）

- 客户端在 SETCLIENTID 里给出 `cb_client4{ cb_program, r_netid("tcp"), r_addr("h1.h2.h3.h4.p1.p2") }`。
- 服务器**主动 TCP 连接**该地址，发 CB_NULL 验证可达性；不可达 ⇒ 不发委托（协议其余部分照常）。
- NAT/防火墙后回调必然失败——这是 4.0 委托在真实网络中形同虚设的原因。
- 回调用的 RPC 程序号由客户端指定（cb_program，Linux 用 0x40000000 起的临时号）。

### 4.1：回传通道（backchannel，问题解决）

- CREATE_SESSION 时声明该连接同时用于回传（SESSION4_BACK_CHAN），服务器的 CB_COMPOUND **沿客户端发起的同一条 TCP 连接反向发送**——NAT 穿透天然成立。
- 回传通道有自己的槽表（CB_SEQUENCE 打头），EOS 语义与前向一致。
- 连接断了：客户端 BIND_CONN_TO_SESSION 绑新连接补上；服务器在回传不可用期间标记会话（SEQUENCE 应答 flags 置 CB_PATH_DOWN），客户端见旗即修。

## 5.5 目录委托（4.1，可选中的可选）

GET_DIR_DELEGATION 允许客户端缓存目录内容并由服务器用 CB_NOTIFY 推送变更（增删项）。Linux 客户端/服务器长期未实现或默认关闭。lightnfs 直接不支持（返回 NFS4ERR_NOTSUPP）即可。

## 5.6 实现策略建议

委托是可选项，且是 v4 实现中"做错代价 > 不做代价"的典型：

1. **第一阶段：不发任何委托**（OPEN 一律 delegation=NONE，不实现 CB 服务即可，4.1 会话仍需宣告回传通道但可永不使用）。完全合规，Linux 客户端毫无怨言。——v1 发布时的状态。
2. 第二阶段：只发**读委托**，实现 CB_RECALL + DELEGRETURN + 召回超时吊销。收益（元数据往返骤减）/复杂度比最高。——**已实现**（2026-08-28）：只对绑定了回传通道的会话授予，冲突操作 CB_RECALL + DELAY，租约期内不归还则吊销；`[protocol] delegations = false` 可关；回传通道另用于 CB_NOTIFY_LOCK。
3. 写委托 + CB_GETATTR：等有真实的单写者工作负载证据再做。——仍不做。
