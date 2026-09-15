# lightnfs 文档

项目说明见仓库根的 [README.md](../README.md)（English）与 [README.zh.md](README.zh.md)（中文）。
本目录按读者与用途分四类：

| 目录 | 是什么 | 什么时候看 |
|------|--------|-----------|
| [design/](design/README.md) | **设计文档**——lightnfs 自己怎么做的：分层、协程运行时、传输与 RPC、协议核心、后端接口与四个后端、状态管理、配置与可观测性，以及三册集群文档 | 读代码、改实现、评估某个决策为什么这么定 |
| [guide/](guide/) | **使用文档**——部署与运维，以及 `scripts/` 下每个脚本的作用 | 要把它装起来、跑起来、接进监控，或者不确定该跑哪个脚本 |
| [reference/](reference/) | **协议调研**——NFSv3 与 NFSv4 协议族的逐章梳理，只描述协议本身 | 想确认"RFC 到底怎么规定的" |
| [testing/](testing/) | **测试文档**——测试报告、基准工具使用指南、安全加固清单 | 想知道哪些东西被验证过、怎么复现 |

## design/ 设计文档

`01` 总体架构 · `02` 协程运行时与并发 · `03` 传输与 RPC/XDR · `04` 协议核心（v3/v4 双引擎）·
`05` 后端抽象接口 · `06` 后端实现 · `07` 状态管理 · `08` 配置、可观测性与安全 ·
`09` 多网关无感故障切换 · `10` 多网关多活 · `11` 共享导出清单。逐册摘要与关键决策表见
[design/README.md](design/README.md)。

[design/followups/](design/followups/) 是三册集群文档各自的**未闭环项**——已知的取舍、缺的
验证、发布前该补的门槛。改动相关代码前先扫一眼。

## guide/ 使用文档

- [deployment.md](guide/deployment.md)——安全信任边界（务必先读）、最小特权 systemd 部署、
  关键配置、运维与可观测性、多网关主备（§5）、多网关多活（§6）、共享导出清单（§6.1）、
  已知限制（§7）。
- [scripts.md](guide/scripts.md)——`scripts/` 速查：源码门禁、验收脚本（本机 / root VM /
  真实集群后端）、外部测试套件的获取、性能与健壮性跑批、两个运维脚本，以及它们与
  `make` / ctest / `ci.sh` 的关系。
- [posix-semantics.md](guide/posix-semantics.md)——POSIX 文件系统语义检查器
  `scripts/posix_semantics.py` 的用法：12 组检查各看什么、SKIP 的判据、NFS 上合法的偏差
  （silly-rename、时间戳粒度、v3 无字节锁）、与 cthon / fsx / pynfs 的分工。
- [fsperf.md](guide/fsperf.md)——文件系统性能测试 `scripts/fsperf.py` 的用法：元数据与
  数据各量了什么、怎么读这些数字（缓存、线程、`O_DIRECT`、长尾）、怎么用 `--json` /
  `--compare` 做对比，以及与 `lightnfs-ctl bench` 和 fio 的分工。

## reference/ 协议调研

[nfsv3/](reference/nfsv3/README.md)：概览、RPC/XDR、数据类型、21 个过程、MOUNT、NLM/NSM、
缓存一致性、错误码、实现要点。
[nfsv4/](reference/nfsv4/README.md)：概览、COMPOUND、命名空间与属性、状态模型、委托与回调、
v4.1 会话、pNFS、v4.2 特性、安全、错误码、实现要点。

设计文档只引用这里的结论，不重复协议细节。

## testing/ 测试文档

- [test-report.md](testing/test-report.md)——测试证据汇总：单测/集成、协议一致性
  （cthon / pynfs / fsx）、三层基准数据、安全清单验收。
- [benchmarks.md](testing/benchmarks.md)——`lightnfs-ctl bench` 三层基准的使用指南、
  基线更新规则、真实挂载上的 fio 建议。
- [security-checklist.md](testing/security-checklist.md)——设计 08 册 §8.5 八项加固的落地
  记录，每项带实现点与验证手段。
