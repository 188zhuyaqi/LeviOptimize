# BDS_PERFORMANCE_PORTING.md 逐项对照

核对日期：2026-10-23。重新读取用户提供的完整原文，按原编号对照本地源码、Endstone 当前实现/历史和实际使用的 LeviLamina 26.51.7 接口。PR #31 已按要求关闭，本轮不再发起 PR。

**原文并非全部适用于目标项目。** 第2节共21项：12项已有源码实现，2.18已实现全帧统计、工具界面部分未移植，其余8项属于目标不存在的框架路径或Linux启动方式。用户指定的2.3/2.4/2.6/2.7/2.8/2.11/2.12/2.18均已新增相应功能路径；NAPI、未命名字段和布局假设集中为可修改的适配点，详见[开发者交接](NATIVE_PORT_HANDOFF.zh.md)。源码与构建通过不表示已完成目标BDS的符号、ABI和实服验证。

## 第2节：21项核心改动

| 编号 | 原文项目 | 当前状态 | 代码或未移植原因 |
| --- | --- | --- | --- |
| 2.1 | 无监听器时绕过包事件 | 未移植：框架不适用 | 优化的是Endstone自己的PacketSend/ReceiveEvent包装。LeviOptimize没有这条包装链；LeviLamina的事件发射器按监听器生命周期装卸hook。复制Endstone插件管理器会新增原先没有的成本，不能视为消除原生BDS开销。 |
| 2.2 | 包事件O(1)连接/玩家查找 | 未移植：框架不适用 | `PeerInfo`/subclient弱引用缓存服务于上述Endstone事件路径，目标没有同一条逐包查找链。原生实体广播的独立优化另见2.8，不能混称此项已完成。 |
| 2.3 | libdeflate、peer splice、移除PacketTrace | 已实现源码，待实服适配验证 | `src/features/network/`，`optNetwork`。加入静态libdeflate/Snappy、raw DEFLATE/None/hybrid协商、尺寸限制和坏数据处理；为新连接替换peer链，保留原生异步任务组。符号集中于RuntimeSymbols.h，布局集中于NativePeerAdapter.h。 |
| 2.4 | 线程压缩scratch/BDS批处理流复用 | 已实现源码 | 同属`optNetwork`：可重入线程scratch、原生批处理stream复用；平凡TLS指针配全局buffer所有权，避免DLL卸载后的TLS析构。native任务/weak控制块未释放时拒绝卸载。 |
| 2.5 | 玩家网络处理后只重建一次优先级 | 已实现 | [`PlayerNetworkingOpt.cpp`](../src/features/PlayerNetworkingOpt.cpp)、[`ChunkOptimizationState.h`](../src/features/ChunkOptimizationState.h)，开关`optPlayerNetworking`。按线程/调用栈批处理、去重，处理嵌套与异常。 |
| 2.6 | 暂存尚不可发送的区块队列 | 已实现源码，待原生容器ABI验证 | `src/features/parked_chunks/`，`optParkedChunks`。node handle存取、有界区块事件日志、重查和生命周期恢复；共享状态处理native回调中禁用/换source的重入。key hash/equality及16/64/488字节布局集中在adapter，运行时先核对native bucket/find。 |
| 2.7 | 任意任务唤醒生成worker | 已实现源码，字段语义待确认 | `WorkerBacklogOpt.cpp`，`optWorkerBacklog`。adapter按Endstone布局把LL的`mUnk853032`解释为offset252的float，通过原生ASYNC数据符号定位；仅预期0.05F改0，禁用条件恢复。不是经目标反汇编确认的字段命名。 |
| 2.8 | 活动玩家快照与实体广播 | 已实现源码 | `src/features/player_snapshot/`，`optPlayerSnapshot`实现活动用户快照、Dimension遍历、活动人数；weak registry/ActorOwner复验，退役与嵌套fallback。既有`optEntityPacketRecipients`保留独立广播收件人路径，仅服务端生效。 |
| 2.9 | 自动保存随机区块快照采样 | 已实现 | [`RandomChunkOpt.cpp`](../src/features/RandomChunkOpt.cpp)、[`ChunkOptimizationState.h`](../src/features/ChunkOptimizationState.h)，`optRandomChunk`。400次重建、单次有界RNG、查活跃区块，销毁/禁用清理；采样返回拥有值的optional。 |
| 2.10 | 扩大混合/存在性缓存 | 已实现 | [`ChunkCachesOpt.cpp`](../src/features/ChunkCachesOpt.cpp)、[`VerifiedBytePatch.h`](../src/features/VerifiedBytePatch.h)，`optChunkCaches`。两个函数独立验证唯一签名和完整expected bytes，条件恢复；增加保留内存。未恢复旧memoization。 |
| 2.11 | 随机刻cache-line预取 | 已实现源码，seed符号待目标发布/确认 | `src/features/random_tick/`，`optRandomTickPrefetch`。复制seed预测采样，不推进RNG；tick/seed符号集中于adapter，可接入经过验证的resolver。缺seed则整组跳过，未复制跨版本绝对地址或恢复旧解锁patch。 |
| 2.12 | locator成对缓存/manager/waypoint | 已实现源码，布局待目标验证 | `src/features/locator/`，`optLocator`实现pair/slot矩阵、RTTI check/hide分类、原生map临时窗口、异常与移除清理及waypoint更新。`LocatorCompat.h`隔离16字节WorldPosition和双vector flat_map布局。manager仍由`optVanillaWaypoints`单独控制。 |
| 2.13 | 无自定义地图渲染时跳过像素重绘 | 未移植：框架不适用 | 跳过的是Endstone的MapView/renderer和包修补成本。LeviOptimize没有插件地图渲染层，不能把新增renderer包装后再旁路当作原生优化。 |
| 2.14 | Python线程复用PyThreadState | 未移植：运行时不适用 | LeviOptimize是原生C++模组，没有Python解释器、GIL或相同callback链。 |
| 2.15 | Endstone调度器空闲阻塞 | 未移植：调度器不适用 | 目标没有Endstone ThreadPoolExecutor的10ms轮询。替换LeviLamina公共调度器属于不同依赖项目，不能凭该提交假定其有同一问题。 |
| 2.16 | Linux全进程mimalloc preload | 未移植：平台/启动时机不适用 | 项目当前只构建Windows，没有Linux wheel/启动器；运行时模组不是进程启动前LD_PRELOAD的等价位置。LL已管理自己的分配器依赖，也不能据此声称已实现全进程替换。 |
| 2.17 | actor到dimension包装器直接查找 | 未移植：框架不适用 | EndstoneActor/EndstoneDimension绑定层不存在于LeviOptimize；目标没有这次按名称二次查找。 |
| 2.18 | 全帧MSPT/TPS口径、status/devtools | 部分实现：统计已实现 | `src/features/timing/`，`optFullFrameTiming`与`/loprofile`。整帧边界、补跑tick分段、排除idle、最近20个有效样本；独立于ECS timing。未移植Endstone Python status/devtools界面、隐藏窗口/渲染线程和平台PSS展示，因为目标没有对应工具系统。 |
| 2.19 | 移除断线玩家地图tracker | 已实现：增强既有功能 | [`ChunkLeakFix.cpp`](../src/features/ChunkLeakFix.cpp)，沿用`fixChunkLeak`。复制稳定玩家ID/匹配key，调用原生`_removeTrackedMapEntity`清除tracker和装饰并处理dirty状态；需要长时实服验证事件时机和区块释放。 |
| 2.20 | 红石失效关联清理 | 已实现，并纠正原文描述 | [`CircuitSceneGraphOpt.cpp`](../src/features/CircuitSceneGraphOpt.cpp)，`optCircuitSceneGraph`。原文“删除pending_updates条目”不准确：实际上按pending updates找到受影响的**power association列表，删除列表中的失效Item**，保留待加入组件并调用原生update。 |
| 2.21 | instaticking时旁路BlockFormEvent | 未移植：框架不适用 | 旁路的是Endstone额外增加的同步事件/日志；目标没有这条事件包装。不能为了减少事件开销而拦截BDS正常固化逻辑。 |

计数复核：已有源码=2.3/2.4/2.5/2.6/2.7/2.8/2.9/2.10/2.11/2.12/2.19/2.20；部分=2.18；未移植=2.1/2.2/2.13/2.14/2.15/2.16/2.17/2.21。八项未移植中七项是目标不存在的Endstone框架/运行时路径，一项是Linux进程启动器。

## 第1、3—8节与中间版本

| 原文范围 | 处理情况与原因 |
| --- | --- |
| 1：公开基准 | 作为上游证据，不移作本项目成绩。100人测试和上游20 TPS不能证明当前移植的收益。 |
| 3：符号/虚表与构建依赖 | 使用LL26.51.7、集中符号/布局adapter，加入静态libdeflate1.25与Snappy1.2.2；字节补丁精确校验，不复制Endstone绝对地址。不构建Linux或移植全进程allocator启动器。编译不等于目标ABI已实测。 |
| 4：验证矩阵 | 最终Windows x64 release服务端/客户端完整构建、十组C++23 helper回归全部通过。0/1/100真人/机器人、资源包、地图/红石长测、split-screen、传送切维度、CPU/RSS/全帧MSPT等实服矩阵仍未完成。 |
| 5：旧memoization、seed解锁、网络旧路径 | 依照真实替代/撤销关系排除；已采用2.10的cache-cap最终方案，不恢复中间方案，也不重复注册移动过的网络hook。 |
| 6.1：树叶/液体无监听器旁路 | 未移植，原因与2.1/2.21相同：Endstone事件层成本，不是目标已有逻辑。 |
| 6.2：历史async-batch网络架构 | 未移植。是完整异步编解码/加密/strand/主线程队列/背压/关闭架构，需要完整设计与协议回归，不能与当前同步peer splice随意混合；该旧远端分支现已不在advertised refs中。 |
| 6.3：编译期优化 | 未逐项移植Endstone的头文件/Entt/模板构建调整，目标构建系统不同，且不是BDS运行时TPS优化。本轮C++23现代化属于目标自身改进，单独提交。 |
| 6.4：早期libdeflate | 未单独叠加。2.3/2.4采用当前peer splice、线程scratch与原生batch stream方案；旧的每连接compressor不是另一个应同时启用的优化。 |
| 7：视距、tick距离、客户端生成、allocator | 属于宿主配置/启动方案，不自动改服主设置；需要保持相同参数重新基准，不能算作代码移植。 |
| 8：原始资料 | 作为审计来源，结合当前clone历史核实；不是额外功能。 |

## 原文之外的新增核查

- `7049af4`生成任务上限已实现为默认关闭的实验选项`optChunkGenDispatcher`；不能与未移植的worker backlog混为一谈。
- 更新分支的`5696530`候选评分仍延期：部分构造FakePolicy、跨调用地址缓存和抽样对比不能证明目标安全。
- 原索引遗漏的ActorUniqueID hash在实际LL26.51.7已经修复；新广播路径只检查已知SDK差异，不重复修改BDS哈希。
- 踢出连接收尾、物品输入负载保护、原生packet limiter配置、RakNet已修复的重复过滤和NetherNet设置，详见[第二轮历史审计](ENDSTONE_SECOND_AUDIT.md)。未验证目标原生缺陷时不增加替代反序列化/断连流程，也不静默写宿主配置。

具体上游来源、风险和实服检查场景见[移植审计](ENDSTONE_PORTING.md)及[开发者交接](NATIVE_PORT_HANDOFF.zh.md)。十四个新增选项默认关闭，PR已关闭，本轮不新建或重开。
