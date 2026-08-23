# lightnfs v1 测试报告（交付物清单 §11）

汇总 v1（= 路线图阶段 5 末，另含阶段 6 第 1 项 v4.2 甜点，见
[design/09-roadmap.md](design/09-roadmap.md)）的测试证据：协议一致性
（cthon / pynfs / fsx）、三层基准数据与安全清单验收。逐阶段的实现过程文档已随发布收尾
移除（记录保留在 git 历史中）；本文即汇总结论与数据，更新日期 2026-08-23。

## 1. 测试环境

- **本机（无特权）**：回环 TCP 打真实 `lightnfsd`，客户端为自研用户态 NFSv3/v4.1 客户端
  `lnfs_accept_client`（协议级等价校验，全部数据逐字节比对后端目录）；pynfs 用户态直连。
- **CI VM（root）**：`m1..m6-acceptance` 作业在 runner VM 内真实 `mount -o vers=3/4.1/4.2`，
  跑 cthon04 / fsx / 内核 POSIX 字节锁；nightly 作业跑 24h fuzz 累计、fsx 过夜
  （FSX_OPS=200 万）与 pynfs 全量套件漂移比对。
- 构建矩阵：Debug / Release / ASAN+UBSAN / TSAN / epoll 兜底（Release）/ libFuzzer，
  GCC 15 与 clang 双工具链。

## 2. 单测 / 集成测试

| 阶段 | 用例数 | 配置 |
|------|--------|------|
| 阶段 0（runtime/XDR/传输） | 64 | 四配置全绿 |
| 阶段 2（v3 读写） | 86 | 四配置全绿 |
| 阶段 3（v4.1 只读） | 97 | 四配置全绿 |
| 阶段 4（v4.1 读写+状态） | 107 | 三配置全绿 |
| 阶段 5（锁+安全，v1 候选） | 111 | 三配置全绿 |
| 当前（2026-08-23，含 v4.2 与 errlog） | 118 | Debug/Release/ASAN/TSAN/epoll 全绿 |

覆盖要点：fake ring 时序注入（EINTR/短读/乱序/取消）、StateMgr 并发矩阵（分片锁死锁自由）、
DRC 字节级重放、EXCLUSIVE 重放、v4 wire 级 COMPOUND 纪律 / 槽重放 / stateid 家族 /
名字空间 op / 锁生命周期、错误映射白名单生成式对照（v3+v4）。

## 3. 协议一致性

### cthon04（真实 mount，CI VM）

- **vers=3**：basic / general / special 全量通过（M2，`accept_m2_vm.sh` → CI
  `m2-acceptance`）；M1 阶段先行的只读子集（test3/5b/9）另有 `cthon_ro.sh` 常态回归。
- **vers=4.1**：basic / general / special 通过（M4）；**lock 组** + 内核 fcntl 字节锁
  冲突/释放通过（M5，`accept_m5_vm.sh -l`）。
- **vers=4.2**：basic / general 回归通过（M8 第 1 项，`accept_m6_vm.sh`）。

### pynfs 4.1

| 里程碑 | 范围 | 结果 |
|--------|------|------|
| M5（阶段 3） | 会话五组 + 扩展组 | 67/75 + 37/44，失败全部为写依赖或需 root 的树对象 |
| M6（阶段 4） | open/rename/verify/courteous/currentstateid + 阶段 3 全部组 | **184 用例：162 通过，22 失败全部命中预期排除表**（`pynfs_m4_expected.txt`） |
| M7（阶段 5） | 锁 / secinfo / courtesy 组 | 26 通过 / 1 失败（CSID7 为 pynfs 自身 NameError） |
| M8（阶段 6.1） | + secinfo_no_name/SEC1/SEC2 等 | **186 用例：168 通过，18 失败全部命中预期排除表**（`pynfs_m5_expected.txt`：委托/回传属 M8 遗留、需 root 的设备节点、CSID7） |

预期排除表随 nightly `conformance-overnight` 作业做全量套件漂移比对（新失败即告警）。

### fsx（xfstests）

- 每 PR：5 万 ops（CI `m2-acceptance`/`m4-acceptance`）；本机 5000 ops 冒烟。
- 过夜：nightly 作业 FSX_OPS=200 万（vers=3 与 4.1），与 24h fuzz 同批。

### 回环端到端（本机，2026-08-23 全量复跑通过）

- **M2**：wtest 三稳定级写 + 逐字节校验、DRC 线上重传字节一致、kill -9 崩溃恢复
  （verifier 0x01→0x02，数据重发收敛）、万连接风暴 10000/10000 存活 + 单连接 512 深
  流水线背压、ASAN 120s 浸泡 768 万 ops 泄漏零报告。
- **M6**：v4.2（DEALLOCATE/SEEK/ALLOCATE 后端镜像、COPY 逐字节、CLONE 按文件系统、
  minor 1 下 OP_ILLEGAL）、v4rw/v4lock/v4walk/walk、重启 reclaim（CLAIM_PREVIOUS/
  GRACE/NO_GRACE 门禁）、courtesy 双路径回收、ctl 工具链，Release 与 ASAN 双配置。
- **fuzz**：libFuzzer 直喂 `handle_request` 60s 短跑无 crash（~10 万 exec/s），语料
  持续累积；24h 长跑随 nightly 累计。

## 4. 三层基准（02 分册 §2.8）

基线（`bench/baseline.txt`，单 reactor、4 连接、32 流水线、io_uring、本机 Release）：

| 层 | 基准 | 基线 rps | 近期实测（2026-08-23） |
|----|------|----------|------------------------|
| L1 传输 | echo（128B） | 375k | 361k–396k |
| L2 RPC | null-RPC | 350k | 338k |
| L4+ 全链路 | fullpath GETATTR（伪后端零延迟） | 345k | 308k–335k |
| L4+ 全链路 | fullpath READ-4k | 275k | 262k–291k |

- 阶段 0 出口指标（null-RPC 单 reactor ≥100k）达成时实测 ~398k（8 连接 ×64 流水线）。
- 阈值门禁：`scripts/bench_gate.sh` 每 PR 以基线 × `LNFS_BENCH_FLOOR`
  （本地 0.5 / CI 0.2）判定，防数量级退化；近期门禁全绿。

## 5. 安全清单验收（08 分册 §8.5）

全部 8 项已逐条验收并归档于 [security-checklist.md](security-checklist.md)：
长度上限+fuzz 全入口、句柄 HMAC+每请求导出/IP 校验、名字双层校验、squash 单点、
grace reclaim 名单强制、资源上限可配、最小特权+seccomp（`packaging/systemd/
lightnfs.service` + `gen_seccomp_allowlist.sh`）、AUTH_SYS 信任边界入部署文档；
另含错误白名单全覆盖复查记录。

## 6. 已知限制与豁免

- 无特权回退句柄模式在无 STATX_BTIME 的文件系统（如 tmpfs）上句柄不跨重启稳定
  （生产指引见 [deployment.md](deployment.md)）。
- pynfs 预期排除：委托/回传（roadmap M8 遗留）、需 root 的块/字符设备树对象、
  CSID7（pynfs 自身缺陷）。
- fsx ≥12h 过夜首轮记录与 24h fuzz 累计随 nightly 在 CI 环境执行（日历项）。
