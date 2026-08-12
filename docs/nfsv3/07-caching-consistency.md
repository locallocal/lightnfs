# 7. 缓存与一致性模型

NFSv3 **不提供跨客户端的强一致性**。协议只给出机制（属性、WCC、verifier），一致性语义靠客户端惯例（close-to-open）拼装。理解这一层对实现服务器同样关键——服务器返回的每一份属性都在喂客户端的缓存决策。

## 7.1 客户端缓存什么

| 缓存 | 内容 | 失效依据 |
|------|------|----------|
| 属性缓存 | fattr3，TTL 典型 3–60s（acregmin/acregmax，目录 acdirmin/acdirmax） | TTL 到期后 GETATTR 重验 |
| 数据缓存（页缓存） | 文件内容 | 属性重验时发现 mtime/size/ctime 变化 → 作废全部页 |
| 目录项缓存（dcache） | name → 句柄 | 目录 mtime 变化 |
| ACCESS 缓存 | 每 (对象, 凭证) 的权限位 | TTL |
| 脏页 | UNSTABLE 写出前/COMMIT 前的数据 | verifier 变化 → 重发 |

## 7.2 close-to-open（CTO）一致性

这是 NFS 世界的核心契约，客户端惯例而非协议条文：

- **open 时**：向服务器重验属性（GETATTR），发现 mtime/ctime/size 与缓存不符 → 作废本地数据缓存。
- **close 时**：把全部脏数据 flush 到服务器（WRITE + COMMIT 成功才算 close 成功）。

保证：A 客户端 close 之后，B 客户端再 open 能看到 A 的全部写入。**不保证**：两个客户端同时打开同一文件的读写一致（需要 NLM 锁 + O_DIRECT 或 noac 才能凑合）。

对服务器的要求：**mtime/ctime/size 必须诚实且及时**。若服务器缓存属性返回了陈旧 mtime，CTO 就会漏判，客户端读到旧数据——这类 bug 极难排查。

## 7.3 属性时间粒度陷阱

CTO 靠 mtime 比较。若文件系统时间戳粒度粗（如 1 秒），同一秒内的"写→读"竞争会漏检。v3 用 FSINFO 的 `time_delta` 声明粒度；服务器应尽量提供纳秒级 mtime（现代文件系统都支持）。同理，服务器**不要自作聪明地缓存/延迟更新 mtime**。

## 7.4 WCC（弱缓存一致性）如何工作

修改类操作返回 `wcc_data { before(size,mtime,ctime), after(fattr3) }`：

```
客户端缓存的属性 == before  ⇒  两次观测之间无第三方修改，
                              客户端可将缓存直接推进到 after（缓存保活）
客户端缓存的属性 != before  ⇒  有并发修改，作废缓存
```

服务器实现要点：

- before/after 必须在**同一次操作内原子采样**（操作前取 size/mtime/ctime，操作后取全量属性，中间不能放进其他请求的修改）。做不到原子就返回 `attributes_follow=FALSE`——宁缺毋滥，错误的 WCC 会让客户端错误地保活脏缓存。
- 并发服务器上这意味着 per-file 或 per-directory 的操作串行化点（或至少属性采样与修改在同一临界区内）。

## 7.5 写路径：UNSTABLE / COMMIT / verifier 全流程

```
客户端                                     服务器
  write() 进本地页缓存（立即返回给应用）
  ...页回写触发...
  WRITE(off=0,   64K, UNSTABLE) ──────────▶ 进服务器页缓存
  ◀── committed=UNSTABLE, verf=V1
  WRITE(off=64K, 64K, UNSTABLE) ──────────▶
  ◀── committed=UNSTABLE, verf=V1
  ...close()/fsync()...
  COMMIT(0, 0) ───────────────────────────▶ fsync
  ◀── verf=V1   （与写时一致 ⇒ 数据安全，客户端丢弃脏页）
```

崩溃场景：

```
  WRITE(..., UNSTABLE) ◀── verf=V1
  【服务器崩溃重启，V1 期间未落盘数据丢失】
  COMMIT(0,0) ◀── verf=V2 ≠ V1
  ⇒ 客户端检测到重启，重发所有未 COMMIT 的 WRITE，再 COMMIT
```

服务器义务清单：

1. verifier 在**每次可能丢失未提交数据的重启后必须改变**；最简单：启动时间戳（秒+纳秒拼 8 字节）。整个服务器用一个全局 verf 即可。
2. 若服务器把每个 WRITE 都同步落盘（committed=FILE_SYNC），崩溃也不丢数据，verf 可以恒定不变——**全同步写是最简单的正确实现**，代价是写性能。
3. 返回 committed=FILE_SYNC 后客户端立即释放脏页，服务器**不得**事后又丢数据——committed 等级说到必须做到。

## 7.6 READDIR cookie 的一致性

目录遍历期间目录被修改（增删项）时：

- 已删除项对应的 cookie 再来续读——服务器应能容忍（跳到下一项），返回 NFS3ERR_BAD_COOKIE 会迫使客户端整目录重列。
- 遍历中新增/删除的项"看到或看不到"都合法（与本地 readdir 相同的弱保证），但**不得重复、不得漏掉未被修改的项**。
- 稳定 cookie 设计（如 hash 链位置、B 树键序号）优于"数组下标"设计（删除会位移导致漏项/重项）。

## 7.7 客户端行为对服务器负载的含义

- 属性缓存 TTL 到期 → GETATTR 风暴：GETATTR 必须是最快路径。
- `ls -l` → READDIRPLUS；不支持时退化为 READDIR + N×(LOOKUP+GETATTR)，RPC 数放大一到两个数量级。
- `noac`/`sync` 挂载、以及 O_DIRECT：每次操作都直达服务器，别假设客户端一定有缓存。
- 客户端预读：连续 READ 会被放大成并发的顺序预读请求，服务器按 offset 排序处理可提升机械盘吞吐（SSD 影响小）。

## 7.8 时钟问题

客户端会拿服务器返回的 mtime 与**自己的时钟**比较来估算"文件多新"（尤其 make 类工具）。服务器时钟漂移会造成编译系统的诡异行为。协议无时钟同步机制，运维上 NTP 是事实前提；实现上服务器只用自己的时钟、绝不用客户端时间（除 SET_TO_CLIENT_TIME 显式要求外）。
