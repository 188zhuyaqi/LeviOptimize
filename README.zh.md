# LeviOptimize

[![English](https://img.shields.io/badge/English-informational?style=for-the-badge)](README.md)
![中文](https://img.shields.io/badge/简体中文-inactive?style=for-the-badge)

Minecraft基岩版服务器（BDS）的性能优化插件，旨在通过解决每秒传输帧数（TPS）低下的问题来提升服务器性能。该插件在LeviLamina框架内运作，通过修改BDS代码以实现显著的性能改进。

## 目录

- [LeviOptimize](#levioptimize)
  - [目录](#目录)
  - [安全性](#安全性)
  - [安装](#安装)
    - [使用Lip](#使用lip)
    - [手动安装](#手动安装)
  - [使用](#使用)
  - [命令](#命令)
  - [功能](#功能)
  - [贡献](#贡献)
  - [许可证](#许可证)

## 安全性

LeviOptimize是一个独立项目，不隶属于Mojang或Microsoft，因此没有得到官方支持。它是开源且免费使用的。用户应注意，LeviOptimize对服务器或数据可能造成的任何潜在损害不承担责任。请谨慎使用，并自行承担风险。

## 安装

### 使用Lip

```sh
lip install github.com/LiteLDev/LeviOptimize
```

### 手动安装

1. 从[发布页面](https://github.com/LiteLDev/LeviOptimize/releases)下载最新版本。
2. 解压`LeviOptimize-windows-x64.zip`文件。
3. 将`LeviOptimize`文件夹复制到BDS安装目录下的`mods`文件夹中。

## 使用

首次运行后，LeviOptimize将在`mod/LeviOptimize/config/`中生成配置文件。您可以修改这些文件以定制插件的行为。

## 命令

- **`timing`**：显示服务器性能统计信息，重点关注TPS和详细的ECS性能指标。
- **`loprofile`**：开启 `features.optFullFrameTiming` 后显示全帧 MSPT/TPS，平均值取最近最多 20 个已采样 tick。通过 `commands.performanceCommand` 控制命令注册。

> **注意**：如果您在使用`timing`命令后应用程序立即崩溃，并且异常原因是空指针，请检查您的msvcp140.dll版本。考虑更新或修复Visual
> C++
> Redistributable，或者简单地从一个正常工作的设备复制一个msvcp140.dll文件到您的BDS根目录。

## 功能

- **`optMovingBlock`**：通过优化活塞运动过程中的方块实体处理来提高FPS和TPS。
- **`fixChunkLeak`**：玩家断开连接时调用 BDS 原生方法清理地图 tracker、装饰和脏状态，使其持有的区块视图可以卸载。
- **`optSeenPercent`**：缓存特定坐标及其相应边界框内的“SeenPercent”值，以提高TPS。由于存在哈希冲突可能导致性能下降的可能性，实际效果不确定。
- **`optPushEntity`**:
  通过减少实体拥挤导致的TPS下降，提升服务器性能，主要包括三个配置选项：
  - `disableVec0Push`:
    当实体碰撞向量为零时阻止实体被挤压，无需显著改变游戏玩法即可提高性能。
  - `maxPushTimes`:
    限制实体每刻被推动的次数，`-1`表示此限制默认关闭。谨慎调整以避免影响原生特性。
  - `unlimitedPlayerPush`:
    不再限制与玩家相关的推动，只在`maxPushTimes`启用时生效。
- **`optPlayerNetworking`**：在所有玩家网络处理完成后，每个区块生成优先级列表只重建一次，避免每位玩家都重复重建。
- **`optRandomChunk`**：自动保存通过区块坐标快照采样，避免反复线性遍历区块表。新增坐标最多在 400 次采样后进入快照；已失效或未加载完成的区块会被跳过。
- **`optCircuitSceneGraph`**：电路更新前清理失效的红石供电关联，保留仍存在或等待添加的组件。
- **`optChunkGenDispatcher`**：来自 Endstone `perf/chunk-gen-dispatcher` 分支的 Windows 实验选项，将区块生成任务的并发上限提高四倍。只有函数边界和指令签名均匹配才应用，否则记录警告并保持 BDS 原行为。更多并发任务可能增加 CPU 和内存占用。
- **`optChunkCaches`**：将区块混合和区块存在性缓存的容量从 10,000/30,000 提高至约一百万，减少玩家分散时的缓存清空。每项修改都要求在目标函数边界内唯一匹配指令，禁用时恢复。此选项会提高保留内存的上限。
- **`optVanillaWaypoints`**：定位栏路标更新时先汇总已跟踪的实体 ID，避免对每个玩家反复扫描整个路标组。定位栏开关变化或存在尚未跟踪的在线玩家时，仍执行原生更新。
- **`optEntityPacketRecipients`**：仅服务端生效，实体广播时只读取一次实体 ID，通过玩家附近实体表收集接收者后批量发送，保留分屏客户端标识。遇到必要的实体 ID 无效时，在发送前回退原生路径；修改实体相关性判断的模组需要兼容性测试。

- **`optNetwork`**：为新连接安装 libdeflate/Snappy 压缩 peer、移除该链上的 PacketTrace，复用原生批处理流和支持重入的线程缓冲。已有连接需重新连接。
- **`optParkedChunks`**：暂存尚不可发送的 publisher 队列节点，根据区块事件重新检查；恢复和禁用时归还原生节点。
- **`optWorkerBacklog`**：核对目标字段和预期值后，将异步 worker 的积压等待从 0.05 秒设为零。
- **`optPlayerSnapshot`**：复用经过有效性检查的活动玩家快照，加速维度遍历和人数统计；不兼容的嵌套遍历回退原生。
- **`optRandomTickPrefetch`**：复制原生随机刻 seed 来预测采样位置并预取存储，不推进游戏 RNG。
- **`optLocator`**：实现定位栏成对 slot 缓存和路标图标／可见性更新，可与独立的 manager 优化 `optVanillaWaypoints` 同时使用。
- **`optFullFrameTiming`**：采集服务器整帧工作时间，覆盖 Level tick 前的工作并分段统计补跑 tick，排除帧间空闲，供 `loprofile` 展示。

本次移植新增的十四个选项默认均为 `false`，配置版本为 20。在 `features` 中逐项开启并重启后测试。符号／布局适配点和待完成的实服验证见[开发者交接](docs/NATIVE_PORT_HANDOFF.zh.md)与[移植审计](docs/ENDSTONE_PORTING.md)。缺少必要符号时跳过对应 hook 组。网络和计时功能禁用后若仍有原生任务或引用未释放，模组会拒绝卸载 DLL，待其释放后再重试。


## 贡献

欢迎通过创建问题或提交PR来提问和贡献。

## 许可证

GPL-3.0-or-later © LiteLDev

改编自 Endstone 的代码保留 Apache-2.0 声明，见[第三方声明](THIRD_PARTY_NOTICES.md)。
