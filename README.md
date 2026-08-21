# lightnfs

用户态 NFS 网关：北向 NFSv3 + NFSv4.1/4.2，南向可插拔存储后端（v1：本地文件系统）。
C++20 协程全异步，io_uring reactor（epoll 兜底）。

- 设计文档：[docs/design/](docs/design/README.md)
- 协议调研：[docs/nfsv3/](docs/nfsv3/README.md)、[docs/nfsv4/](docs/nfsv4/README.md)
- 开发计划：[docs/development-plan.md](docs/development-plan.md)

## 构建

依赖：CMake ≥3.22、Ninja、GCC ≥13 或 Clang ≥17、liburing（`apt install liburing-dev`，
无系统包时运行 `scripts/fetch_liburing.sh` vendor 构建）。

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build            # 单测 + 集成测试
./build/bench_nullrpc             # L2 基准：单 reactor null-RPC（门禁 ≥100k rps）
./build/bench_echo                # L1 基准：传输层 echo
```

Sanitizer 配置：`-DLNFS_SANITIZE=address|thread`；fuzz（需 clang）：`-DLNFS_BUILD_FUZZ=ON`
后运行 `./build/fuzz_handle_request fuzz/corpus`。

## 当前状态

阶段 2（M2+M3）完成：NFSv3 全部 21 个过程读写可用——写路径三档稳定级与 COMMIT、
CREATE 三模式（EXCLUSIVE verifier 跨重启）、DRC 重复请求缓存、boot-epoch 写校验子、
fd 缓存读写升级、异步日志/Prometheus 指标/`lightnfs-ctl`/`lightnfs-fh` 工具。
详见 [M2 读写说明](docs/m2-readwrite.md) 与 [M1 只读说明](docs/m1-readonly.md)；
安全清单落地见 [security-checklist.md](docs/security-checklist.md)；接口评审结论见
[Backend API v1 评审](docs/backend-api-review.md)。
