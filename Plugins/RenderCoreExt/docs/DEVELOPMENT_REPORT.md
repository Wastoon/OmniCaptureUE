# RenderCoreExt 插件整体开发汇报

## 1. 项目定位

RenderCoreExt 是一个面向 Unreal Engine 5 的自动化渲染与数据采集插件。它的核心目标不是替代 UE 编辑器里的手动渲染，而是把 UE 场景、MetaHuman、Level Sequence、多视图相机和外部 Python 控制程序连接成一条可编程的数据生产管线。

从应用角度看，这个插件解决的是一个很明确的问题：

> 如何在 UE 中自动控制数字人动画，并在每一帧稳定导出多视角、多模态的训练或分析数据。

它面向的不是单张效果图，而是批量化、可重复、可扩展的数据集生成流程。

## 2. 核心能力概览

当前插件主要包含五类能力。

| 能力模块 | 说明 |
| --- | --- |
| 外部控制接口 | Python 客户端通过 TCP 向 UE 发送 JSON 命令，实现自动化调度 |
| 场景对象控制 | 支持设置 Actor 位姿、查找场景对象、管理相机组件 |
| 多视图相机系统 | 支持 pinhole、fisheye、dome 等多种相机模型和多相机布局 |
| 多通道数据导出 | 支持 RGB、Depth、Normal、World Position、Semantic 等数据输出 |
| 动画序列驱动 | 支持控制 MetaHuman 表情曲线，也支持逐帧驱动 Level Sequence |

这些能力组合起来，使插件具备从“控制动画”到“采集数据”的完整闭环。

## 3. 系统架构

插件整体可以分为三层。

```text
Python Client
    |
    | TCP + JSON Command
    v
RenderServer Actor
    |
    | Command Dispatch
    v
UE Runtime Components
    |-- Camera Components
    |-- Data Exporter
    |-- MetaHuman / AnimInstance
    |-- LevelSequencePlayer
```

### 3.1 Python Client 层

Python 客户端负责描述“要做什么”，例如：

- 创建多少个相机
- 每个相机的位置和视角
- 输出分辨率是多少
- 需要导出哪些通道
- 要播放哪个 Level Sequence
- 从第几帧采集到第几帧

Python 端本身不直接参与 UE 渲染，它只负责把任务参数结构化后发送给 UE。

这种设计的优点是灵活：后续不管是接入数据集生成脚本、实验平台、调度系统，还是批量任务管理，都可以基于 Python 端扩展。

### 3.2 RenderServer 层

RenderServer 是插件的中枢。它在 UE 场景中作为一个 Actor 存在，负责：

- 监听 socket 连接
- 解析 JSON 命令
- 维护任务状态
- 分发命令到不同组件
- 在 UE Tick 中推进异步任务

RenderServer 的重要设计点是：它不是收到命令后在一个函数里阻塞式完成所有工作，而是把长任务拆成状态机，在 Tick 中逐步推进。

这非常符合 UE 的运行模型，尤其适合 Sequencer 跳帧、动画蓝图更新、GPU 渲染同步这类需要跨帧完成的任务。

### 3.3 UE Runtime Components 层

这一层是具体执行层，包括：

- `UFisheyeCameraComponent`
- `UPinholeCameraComponent`
- `UMultiViewFisheyeCameraComponent`
- `UMultiViewERPCameraComponent`
- `UDomeLightStageCameraComponent`
- `DataExporter`

它们负责相机创建、渲染目标管理、后处理材质、图像读取、深度保存、位姿保存和多通道文件导出。

## 4. 典型工作流程

以“驱动 MetaHuman 的 driveface Level Sequence 并逐帧采集”为例，完整流程如下：

```text
1. Python 客户端连接 UE RenderServer
2. 清理旧相机组件
3. 创建 dome 多视角相机系统
4. 配置 RGB / Depth / Normal / Position / Semantic 通道
5. 指定 Level Sequence Actor，例如 driveface
6. RenderServer 找到对应的 LevelSequencePlayer
7. 设置 Sequencer 到目标帧
8. 等待动画、RigLogic、渲染状态稳定
9. 调用多视图相机进行采集
10. 保存当前帧所有视角和所有通道
11. 推进到下一帧，重复 7-10
12. 完成整个序列的数据集导出
```

这个流程的关键是“动画帧”和“渲染帧”绑定在一起。插件不是简单播放动画录屏，而是精确控制每一帧的状态，然后采集这一帧的多模态数据。

## 5. 功能设计重点

### 5.1 命令式外部控制

插件采用 JSON 命令作为外部控制协议。典型命令包括：

| 命令 | 功能 |
| --- | --- |
| `configure_camera` | 创建并配置相机组件 |
| `clear_cameras` | 清除指定 Actor 上的相机 |
| `set_pose` | 设置 Actor 位置和旋转 |
| `set_metahuman_expression` | 注入 MetaHuman 表情曲线 |
| `capture_metahuman_sequence` | 程序化表情逐帧采集 |
| `set_sequence_frame` | 设置 Level Sequence 到指定帧 |
| `capture_level_sequence` | 对 Level Sequence 进行逐帧采集 |

这种命令式设计让插件具备很强的可扩展性。新增功能时，只需要扩展命令字段和 UE 端处理逻辑，不需要改变整体架构。

### 5.2 多视图相机抽象

插件并没有把相机写死成某一个固定模型，而是抽象出不同相机组件：

- pinhole：用于传统透视相机采集
- fisheye：用于鱼眼模型采集
- ERP：用于全景投影
- dome：用于环绕式多视角采集

在 dome 模式下，可以通过 Python 手动指定相机阵列。例如当前 MetaHuman 采集使用 17 个相机覆盖上半球：

- 8 个水平环绕视角
- 8 个上方环绕视角
- 1 个顶部视角

这种设计比单个摄像机更适合数字人数据采集，因为它可以同时获得同一动画帧在多个视角下的观测。

### 5.3 多通道输出设计

插件的输出不是单纯 RGB 图像，而是多模态数据：

- RGB：用于外观、纹理和视觉效果
- Depth：用于几何距离
- Normal：用于表面方向
- World Position：用于三维空间定位
- Semantic：用于语义或区域标注
- Pose/metadata：用于记录相机参数和帧信息

这让插件从“渲染工具”变成“数据生产工具”。对后续三维重建、NeRF/Gaussian Splatting、视觉算法训练、数字人分析等任务更有价值。

### 5.4 Sequencer 驱动设计

Level Sequence 是 UE 中标准的动画编排工具。插件支持直接控制 Sequencer 的时间轴，本质上相当于用代码执行：

```text
SequencePlayer -> SetPlaybackPosition(frame)
```

这带来一个重要能力：动画资产仍然可以按照 UE 原生工作流制作，例如 MetaHuman face 动画、ARKit mapping rig、mocap 动画等；插件只负责在采集阶段接管时间轴并逐帧导出。

这比完全用代码生成动画更通用，也更容易和美术、动画流程协作。

## 6. 架构设计取舍

### 6.1 为什么使用 socket 而不是只用 UE Python

UE Python 更适合编辑器脚本，但本项目需要外部客户端长期控制 UE，并且需要和外部数据处理流程结合。Socket 方式更适合：

- 跨进程控制
- 外部任务调度
- 长时间批量采集
- 与 Python 数据处理生态对接
- 未来扩展到远程机器或批处理系统

### 6.2 为什么使用 Tick 状态机

逐帧采集不是一个纯 CPU 同步任务。Sequencer 跳帧之后，动画蓝图、Control Rig、RigLogic、渲染线程、GPU render target 都需要时间更新。

如果在同一帧内立刻采集，很容易拿到旧状态。因此插件使用 Tick 状态机：

```text
跳到目标帧
等待若干 Tick
执行渲染采集
推进下一帧
```

这个设计牺牲了一点实现复杂度，但换来更稳定的帧同步。

### 6.3 为什么把相机配置放在 Python 端

相机阵列、分辨率、导出通道、材质路径都可能因任务不同而变化。如果全部写死在 C++ 插件中，每次改实验参数都要重新编译。

把配置放到 Python 端后，实验迭代更快：

- 改视角布局不用编译
- 改分辨率不用编译
- 改采集帧范围不用编译
- 改输出目录不用编译

C++ 插件负责稳定执行，Python 负责灵活描述任务。

## 7. 核心难点与挑战

### 7.1 UE 动画系统和渲染系统的同步

本项目最难的地方不是“调用一次渲染函数”，而是保证采集时刻的状态正确。

在一帧采集中，实际涉及多个系统：

- Level Sequence 时间轴
- Skeletal Mesh 动画
- MetaHuman Face AnimBP
- RigLogic
- Component Transform
- Scene Capture
- Render Target
- 后处理材质
- GPU 到 CPU 的图像读取

这些系统不是全部在同一个函数调用中同步完成的，因此必须设计跨帧等待和状态推进机制。

### 7.2 多视角多通道的一致性

每一帧可能需要导出 17 个视角，每个视角又包含多个通道。真正的挑战是保证它们是一组一致的数据：

- 同一动画帧
- 同一相机位姿
- 同一分辨率
- 同一命名规则
- RGB 和 depth 对齐
- metadata 能够追溯

否则即使文件都导出了，也未必是可用于算法的数据。

### 7.3 MetaHuman 的复杂性

MetaHuman 不是普通 Skeletal Mesh。它包含：

- Face AnimBP
- Post Process AnimBP
- RigLogic
- 大量 `CTRL_*` 曲线
- LOD 相关 mesh
- Sequencer 绑定关系

要让 Python 控制表情，或者让 Sequencer 播放 face 动画后稳定采集，需要理解 UE 动画蓝图和 MetaHuman 内部驱动链路。

### 7.4 插件工程化稳定性

UE 插件开发本身有较高工程复杂度：

- UHT 宏和反射系统严格依赖头文件结构
- Build.cs 依赖必须准确
- 引擎 private header 容易造成版本耦合
- Editor 进程会锁定 DLL
- Visual Studio 错误列表可能包含大量级联错误

因此插件开发不仅是功能开发，也包含大量工程稳定性工作。

### 7.5 大规模数据导出的性能压力

高分辨率、多视角、多通道意味着数据量非常大。例如：

```text
17 个相机 × 5 个通道 × 2048 分辨率 × N 帧
```

这会带来：

- GPU render target 压力
- CPU 读回压力
- 磁盘写入压力
- 任务耗时变长
- 内存和显存资源管理问题

后续要从功能可用走向生产可用，必须继续做长序列压力测试和资源管理优化。

## 8. 当前进展评估

当前项目已经完成主链路闭环：

```text
外部 Python 控制
    -> UE 插件接收命令
    -> 配置多视图相机
    -> 控制 MetaHuman / Level Sequence
    -> 逐帧等待稳定
    -> 多通道导出
```

按功能成熟度估计，整体约完成 80%。

| 模块 | 状态 |
| --- | --- |
| 通信协议 | 已打通 |
| 命令分发 | 已打通 |
| 相机创建与配置 | 已打通 |
| 多通道导出 | 已打通 |
| MetaHuman 表情控制 | 已验证 |
| Level Sequence 逐帧控制 | 已打通 |
| 长序列稳定性 | 待加强 |
| 任务状态回传 | 待加强 |
| 数据完整性校验 | 待加强 |
| 配置文件化 | 待加强 |

## 9. 后续规划

### 9.1 任务状态回传

当前主要依赖 UE 日志判断执行状态。后续可以让 UE 主动向 Python 返回：

- 命令是否成功
- 当前采集帧
- 当前输出路径
- 当前失败原因
- 任务完成状态

这样 Python 客户端可以成为完整的任务控制台。

### 9.2 数据校验工具

增加采集后的自动检查：

- 帧数是否完整
- 每帧视角数量是否完整
- 每个视角通道是否完整
- 图像尺寸是否一致
- 文件是否可读
- metadata 是否和图像对应

这对后续批量数据生产非常关键。

### 9.3 配置文件化

将相机布局、材质路径、导出通道、Actor 名称、帧范围等参数迁移到配置文件，减少 Python 脚本里的硬编码。

目标是让用户只改配置，不改代码。

### 9.4 可视化调试面板

可以增加 UE 编辑器内面板，用于显示：

- RenderServer 是否运行
- 当前连接状态
- 当前命令
- 当前帧
- 当前相机组件
- 当前输出目录

这会显著降低演示和调试成本。

### 9.5 性能和稳定性优化

继续测试：

- 更长动画序列
- 更高分辨率
- 更多相机
- 更多输出通道
- 多轮连续采集

并针对瓶颈优化 render target 复用、磁盘写入、GPU readback 和资源释放。

## 10. 汇报总结

RenderCoreExt 插件的核心价值，是把 UE 从一个交互式内容创作工具，扩展成一个可由外部程序控制的数字人数据生产系统。

它打通了四个关键环节：

```text
外部自动化控制
    -> UE 场景和动画驱动
    -> 多视角多通道渲染
    -> 结构化数据集导出
```

这套系统后续可以服务于：

- MetaHuman 表情数据采集
- 多视角人脸/头部数据集生成
- 三维重建数据生产
- 神经渲染训练数据生成
- 算法评测和仿真数据构建

从工程角度看，目前最重要的工作已经完成：主链路被打通，关键技术可行性已经验证。下一阶段的重点是把它从“能跑通”提升为“稳定、可配置、可监控、可批量生产”。
