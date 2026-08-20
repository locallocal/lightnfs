# Backend API v1 接口评审记录（M1）

评审基线：`docs/design/05-backend-api.md`、`06-backends.md` §6.5/§6.6。结论：
`kBackendApiVersion = 1` 覆盖 LocalFS、Lustre 与 GlusterFS 的对象、匿名 IO、目录游标、
能力协商和锁下沉需求，可以进入 5.10 演进规则管控。

## Lustre 映射核对

| Backend API | Lustre 映射 | 结论 |
|---|---|---|
| `ObjId` / `resolve` | 16B FID；`llapi_open_by_fid` | ✅ P1/P2，无接口缺口 |
| `getattr` / `change` | POSIX statx + changelog/版本 | ✅ `kNativeChange` 可表达 |
| `lookup` / 目录修改 | POSIX 挂载点 `*at` 调用 | ✅ 对象接口逐项覆盖 |
| `open` / 匿名 IO | FID 打开；uring pread/pwrite | ✅ `OpenCtx.open == nullptr` 可表达 |
| `readdir` | getdents/d_off | ✅ 稳定 cookie 与 plus enrichment 可表达 |
| `statfs` / `limits` | `llapi_obd_statfs`、条带参数 | ✅ 文件系统级接口覆盖 |
| HSM 延迟 | released 文件恢复 | ✅ `kJukebox` 能力与错误哨兵覆盖 |
| 字节锁 | MDS 全局 flock | ✅ `native_locks()`/`kByteLocks` 切换点唯一 |

## GlusterFS 映射核对

| Backend API | libgfapi 映射 | 结论 |
|---|---|---|
| `Backend::start/stop` | `glfs_new/init/fini` | ✅ 生命周期接口覆盖 |
| `ObjId` / `resolve` | 16B GFID；`glfs_h_create_from_handle` | ✅ P1/P2，无接口缺口 |
| 对象与目录操作 | `glfs_h_lookupat/creat/...` | ✅ handle-based 接口逐项覆盖 |
| `open` / 匿名 IO | `glfs_h_open` / `glfs_h_anonymous_open` | ✅ v3/v4 共用模型覆盖 |
| `readdir` | `glfs_readdirplus_r` + d_off | ✅ attr/oid 尽力返回可表达 |
| 阻塞调用 | libgfapi 调用放入 offload | ✅ Task 接口不泄露线程模型 |
| `change` | 无原生计数 | ✅ 不置 `kNativeChange`，core 合成 |
| 字节锁 | posix-locks xlator | ✅ `native_locks()` 可下沉 |

## 决议

- 必选语义或必选操作变化必须提升 `kBackendApiVersion`。
- 新增可选操作必须提供 `ENOTSUP` 默认实现并分配新的 `Cap` 位。
- Backend API 不接受 NFS 状态码、文件句柄或协议过程类型。
- LocalFS 无特权降级句柄不置 `kStableHandles`；多节点/跨重启生产部署必须使用
  kernel fhandle 模式或未来具备原生稳定 ID 的后端。
