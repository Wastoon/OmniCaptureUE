# OmniCaptureUE

OmniCaptureUE 是一个基于 Unreal Engine 5.5 和 `RenderCoreExt` 插件构建的数据仿真与多视角渲染平台。项目目标是支持可配置的多相机阵列、可配置灯光阵列、相机运动轨迹或 Level Sequence 导入，并对静态场景与动态场景生成高精度多属性真值数据。

当前系统已经支持运动多相机阵列下的多属性渲染，包括真实化 RGB 图、精确深度图、精确法线图、世界坐标 Position 图、Semantic 图，同时导出运动相机内参、相机阵列外参以及运动 rig 的 pose 信息。项目中还搭建了 dome 穹顶相机阵列，可以自定义相机数量、相机外参和穹顶拍摄系统中的灯光阵列，并以 MetaHuman 为渲染试点完成了多机位、多属性的高精度渲染验证。

![系统结构框图](Plugins/RenderCoreTools/imgs/系统结构框图.png)

## 主要功能

- 基于 UE 5.5 的运行时渲染插件。
- Python 客户端通过 TCP Socket 控制 UE 场景。
- 支持 pinhole、fisheye、multi-view fisheye、ERP 和 dome light-stage 相机模型。
- 支持手动、环形和 Fibonacci sphere 三类 dome 相机布局。
- 支持通过 JSON 配置灯光阵列。
- 支持 RGB、Depth、Normal、Position、Semantic 多通道输出。
- 支持 MetaHuman 动态 Level Sequence 逐帧渲染。
- 支持导入 ARKit blendshape JSON，对 MetaHuman 面部动画进行逐帧覆盖。
- 支持导出每帧的相机内参、外参、世界位姿和 rig pose 信息。

## 仓库结构

```text
OmniCaptureUE/
  Config/                         UE 项目配置
  Content/                        Demo 地图、MetaHuman 内容和场景资产
  Plugins/
    RenderCoreExt/                UE 运行时插件
      Content/Materials/          Capture 与 AOV 后处理材质
      Source/RenderCoreExt/       C++ 渲染服务端、相机、数据导出、灯光管理
      shaders/                    Fisheye 和 depth 相关 HLSL shader
      docs/                       开发与验证记录
    RenderCoreTools/              Python 客户端、灯光配置、示例与展示素材
      imgs/                       README 使用的图片与验证视频
      output/                     示例渲染输出和 metadata
  Source/                         UE 项目模块
  dev_uedemo.uproject             UE 5.5 项目文件
```

## RenderCoreExt 插件组成

`RenderCoreExt` 是整个仿真平台的核心插件：

- `ARenderServer`：在 UE 中启动 TCP 服务端，默认监听 `9998` 端口，接收 Python 端 JSON 命令，并分发相机、灯光、位姿和序列采集操作。
- `UFisheyeCameraComponent`：基础相机捕获组件，提供 RGB、Depth、Pose、Fisheye、Cubemap 与导出能力。
- `UMultiViewFisheyeCameraComponent` 和 `UMultiViewERPCameraComponent`：多视角鱼眼与全景捕获组件。
- `UDomeLightStageCameraComponent`：穹顶 light-stage 相机阵列组件，用于多机位、多属性采集。
- `FLightManager`：根据 JSON 创建或更新 area、point、spot 灯光。
- `FDataExporter`：负责 render target readback 与数据导出。

## 渲染效果展示

下面展示的是 MetaHuman ID4 的多属性真值渲染结果。

| RGB | Normal |
| --- | --- |
| ![ID4 RGB](Plugins/RenderCoreTools/imgs/ID4_RGB.png) | ![ID4 Normal](Plugins/RenderCoreTools/imgs/ID4_Normal可视化.png) |

| Semantic | Depth |
| --- | --- |
| ![ID4 Semantic](Plugins/RenderCoreTools/imgs/ID4_Semantic.png) | ![ID4 Depth](Plugins/RenderCoreTools/imgs/ID4_depth可视化.png) |

为了检查 depth 与 position 的准确性，可以将它们恢复为 3D 点云结构。下图可以看到恢复出的几何结构正确。

| Depth 转 3D 点云 | Depth 转 3D 点云 |
| --- | --- |
| ![Depth 点云 1](Plugins/RenderCoreTools/imgs/ID4_depth转3D点云_screenshot1.png) | ![Depth 点云 2](Plugins/RenderCoreTools/imgs/ID4_depth转3D点云_screenshot2.png) |

| Position 转 3D 点云 | Position 转 3D 点云 |
| --- | --- |
| ![Position 点云 1](Plugins/RenderCoreTools/imgs/ID4_position转3D点云_screenshot1.png) | ![Position 点云 2](Plugins/RenderCoreTools/imgs/ID4_position转3D点云_screenshot2.png) |

如果导入连续 MetaHuman 动画资产，例如面部或肢体动画序列，也可以渲染连续的多视角真值视频：

- [ID4 RGB 多视角动画](Plugins/RenderCoreTools/imgs/multiview_rendered_animation_ID4/RGB_video.mp4)
- [ID4 Depth 多视角动画](Plugins/RenderCoreTools/imgs/multiview_rendered_animation_ID4/Depth_video.mp4)
- [ID4 Normal 多视角动画](Plugins/RenderCoreTools/imgs/multiview_rendered_animation_ID4/Normal_video.mp4)
- [ID4 Position 多视角动画](Plugins/RenderCoreTools/imgs/multiview_rendered_animation_ID4/Position_video.mp4)
- [ID4 Semantic 多视角动画](Plugins/RenderCoreTools/imgs/multiview_rendered_animation_ID4/Semantic_video.mp4)

系统也支持替换不同身份的 MetaHuman，例如 ID1、ID2、ID3：

- [MetaHuman ID1](Plugins/RenderCoreTools/imgs/metahuman_ID1.mp4)
- [MetaHuman ID2](Plugins/RenderCoreTools/imgs/metahuman_ID2.mp4)
- [MetaHuman ID3](Plugins/RenderCoreTools/imgs/metahuman_ID3.mp4)
- [ID1 RGB 多视角动画](Plugins/RenderCoreTools/imgs/multiview_rendered_animation_ID1/RGB_video.mp4)
- [ID1 depth 转 3D 点云验证](Plugins/RenderCoreTools/imgs/ID1_depth转换为3D点云可视化.mp4)
- [ID1 position 转 3D 点云验证](Plugins/RenderCoreTools/imgs/ID1_position转换为3D点云可视化.mp4)

## 环境配置

推荐开发环境：

- Windows 10/11
- Unreal Engine 5.5
- Visual Studio 2022，并安装 C++ game development 工具链
- Python 3.10 或更高版本
- 建议使用 Git LFS 管理大型 UE 资产与视频素材

Python 依赖位于 `Plugins/RenderCoreTools/requirements.txt`：

```powershell
cd Plugins/RenderCoreTools
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install -r requirements.txt
```

如果需要使用 OpenCV 读取 EXR 文件，建议设置：

```powershell
$env:OPENCV_IO_ENABLE_OPENEXR = "1"
```

## UE 项目配置

1. 使用 Unreal Engine 5.5 打开 `dev_uedemo.uproject`。
2. 如果 UE 提示重新编译模块，选择编译。
3. 根据需要确认以下插件已启用：MetaHuman、LiveLink、AppleARKitFaceSupport、Movie Render Pipeline、RenderCoreExt。
4. 打开目标地图或目标 Level Sequence。
5. 在关卡中添加 `RenderServer` Actor，或使用已有的 `RenderServer` Actor。
6. 确认 `RenderServer` 的 `ListenPort` 为 `9998`。
7. 添加或选择用于采集的 Actor。默认 Python 客户端使用 Actor 名称 `MyMetaHuman`，相机组件名称 `DomeCam`。
8. 在 UE Editor 中点击 Play，或运行打包后的程序，使 TCP 服务端启动。

UE 日志中应出现类似信息：

```text
RenderServer listening on port 9998
```

## Python 客户端使用

除非使用绝对路径，建议在 `Plugins/RenderCoreTools` 目录下运行 Python 客户端。

### 创建灯光阵列

`create_light.py` 会读取 `dome_light.json`，并向 UE 发送 `create_lights` 命令：

```powershell
cd Plugins/RenderCoreTools
python create_light.py
```

`dome_light.json` 中每个灯光支持以下字段：

- `name`
- `type`：`AREA`、`POINT` 或 `SPOT`
- `enabled`
- `location`
- `rotation_euler`
- `energy`
- `color`
- `use_shadow`
- `area_size` 和 `area_size_y`，用于 area light

### 渲染 MetaHuman Level Sequence

`driveface_sequence_client.py` 是主要的序列采集客户端。它可以配置 dome 相机阵列，逐帧驱动 Level Sequence，可选地导入 ARKit 面部系数，并触发多通道渲染。

基础采集命令：

```powershell
cd Plugins/RenderCoreTools
python driveface_sequence_client.py `
  --host 127.0.0.1 `
  --port 9998 `
  --sequence driveface `
  --capture-actor MyMetaHuman `
  --capture-component DomeCam `
  --export-path "D:/sim_result/driveface_demo" `
  --start-frame 0 `
  --end-frame 120 `
  --frame-step 1
```

采集单帧：

```powershell
python driveface_sequence_client.py `
  --sequence driveface `
  --single-frame 24 `
  --export-path "D:/sim_result/single_frame"
```

导入 ARKit 表情系数：

```powershell
python driveface_sequence_client.py `
  --sequence driveface `
  --arkit-json "D:/data/arkit_sequence.json" `
  --export-path "D:/sim_result/arkit_capture"
```

常用参数说明：

| 参数 | 含义 |
| --- | --- |
| `--host`, `--port` | UE 渲染服务端地址，默认 `127.0.0.1:9998`。 |
| `--sequence` | Level Sequence Actor 的名称或 Label。 |
| `--metahuman-actor` | 用于面部覆盖的 MetaHuman Actor，默认等于 capture actor。 |
| `--capture-actor` | 持有相机组件的 Actor，默认 `MyMetaHuman`。 |
| `--capture-component` | 相机组件名称，默认 `DomeCam`。 |
| `--arkit-json` | 可选 ARKit coefficient sequence JSON。 |
| `--export-path` | 输出目录。如果不提供，脚本使用内部默认路径。 |
| `--start-frame`, `--end-frame` | 采集帧范围。 |
| `--frame-count` | 使用固定帧数采集，可替代 `--end-frame`。 |
| `--frame-step` | 采样步长。 |
| `--wait-frames` | Level Sequence seek 后等待多少个 UE 帧再采集。 |
| `--single-frame` | 只采集指定单帧。 |
| `--seek-only` | 只 seek，不渲染。 |
| `--skip-configure` | 不重新配置相机阵列。 |
| `--keep-cameras` | 配置前保留已有相机组件。 |
| `--resolution` | 每个相机的正方形输出分辨率，默认 `2048`。 |
| `--dome-radius` | Dome 相机半径，单位为 UE 厘米。 |
| `--dome-batch-size` | 每批并行渲染的相机数量。 |
| `--dome-enabled-passes` | 渲染通道 bitmask，默认 `0x1F` 表示全部启用。 |

渲染通道 bitmask：

```text
RGB      0x01
Depth    0x02
Normal   0x04
Position 0x08
Semantic 0x10
All      0x1F
```

例如只渲染 RGB 和 Depth：

```powershell
python driveface_sequence_client.py --dome-enabled-passes 0x03
```

### 手动 Dome 相机阵列

默认客户端会配置一个 17 相机的上半球 dome 阵列：

- 下环 8 个相机
- 上环 8 个相机
- 顶部 1 个相机

相机列表定义在 `driveface_sequence_client.py` 的 `build_manual_dome_cameras()` 中。每个相机格式如下：

```json
{
  "name": "cam_front",
  "pos": [-200, 0, 150],
  "rot": [0, 0, 0],
  "fov": 45
}
```

其中 `pos` 是相对 capture actor 的位置，单位为 UE 厘米；`rot` 为 `[pitch, yaw, roll]`，单位为度；`fov` 为该相机视场角。

## 输出格式

Dome 采集会按帧生成目录：

```text
Frame_0001/
  cam_front_RGB.png
  cam_front_Depth.exr
  cam_front_Normal.exr
  cam_front_Position.exr
  cam_front_Semantic.png
  ...
  dome_cameras_metadata.json
```

仓库中提供了一个示例输出：

```text
Plugins/RenderCoreTools/output/Frame_0001
```

`dome_cameras_metadata.json` 中记录每个相机的标定与文件信息，包括：

- 相机名称与索引
- 图像分辨率
- FOV 与相机内参
- 世界坐标系下的外参
- RGB、Depth、Normal、Position、Semantic 输出文件名

## 构建与打包

仓库中包含两个 Windows 批处理脚本：

- `build_plugin.bat`：使用 Unreal Automation Tool 构建 `RenderCoreExt` 插件。
- `package_project.bat`：打包整个 UE 项目。

这两个脚本中的 UE 引擎路径、项目路径、插件路径和输出路径是本地示例路径，使用前需要按自己的机器环境修改。

典型插件构建命令格式：

```powershell
RunUAT.bat BuildPlugin `
  -Plugin="D:/path/to/OmniCaptureUE/Plugins/RenderCoreExt/RenderCoreExt.uplugin" `
  -Package="D:/Release/RenderCoreExt_Plugin_v1.0" `
  -Rocket `
  -TargetPlatforms=Win64
```

## 使用注意事项

- Python 客户端发送命令时，UE 项目必须处于运行状态。
- 输出路径需要有写入权限，并预留足够磁盘空间；多相机 EXR 数据体积会快速增长。
- 高分辨率 dome 序列渲染耗时较长，可以根据机器性能调整 `--dome-batch-size`、`--resolution` 和 `--hold-seconds`。
- Depth 和 Position 以高精度 EXR 输出，可以恢复为 3D 点云，用于验证几何准确性。
- Semantic 渲染会根据 MetaHuman mesh 与材质状态，使用 vertex color 或材质替换路径生成语义图。

