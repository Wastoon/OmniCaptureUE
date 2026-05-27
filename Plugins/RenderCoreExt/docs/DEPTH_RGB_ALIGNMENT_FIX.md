# RGB和深度对齐问题修复指南

## 问题分析

你遇到的问题是：**RGB和深度图的像素不对应，FOV也不一样**。

### 根本原因

1. **RGB材质**：做的是**去畸变**（从鱼眼图像恢复到正常透视图像）
2. **深度节点**：做的是**正向畸变**（从屏幕UV计算3D方向）

这两个过程是**相反的**，导致：
- RGB显示的是去畸变后的正常透视图像
- 深度显示的是鱼眼畸变图像
- 两者像素不对应

## 解决方案

### 方案1：两者都做去畸变（推荐）

让RGB和深度都使用**相同的去畸变逻辑**，这样它们都会显示正常透视图像，并且像素严格对应。

#### RGB材质Custom节点（已修正）

使用 `FisheyeRGBPostProcess_CORRECTED.hlsl` 中的代码，关键点：
- 输入：ScreenUV（鱼眼图像的UV）
- 输出：OriginalUV（去畸变后的UV，用于采样SceneTexture）
- 逻辑：从r_d反推theta，计算去畸变后的UV

#### 深度Cubemap采样节点（已修正）

使用 `FisheyeDepthFromCubemap_CORRECTED.hlsl` 中的代码，关键点：
- 输入：ScreenUV（鱼眼图像的UV）
- 输出：RayDirection（3D射线方向，用于从Cubemap采样）
- 逻辑：**使用与RGB完全相同的去畸变逻辑**，然后转换为3D方向

### 关键修正点

1. **移除Y轴翻转**：
   ```hlsl
   // 错误（深度节点中）：
   float2 FlippedUV = float2(ScreenUV.x, 1.0 - ScreenUV.y);
   
   // 正确（与RGB保持一致）：
   float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);
   ```

2. **使用相同的去畸变逻辑**：
   - 两者都从 `r_d` 反推 `theta`
   - 两者都计算 `OriginalNormalized`
   - RGB：转换为UV用于采样SceneTexture
   - 深度：转换为3D方向用于采样Cubemap

3. **确保参数一致**：
   - Fx, Fy, Cx, Cy, K1-K4 必须完全相同
   - ImageWidth, ImageHeight 必须完全相同

## 材质设置步骤

### 1. RGB后处理材质

1. 创建Post Process材质
2. 添加Custom节点，粘贴 `FisheyeRGBPostProcess_CORRECTED.hlsl` 代码
3. 设置输入：
   - ScreenUV → TextureCoordinate
   - Fx, Fy, Cx, Cy, K1-K4 → 标量参数
   - ImageWidth, ImageHeight → 标量参数
4. 输出 → SceneTexture(PostProcessInput0) 的 UVs
5. SceneTexture的Color → Emissive Color

### 2. 深度Cubemap采样材质

1. 创建Material（不是Post Process）
2. 添加Custom节点，粘贴 `FisheyeDepthFromCubemap_CORRECTED.hlsl` 代码
3. 设置输入（与RGB材质完全相同）：
   - ScreenUV → TextureCoordinate
   - Fx, Fy, Cx, Cy, K1-K4 → 标量参数
   - ImageWidth, ImageHeight → 标量参数
4. 输出 → SampleCubemap 的 Direction
5. SampleCubemap的Texture → DepthCubemapRenderTarget
6. SampleCubemap的Color → Emissive Color

### 3. 确保参数同步

在C++代码中，确保RGB和深度材质使用相同的参数：

```cpp
// RGB材质参数
DynamicMaterial->SetScalarParameterValue("Fx", FisheyeParameters.Fx);
// ... 其他参数

// 深度材质参数（必须完全相同）
CubemapDepthMaterial->SetScalarParameterValue("Fx", FisheyeParameters.Fx);
// ... 其他参数
```

## 验证方法

1. **检查FOV**：
   - RGB和深度图的视野应该完全一致
   - 如果RGB是去畸变的正常透视，深度也应该是去畸变的正常透视

2. **检查像素对应**：
   - 在RGB图像上选择一个特征点（如物体边缘）
   - 在深度图上相同位置应该对应相同的深度值
   - 如果不对应，说明UV计算不一致

3. **检查边缘对齐**：
   - RGB和深度的物体边缘应该完全对齐
   - 如果RGB边缘清晰但深度边缘模糊（或相反），说明采样位置不对

## 常见问题

### Q1: 为什么深度图全黑/全白？

**A**: 可能是：
1. Cubemap没有正确捕获
2. 射线方向计算错误
3. Cubemap采样方向不对

**解决**：
- 检查 `CaptureDepthCubemap()` 是否被调用
- 检查Cubemap的捕获位置是否与相机位置一致
- 检查射线方向的坐标系是否正确（UE5: X-Forward, Y-Right, Z-Up）

### Q2: RGB和深度还是不对应？

**A**: 检查：
1. 两个材质的参数是否完全相同
2. ImageWidth和ImageHeight是否相同
3. UV计算逻辑是否完全一致（特别是Y轴处理）

### Q3: FOV不一样？

**A**: 确保：
1. 两个材质使用相同的相机内参（Fx, Fy）
2. 两个材质使用相同的图像尺寸（ImageWidth, ImageHeight）
3. 去畸变逻辑完全一致

## 调试技巧

1. **添加调试输出**：
   - 在Custom节点中，可以临时返回中间值（如r_d, theta）来检查计算是否正确

2. **对比UV值**：
   - 在RGB和深度材质中，输出相同的中间值（如OriginalUV），应该得到相同的结果

3. **检查边界**：
   - 在图像中心、边缘、角落分别检查，确保所有位置都对应

## 总结

关键是要让RGB和深度使用**完全相同的去畸变逻辑**，这样它们才会：
- 显示相同的视野（FOV）
- 像素严格对应
- 边缘对齐

使用提供的修正代码，确保两个Custom节点的逻辑完全一致即可。
