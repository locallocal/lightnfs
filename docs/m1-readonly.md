# M1：NFSv3 只读服务

本阶段实现 NFSv3 `NULL/GETATTR/LOOKUP/ACCESS/READLINK/READ/READDIR/READDIRPLUS/FSSTAT/
FSINFO/PATHCONF`，以及 MOUNTv3 `NULL/MNT/EXPORT`（DUMP/UMNT/UMNTALL 兼容空实现）。

## LocalFS 句柄模式

- `handles = "kernel"`：使用 `name_to_handle_at/open_by_handle_at`，置
  `kStableHandles`，满足跨进程重启 P1 和删除重建 P2。进程需要
  `CAP_DAC_READ_SEARCH`；文件系统必须支持 exportfs 句柄。
- `handles = "auto"`（默认）：启动时实际执行句柄生成和反查探测；失败后显式降级。
- `handles = "fallback"`：编码 `{st_dev, st_ino, btime-generation-hint}`；无 birth-time
  的文件系统改用进程内 generation，并用进程内
  ObjId→相对路径索引反查。不置 `kStableHandles`。外部 rename 或进程重启后旧句柄可能
  STALE，只适合开发、容器和无特权试用，不能宣称标准 NFSv3 持久句柄语义。

## 安全边界

- 文件句柄使用持久化 128-bit 密钥和 SipHash-2-4 认证；伪造/损坏返回 BADHANDLE，
  已删除对象返回 STALE，客户端不在导出 CIDR 中返回 ACCES。
- root/all squash 在认证凭证进入后端前完成。
- LocalFS lookup 使用单分量检查与 `O_PATH|O_NOFOLLOW`；导出根的 `..` 被钳制在根。
- AUTH_SYS 没有加密和强认证，只能用于可信网络或由 WireGuard/TLS 隔离的网络。

## 启动

```sh
cp config/lightnfs.toml.example /tmp/lightnfs.toml
./build/lightnfsd --check-config --config /tmp/lightnfs.toml
./build/lightnfsd --config /tmp/lightnfs.toml
```

默认向本机 rpcbind 注册 NFSv3 和 MOUNTv3；rpcbind 不可用时仍可通过 mount 的
`port=`/`mountport=` 显式指定端口。

## 验收（开发计划 §3.5）

验收分两半，数据集统一由 `scripts/gen_dataset.sh DEST [BIGDIR_COUNT]` 生成
（`tree/` 内容树 + `manifest.md5` + `bigdir/` 10 万项 + `cthon/` 预置树）。

**回环半（无 root）**：`scripts/accept_m1_local.sh [ASAN_STRESS_SECS] [BIGDIR_COUNT]`
一键构建 Release+ASAN、起真实 `lightnfsd`、用 `lnfs_accept_client`（用户态 NFSv3 客户端）执行：

- `walk`：READDIRPLUS 递归遍历，目录项集合与后端目录逐目录比对，普通文件 READ 逐字节比对、
  符号链接 READLINK 比对；FSSTAT/FSINFO/PATHCONF/ACCESS 冒烟；负路径（伪造句柄→BADHANDLE、
  坏 cookieverf→BAD_COOKIE、全部写过程→PROC_UNAVAIL、只读导出 ACCESS 不给写位）
- `bigdir`：纯 READDIR 对 10 万项目录全分页，无重复、无遗漏
- `stress`：多连接×流水线随机偏移 READ，每个应答与本地文件逐字节比对；ASAN 构建下长跑
  即泄漏检查（服务器平滑退出后 LeakSanitizer 判定）

**真实 mount 半（需特权）**：

- root VM / CI runner：`scripts/accept_m1_vm.sh [BIGDIR_COUNT]`（CI 的 `m1-acceptance` 作业）
- docker 主机：`scripts/accept_m1_container.sh [BIGDIR_COUNT]`（特权容器 + host 网络）
- 已挂载环境：`scripts/accept_m1.sh SERVER EXPORT [NFS_PORT] [MOUNT_PORT] [COUNT]`
  （`ls -lR`、`cat`、`md5sum -c`、bigdir 计数、只读强制）

cthon04 由 `scripts/fetch_cthon.sh` 拉取构建（对 test5b 打最小 `CTHON_RO` 补丁：跳过尾部
unlink 清理）；`scripts/cthon_ro.sh CTHON_DIR TESTDIR` 运行只读可行子集
test3（lookup）/test5b（read）/test9（statfs）。其余 basic 测试均含写操作，属阶段 2 验收。
