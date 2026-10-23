# 原生性能移植交接

2026-10-23，按用户追加要求实现原索引 2.3、2.4、2.6、2.7、2.8、2.11、2.12、2.18。PR #31 已按要求关闭；修改同步到用户 fork 的 `perf/port-endstone-optimizations` 分支，同时保留本地源码和补丁，不新建或重开 PR。原始 `BDS_PERFORMANCE_PORTING.md` 保持不变，最新逐项状态见[对照清单](PORTING_CHECKLIST.zh.md)。

这里的“实现”指已有完整功能路径和目标 SDK 适配代码；运行时符号发布、ABI 假设和实服收益仍需开发者确认。未用 Endstone 另一版本的绝对地址替代缺失符号。

## 功能入口和集中适配点

| 原编号 / 默认关闭的开关 | 已实现路径 | 开发者优先确认的位置 |
| --- | --- | --- |
| 2.3 / `optNetwork` | libdeflate raw DEFLATE、Snappy/None/hybrid 协商；新连接压缩 peer splice、移除该链上的 PacketTrace；保留原生异步任务组和接收批处理。 | `src/features/network/RuntimeSymbols.h`：incoming、NetworkSystem 析构和 BatchedNetworkPeer::flush 的精确 decorated symbols。`NativePeerAdapter.h`：LL 的 CompressedNetworkPeer 声明适配及原生字段布局。需实测压缩阈值、资源包、加密、分屏、重连、其他网络模组。 |
| 2.4 / 与 `optNetwork` 合并 | 复用原生 outgoing batch stream；可重入的每线程压缩 scratch，批包 varuint32 解析、尺寸上限、坏数据拒绝。线程只持有平凡 TLS 指针，缓冲由 DLL 全局存储统一拥有。 | `NetworkBuffers.h`、`NetworkCodec.h`；压缩结果只在同步消费期间借用，异步队列继续由原生拥有。确认目标批处理 stream 的消费时机和 peer 链组合。 |
| 2.6 / `optParkedChunks` | 通过 node handle 暂存不可发送区块；按区块事件、两 tick 游标和周期性重查恢复；传送、source/维度变化、disconnect、销毁和禁用时还原。事件日志有容量上限，落后游标全量重查。 | `src/features/parked_chunks/ParkedChunksAdapter.h`：SDK 缺失的 key equality/hash、必需符号及 16/64/488 字节布局。原生 bucket/find 校验通过后才 extract；仍需目标二进制与 native unordered ABI 对照。 |
| 2.7 / `optWorkerBacklog` | 原生异步 worker 最小积压等待由 0.05F 改为 0；值不符时跳过，禁用时只恢复仍为本模块写入的值。 | `src/features/WorkerBacklogAdapter.h`：`ASYNC` 使用原生数据符号，非 SDK synthetic accessor；`mUnk853032` 暂按 Endstone 布局解释为 offset 252 的 float，保留 static_assert 和 0.05F 运行时门槛。**该字段语义仍需目标 ABI 确认**。 |
| 2.8 / `optPlayerSnapshot` + 既有 `optEntityPacketRecipients` | 活动用户快照、维度 forEachPlayer 与活动人数；每次使用先锁 weak registry 并复验 ActorOwner，按 Level 退役；广播继续保留每次调用独立选择与原子 fallback。 | `src/features/player_snapshot/PlayerSnapshotOpt.cpp`：LL weak-ref/registry、active-users 字节身份与 ActorOwner 关系。保留世界线程约束，列表改变的嵌套遍历回退原生。广播选项仅服务端安装。 |
| 2.11 / `optRandomTickPrefetch` | 在 spawner 后按原生 seed 副本预测同批随机刻样本，最多 256 个固定数组项，预取 subchunk 存储，不修改 live RNG。 | `src/features/random_tick/RandomTickAdapter.h`：tickImpl 和函数局部静态 seed 的符号名由 SDK 签名／clang MS ABI probe 推导，**不等于目标符号库已发布**。若未暴露 seed，可实现 `seedResolverOverride`；缺失时整组不装 hook。不恢复被上游撤销的 seed-unlock patch。 |
| 2.12 / `optLocator` + 既有 `optVanillaWaypoints` | sender 成对 slot 矩阵、移除/重用清理、RTTI 分类 check/hide 回调、sent-data 临时窗口；ServerWaypoint 图标、位置与可见性更新；manager 成员检查保留独立开关。 | `src/features/locator/LocatorCompat.h`：WorldPosition 按 16 字节 `{Vec3,int}`，flat_map 按 key/mapped 两个 vector 解释。`LocatorOpt.cpp` 集中 hook 候选符号与 RTTI 分类。大小检查不能证明字段语义，需目标反汇编/发包对照；未知回调走原生。 |
| 2.18 / `optFullFrameTiming` | ServerInstance 更新开始/结束计时，Level tick 分段处理 catch-up；最近 20 个有效样本，排除帧间空闲；host 权限 `/loprofile` 显示当前/平均 TPS、MSPT。 | `src/features/timing/FullFrameTimingOpt.cpp`：析构、queueForServerThread 必需符号，原生 coordinator 注册与安全任务边界退役；只采集 service 当前 ServerInstance/Level。没有移植 Endstone Python status/devtools 窗口、渲染线程或平台 PSS 展示。 |

## 生命周期与卸载

- 网络关闭后停止改造新连接，能安全识别为本模块所有的链恢复原生；被其他模组持有、锁忙或仍被任务引用的包装器退回原生处理。原生任务和 expired weak_ptr 仍可能持有 DLL 定义的 control block，因此同时阻止 DLL 卸载；待其全部释放后重试。
- 全帧计时禁用时先将共享 registration 标为 inactive，通过服务器任务边界移除原生监听器；不在事件分发中直接删 listener vector。排队的原生 `std::function`、监听记录及活跃 hook 仍会阻止卸载，析构 hook 保留到清理完成。若实例恰在嵌套事件分派中析构，无法证明外部 non-owner coordinator 已释放监听器时，保留 inactive 记录并拒绝卸载，需要重启进程。
- 网络和停放区块 hooks 使用 resident lease；禁用功能只关闭入口并恢复状态，待活跃回调退出后才由卸载检查释放 hook，避免回调中拆除自己的 trampoline。
- `LeviOptimize::unload()` 分别调用网络、停放区块和全帧计时三个 `canUnload()`，任意一项尚未清理便返回 false。常规 disable 与完整 DLL unload 是两个不同阶段。
- 停放区块路径在 native 回调前后检查退役/代次，回调中的关闭、换 source 或删除玩家不会继续访问原先失效的状态。真实 BDS 重入和服务器关闭仍要实测。

## C++23 与验证

沿用项目 C++23 构建，使用值语义、`std::optional` / `std::expected`、range 查询、PImpl 和 RAII 管理临时窗口、节点、buffer 与回调代次。不得通过手工定义 STL 内部宏开启标准特性。

最终 Windows x64 release 的 server/client 均完整编译链接通过（LLVM 22.1.8、MSVC 14.44.35207、LL 26.51.7）。十组回归全部通过，其中网络34项、区块流状态16项、全帧计时12项；构建/执行方式见 [tests/README.md](../tests/README.md)。纯 helper 检查覆盖协议、边界值、状态迁移和异常恢复；不能覆盖 native hook 安装与整个服务器的生命周期。完整日志位于工作区 `artifacts/complete-{server,client}-build.log` 与 `artifacts/complete-helper-tests.log`。

建议开发者首先开启单项并记录符号解析与 ABI 门槛，再验证登录、分屏、资源包、压缩协商、玩家进出、传送切维度、区块重载、定位栏开关、禁用/启用/卸载和停服。使用同一世界、视距、tick 距离及人数分别测 CPU、RSS、全帧 MSPT/TPS。当前没有 BDS 实服或机器人工作负载，没有本项目的收益百分比。

## 本地交付与补丁基线

本地分支仍是 `perf/port-endstone-optimizations`。工作区根目录的 `LeviOptimize-Native-expansion.patch` 是本轮增量，基于本地 `feed15a`；该基线与此前 fork 分支的 `276f6c4d04daf844e372afed8068433520da3876` 具有相同的 tree `12e8f3c640c422181d682f91bb1c5927145d4503`，可在该 fork 内容上应用。先执行 `git apply --check <补丁路径>`，确认后再 `git apply <补丁路径>`。

`LeviOptimize-Endstone-full.patch` 包含从原项目 `e22a84421563cdb3108cd4e92ada0481f4e2e12f` 起的全部移植，应应用到干净的原始基线。两个补丁是不同起点的交付方式，不要依次叠加。源码包保留当前提交的全部受版本管理文件；构建产物为开发验证用，不代表已完成实服发布验证。
