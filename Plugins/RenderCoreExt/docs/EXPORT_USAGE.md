# RGB/Depth/Pose 导出功能使用说明

## 📁 保存位置

### 默认保存位置
- **默认路径**: `C:/UE5_Exports/`
- 可以通过Python客户端或UE5编辑器中的组件属性设置

### 文件命名格式
保存的文件会按照以下格式命名：
- **RGB图像**: `{BasePath}_rgb_{FrameNumber:05d}.png`
- **深度图**: `{BasePath}_depth_{FrameNumber:05d}.png`
- **位姿文件**: `{BasePath}_pose_{FrameNumber:05d}.json`

### 示例
如果设置 `BasePath = "C:/UE5_Exports/dataset_001/frame_00001"`，会生成：
- `C:/UE5_Exports/dataset_001/frame_00001_rgb_00001.png`
- `C:/UE5_Exports/dataset_001/frame_00001_depth_00001.png`
- `C:/UE5_Exports/dataset_001/frame_00001_pose_00001.json`

---

## 🐍 Python客户端控制

### 方法1：在set_pose时自动保存（推荐）

```python
# 设置导出路径
set_export_path(sock, "CameraRigActor", "C:/MyExports/dataset_001")

# 每次set_pose时自动保存
for i in range(100):
    frame_path = f"C:/MyExports/dataset_001/frame_{i:05d}"
    set_pose(sock, "CameraRigActor", 
             position=[x, y, z], 
             rotation_quat=[qx, qy, qz, qw],
             export_path=frame_path,  # 指定保存路径
             frame_number=i)          # 指定帧号
```

### 方法2：手动控制保存时机

```python
# 设置默认导出路径
set_export_path(sock, "CameraRigActor", "C:/MyExports/dataset_001")

# 运动控制
for i in range(100):
    set_pose(sock, "CameraRigActor", position=[x, y, z], rotation_quat=[qx, qy, qz, qw])
    
    # 每10帧保存一次
    if i % 10 == 0:
        save_frame(sock, "CameraRigActor", 
                  export_path=f"C:/MyExports/dataset_001/frame_{i:05d}",
                  frame_number=i)
```

### 方法3：使用相机内部计数器

```python
# 不指定frame_number，使用相机内部计数器
set_pose(sock, "CameraRigActor", 
         position=[x, y, z], 
         rotation_quat=[qx, qy, qz, qw],
         export_path="C:/MyExports/frame")
# 会自动使用相机内部的FrameCounter
```

---

## 🎮 UE5编辑器控制

### 在组件Details面板中设置

1. 选中包含 `FisheyeCameraComponent` 的Actor
2. 在Details面板找到 `Fisheye` 分类下的 `Export` 子分类
3. 设置以下属性：
   - **Export Base Path**: 设置保存基础路径（如 `C:/UE5_Exports/`）
   - **Auto Save On Capture**: 勾选后每次调用 `CaptureFisheyeScene()` 时自动保存

### 在蓝图中调用

1. 找到 `FisheyeCameraComponent` 节点
2. 调用以下函数：
   - `Save RGB Image` - 保存RGB图像
   - `Save Depth Image` - 保存深度图
   - `Save Pose` - 保存位姿
   - `Save All Data` - 保存所有数据

---

## 📋 Python API参考

### `set_pose(sock, actor_name, position, rotation_quat, export_path=None, frame_number=None)`

设置Actor位姿，可选择是否保存数据。

**参数**:
- `sock`: socket连接对象
- `actor_name`: Actor名称（如 "CameraRigActor"）
- `position`: 位置列表 `[x, y, z]`
- `rotation_quat`: 旋转四元数 `[x, y, z, w]`
- `export_path`: (可选) 如果提供，会自动保存RGB/Depth/Pose
- `frame_number`: (可选) 帧号，如果不提供则使用相机内部计数器

### `set_export_path(sock, actor_name, export_path)`

设置默认导出路径。

**参数**:
- `sock`: socket连接对象
- `actor_name`: Actor名称
- `export_path`: 导出基础路径

### `save_frame(sock, actor_name, export_path=None, frame_number=None)`

手动保存当前帧。

**参数**:
- `sock`: socket连接对象
- `actor_name`: Actor名称
- `export_path`: (可选) 保存路径，如果不提供则使用之前设置的路径
- `frame_number`: (可选) 帧号

---

## 📄 位姿JSON格式

保存的位姿文件是JSON格式，包含以下信息：

```json
{
  "frame": 1,
  "position": {
    "x": 1420.0,
    "y": 1420.0,
    "z": 300.0
  },
  "rotation": {
    "x": 0.0,
    "y": 0.0,
    "z": 0.7071068,
    "w": 0.7071068
  },
  "rotation_euler": {
    "pitch": 0.0,
    "yaw": 90.0,
    "roll": 0.0
  }
}
```

- `frame`: 帧号
- `position`: 位置（UE5坐标系：X-前，Y-右，Z-上）
- `rotation`: 旋转四元数 `[x, y, z, w]`
- `rotation_euler`: 欧拉角（方便阅读）

---

## ⚠️ 注意事项

1. **路径格式**: 使用正斜杠 `/` 或双反斜杠 `\\`，Windows路径示例：`C:/MyExports/` 或 `C:\\MyExports\\`

2. **目录权限**: 确保保存目录有写入权限

3. **性能**: 保存操作在主线程执行，大量保存可能影响帧率。建议：
   - 不要每帧都保存（除非必要）
   - 使用异步保存（未来版本可能支持）

4. **深度图格式**: 深度图保存为8位灰度PNG，深度值已归一化到0-255范围

5. **RGB图像格式**: RGB图像保存为PNG格式，包含完整的颜色信息

---

## 🔍 故障排除

### 问题：文件没有保存

**检查**:
1. 确认路径正确且有写入权限
2. 检查UE5日志中是否有错误信息
3. 确认 `FisheyeCameraComponent` 已正确附加到Actor上

### 问题：深度图全黑或全白

**可能原因**:
1. 深度捕获未正确配置
2. 场景中没有深度信息（所有物体都在无限远）
3. 深度值范围异常

**解决方法**:
- 检查 `DepthRenderTarget` 是否正确创建
- 确认场景中有可渲染的物体
- 查看UE5日志中的深度范围信息

---

## 📝 完整示例

参考 `python_client_example.py` 文件，包含完整的使用示例。
