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
