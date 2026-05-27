# 正确的鱼眼深度图生成方法

## 核心原理

要获得**正确的鱼眼深度图**，不能对深度缓冲区做2D UV重映射。正确的方法是：

1. **使用Cubemap捕获场景深度**（6个方向）
2. **从鱼眼像素坐标计算真实的3D射线方向**
3. **使用3D射线方向从Cubemap采样深度**

这样得到的深度是**真实的3D空间深度**，而不是2D重映射的结果。

## 工作流程

### 1. 捕获阶段

```
SceneCaptureComponent2D (RGB)
  ↓
正常透视渲染 → SceneTexture (PostProcessInput0)
  ↓
RGB后处理材质（去畸变）→ 输出正常透视RGB图像
```

```
SceneCaptureComponentCube (Depth)
  ↓
6个方向渲染 → Cubemap深度纹理
  ↓
深度材质（从Cubemap采样）→ 输出鱼眼深度图
```

### 2. RGB处理流程

**目标**：从鱼眼像素坐标 → 正常透视UV → 采样SceneTexture

1. 输入：ScreenUV（鱼眼图像的UV，0-1）
2. 计算：从鱼眼UV反推正常透视UV（去畸变）
3. 输出：正常透视UV
4. 采样：使用正常透视UV采样SceneTexture
5. 结果：去畸变的正常透视RGB图像

### 3. 深度处理流程

**目标**：从鱼眼像素坐标 → 3D射线方向 → 从Cubemap采样深度

1. 输入：ScreenUV（鱼眼图像的UV，0-1）
2. 计算：从鱼眼UV计算3D射线方向（使用Kannala-Brandt模型）
3. 输出：3D射线方向（RayDirection）
4. 采样：使用3D方向从Cubemap采样深度
5. 结果：鱼眼深度图（与RGB对应）

## 关键代码

### 深度Cubemap采样节点

```hlsl
// 从鱼眼像素坐标计算3D射线方向
float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);
float2 NormalizedCoord = (PixelCoord - float2(Cx, Cy)) / float2(Fx, Fy);
float r_d = length(NormalizedCoord);

// 从r_d反推theta（逆畸变）
float theta = ...; // 牛顿迭代求解

// 计算方位角
float phi = atan2(NormalizedCoord.y, NormalizedCoord.x);

// 转换为3D射线方向
float3 RayDirection = float3(
    cos(theta),
    sin(theta) * cos(phi),
    sin(theta) * sin(phi)
);

return RayDirection; // 用于Cubemap采样
```

### RGB后处理节点

```hlsl
// 从鱼眼像素坐标计算正常透视UV（去畸变）
float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);
float2 NormalizedCoord = (PixelCoord - float2(Cx, Cy)) / float2(Fx, Fy);
float r_d = length(NormalizedCoord);

// 从r_d反推theta（逆畸变）
float theta = ...; // 牛顿迭代求解

// 计算去畸变后的归一化坐标
float r = theta;
float2 OriginalNormalized = NormalizedCoord * (r / r_d);

// 转换回UV
float2 OriginalUV = ...;

return OriginalUV; // 用于SceneTexture采样
```

## 为什么这样是正确的？

### 传统错误方法（2D UV重映射）

```
鱼眼深度图 = 对正常透视深度图做UV重映射
```

**问题**：
- 深度值本身是3D空间的属性
- 2D重映射会破坏深度的几何意义
- 不同方向的深度值被错误地混合

### 正确方法（3D射线采样）

```
鱼眼深度图 = 从Cubemap按3D射线方向采样
```

**优势**：
- 每个鱼眼像素对应真实的3D射线方向
- 从Cubemap采样的是该方向的真实深度
- 深度值保持几何正确性

## 材质设置

### RGB后处理材质

1. **Material Domain**: Post Process
2. **Custom节点**：
   - 输入：ScreenUV, Fx, Fy, Cx, Cy, K1-K4, ImageWidth, ImageHeight
   - 输出：OriginalUV（去畸变后的UV）
3. **SceneTexture节点**：
   - UVs输入：连接Custom节点的输出
   - SceneTextureId: PostProcessInput0
4. **输出**：SceneTexture的Color → Emissive Color

### 深度Cubemap采样材质

1. **Material Domain**: Surface（或Post Process）
2. **Custom节点**：
   - 输入：ScreenUV, Fx, Fy, Cx, Cy, K1-K4, ImageWidth, ImageHeight
   - 输出：RayDirection（3D方向）
3. **SampleCubemap节点**：
   - Direction输入：连接Custom节点的输出
   - Texture输入：DepthCubemapRenderTarget
4. **输出**：SampleCubemap的Color → Emissive Color（或Depth输出）

## 验证方法

### 1. 检查深度值合理性

- 深度值应该随距离单调递增
- 不应该出现异常的跳跃或突变
- 边缘区域的深度应该平滑过渡

### 2. 检查RGB和深度对应

- 在RGB图像上选择一个特征点
- 在深度图上相同位置应该对应合理的深度值
- 如果RGB是去畸变的正常透视，深度也应该是去畸变的正常透视

### 3. 检查几何一致性

- 使用深度图重建3D点云
- 3D点应该与场景几何一致
- 不应该出现明显的扭曲或变形

## 常见问题

### Q1: RGB和深度FOV不一样？

**A**: 确保两个材质使用：
- 相同的相机内参（Fx, Fy, Cx, Cy）
- 相同的图像尺寸（ImageWidth, ImageHeight）
- 相同的畸变系数（K1-K4）

### Q2: 深度图全黑/全白？

**A**: 检查：
1. Cubemap是否正确捕获
2. 射线方向计算是否正确
3. Cubemap采样方向是否正确（UE5坐标系：X-Forward, Y-Right, Z-Up）

### Q3: 边缘深度值异常？

**A**: 可能是：
1. 射线方向超出Cubemap范围
2. 需要处理边界情况（saturate或clamp）
3. Cubemap分辨率不够高

## 总结

正确的鱼眼深度图生成流程：

1. ✅ 使用Cubemap捕获6个方向的深度
2. ✅ 从鱼眼像素坐标计算真实的3D射线方向
3. ✅ 使用3D射线方向从Cubemap采样深度
4. ✅ 确保RGB和深度使用相同的相机参数

**不要**：
- ❌ 对深度缓冲区做2D UV重映射
- ❌ 使用不同的畸变参数
- ❌ 忽略3D几何关系
