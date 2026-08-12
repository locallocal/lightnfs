# 7. pNFS（并行 NFS，4.1 可选特性）

## 7.1 动机与角色划分

单台 NFS 服务器的网卡/磁盘是聚合带宽瓶颈。pNFS 把**元数据面与数据面分离**：

```
            ┌───────────────┐
   元数据   │  MDS 元数据服务器 │   ← 常规 NFSv4.1 协议（OPEN/GETATTR/…）
   ┌───────▶│  (状态、布局授予) │
   │        └───────┬───────┘
客户端               │ 控制协议（规范外，实现自定）
   │        ┌───────┴──────────────┐
   └───────▶│ DS 数据服务器 × N     │   ← 布局指定的数据协议直连并行读写
    数据    │ (仅 READ/WRITE/COMMIT)│
            └──────────────────────┘
```

客户端从 MDS 取得**布局（layout）**——"这个文件的字节范围 X 在哪些 DS 上如何分布"——然后绕过 MDS 直连 DS 并行 IO。聚合带宽随 DS 数量水平扩展。

## 7.2 布局类型

布局类型决定客户端↔DS 的数据协议，各自独立 RFC：

| layout type | 规范 | 数据协议 | 现状 |
|-------------|------|----------|------|
| LAYOUT4_NFSV4_1_FILES (1) | RFC 8881 §13 | NFSv4.1（DS 也是 NFS 服务器） | 经典型 |
| LAYOUT4_OSD2_OBJECTS (2) | RFC 5664 | OSD 对象协议 | 已死 |
| LAYOUT4_BLOCK_VOLUME (3) | RFC 5663 | 客户端直连 SAN 块设备 | 小众（+SCSI 变体 RFC 8154） |
| LAYOUT4_FLEX_FILES (4) | RFC 8435 | NFSv3 **或** v4.x 的 DS | **当代主流**（NetApp/Hammerspace 等） |

Flexible Files 值得单点注意：DS 可以是**普通 NFSv3 服务器**，MDS 给客户端发 per-DS 的合成凭证；还内建客户端侧 mirror。它让"一堆现成 NFS 盒子 + 一个调度 MDS"就能拼出 pNFS 集群，也是 Linux 服务器端唯一实现过的类型（knfsd 有实验性 flexfiles/block 导出）。

## 7.3 协议操作

| 操作 | 作用 |
|------|------|
| GETDEVICEINFO | deviceid → DS 地址列表等设备信息（客户端缓存，CB_NOTIFY_DEVICEID 失效） |
| LAYOUTGET | 取布局：(fh, 范围, iomode READ/RW) → layout（含 deviceid、条带参数、stateid） |
| LAYOUTRETURN | 归还布局（文件/fsid/全部三种粒度） |
| LAYOUTCOMMIT | 向 MDS 提交"我经由布局写到了 offset X"（更新可见 size/mtime） |
| CB_LAYOUTRECALL | MDS 召回布局（冲突/重平衡时） |
| (4.2) LAYOUTERROR / LAYOUTSTATS | 客户端上报 DS 的 IO 错误 / 统计 |

要点：

- **布局是状态**：有自己的 layout stateid，可被召回（流程与委托召回同构：CB_LAYOUTRECALL → 客户端 flush → LAYOUTRETURN）。
- **iomode**：READ 布局可多客户端共享；RW 布局涉及写序列化，由布局类型定义冲突规则。
- **LAYOUTCOMMIT 的必要性**：客户端绕过 MDS 写 DS，MDS 不知道文件长大了；LAYOUTCOMMIT 把 EOF/时间戳同步回 MDS（files 布局中 DS 与 MDS 若共享后端文件系统则可豁免部分）。
- **fallback 义务**：客户端任何时候都可以放弃 pNFS 走 MDS 常规 READ/WRITE（MDS 必须支持全量 IO）。DS 挂了客户端也这么退化。因此 pNFS 是纯性能层，正确性锚点始终在 MDS。
- 条带化（files/flexfiles）：`stripe_unit`、DS 列表、first_stripe_index 决定 offset→DS 映射，客户端按此切分 IO。

## 7.4 lightnfs 结论

pNFS 是"分布式存储系统"级别的工程（MDS-DS 一致性、召回风暴、DS 故障语义），与轻量目标相悖：

- **不实现**：EXCHANGE_ID 应答置 EXCHGID4_FLAG_USE_NON_PNFS，layout 类操作返回 NFS4ERR_NOTSUPP，layout_type 属性不置——客户端完全不会尝试 pNFS。零成本合规。
- 若未来要横向扩展，flexfiles + 现成 NFSv3 DS（甚至复用 lightnfs 自身的 v3 实现当 DS）是唯一现实路线。
