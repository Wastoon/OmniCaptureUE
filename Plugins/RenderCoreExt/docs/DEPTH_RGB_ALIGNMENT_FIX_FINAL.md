# RGB和深度对齐问题 - 最终修复方案

## 发现的问题

### 1. Y轴翻转不一致 ⚠️

**深度节点**（当前）：
```hlsl
float2 FlippedUV = float2(ScreenUV.x, 1.0 - ScreenUV.y);
float2 PixelCoord = FlippedUV * float2(ImageWidth, ImageHeight);
```

**RGB节点**（当前）：
```hlsl
float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);
```

**问题**：深度节点有Y轴翻转，RGB节点没有，导致上下颠倒。

### 2. 材质域不同导致UV坐标系不同 ⚠️

- **RGB材质**：Post Process域，ScreenUV是后处理空间的UV
- **深度材质**：Surface域，使用`DrawMaterialToRenderTarget`，ScreenUV可能是不同的坐标系

**问题**：不同材质域的UV坐标系可能不同，导致不对应。

### 3. 参数传递不一致 ⚠️

**RGB材质**（第69-70行）：
```cpp
DynamicMaterial->SetScalarParameterValue("ImageWidth", TextureTarget ? (float)TextureTarget->SizeX : 1280.0f);
DynamicMaterial->SetScalarParameterValue("ImageHeight", TextureTarget ? (float)TextureTarget->SizeY : 720.0f);
```

**深度材质**（第346-347行）：
```cpp
CubemapDepthMaterial->SetScalarParameterValue("ImageWidth", (float)DepthRenderTarget->SizeX);
CubemapDepthMaterial->SetScalarParameterValue("ImageHeight", (float)DepthRenderTarget->SizeY);
```

**问题**：如果`TextureTarget`和`DepthRenderTarget`尺寸不同，会导致FOV不同。

## 修复方案

### 1. 统一Y轴处理

**深度节点**应该与RGB节点使用相同的UV处理：
```hlsl
// 移除FlippedUV，直接使用ScreenUV
float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);
```

### 2. 统一参数传递

在`RenderFisheyeDepthFromCubemap()`中，使用与RGB材质相同的尺寸：
```cpp
// 使用TextureTarget的尺寸，确保与RGB一致
float ImageWidth = TextureTarget ? (float)TextureTarget->SizeX : 1280.0f;
float ImageHeight = TextureTarget ? (float)TextureTarget->SizeY : 720.0f;

CubemapDepthMaterial->SetScalarParameterValue("ImageWidth", ImageWidth);
CubemapDepthMaterial->SetScalarParameterValue("ImageHeight", ImageHeight);

// 确保深度渲染目标尺寸与RGB一致
if (DepthRenderTarget && TextureTarget)
{
    if (DepthRenderTarget->SizeX != TextureTarget->SizeX || 
        DepthRenderTarget->SizeY != TextureTarget->SizeY)
    {
        DepthRenderTarget->ResizeTarget(TextureTarget->SizeX, TextureTarget->SizeY);
    }
}
```

### 3. 处理Surface材质的UV坐标系

如果Surface材质在`DrawMaterialToRenderTarget`时UV坐标系不同，可以在材质编辑器中：
- 使用`TextureCoordinate`节点获取UV
- 如果需要翻转Y，在材质图中添加翻转节点，而不是在Custom节点中

## 修正后的代码

### 深度Cubemap采样节点（`FisheyeDepthFromCubemap_FIXED.hlsl`）

```hlsl
// 与RGB材质完全相同的UV处理
float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);
float2 NormalizedCoord = (PixelCoord - float2(Cx, Cy)) / float2(Fx, Fy);
float r_d = length(NormalizedCoord);

// 与RGB材质完全相同的去畸变逻辑
float theta = r_d;
// ... 牛顿迭代求解theta（与RGB完全相同）

// 计算3D射线方向
float phi = atan2(NormalizedCoord.y, NormalizedCoord.x);
float3 RayDirection = float3(
    cos(theta),
    sin(theta) * cos(phi),
    sin(theta) * sin(phi)
);

return RayDirection;
```

### C++代码修正

已在`RenderFisheyeDepthFromCubemap()`中：
1. 使用`TextureTarget`的尺寸而不是`DepthRenderTarget`的尺寸
2. 确保`DepthRenderTarget`尺寸与`TextureTarget`一致
3. 添加日志输出以便调试

## 验证步骤

1. **检查尺寸一致性**：
   - 查看日志，确认RGB和深度材质使用相同的ImageWidth和ImageHeight
   - 确认`DepthRenderTarget`和`TextureTarget`尺寸相同

2. **检查参数一致性**：
   - 查看日志，确认Fx, Fy, Cx, Cy, K1-K4完全相同

3. **检查UV处理**：
   - 如果深度图上下颠倒，说明Surface材质的UV坐标系不同
   - 解决方案：在材质编辑器中，在Custom节点前添加Y轴翻转节点

4. **检查像素对应**：
   - 在RGB图像中心选择一个特征点
   - 在深度图相同位置应该对应相同的深度值
   - 如果不对应，检查Custom节点的逻辑是否完全一致

## 如果仍然不对应

### 可能的原因

1. **Surface材质的UV坐标系不同**：
   - `DrawMaterialToRenderTarget`可能使用不同的UV坐标系
   - 解决方案：在材质编辑器中，在Custom节点前添加UV变换节点

2. **Cubemap采样方向错误**：
   - 检查射线方向的坐标系是否正确（UE5: X-Forward, Y-Right, Z-Up）
   - 检查Cubemap的捕获位置是否与相机位置一致

3. **材质参数未更新**：
   - 确保每次`CaptureFisheyeScene()`时都更新材质参数
   - 检查动态材质实例是否正确创建

## 调试建议

1. **添加调试输出**：
   - 在Custom节点中，可以临时返回中间值（如r_d, theta）来检查计算是否正确

2. **对比中间值**：
   - 在RGB和深度材质中，输出相同的中间值（如NormalizedCoord），应该得到相同的结果

3. **检查边界情况**：
   - 在图像中心、边缘、角落分别检查，确保所有位置都对应

## 总结

关键修复点：
1. ✅ 移除深度节点中的Y轴翻转
2. ✅ 使用相同的ImageWidth和ImageHeight（来自TextureTarget）
3. ✅ 确保DepthRenderTarget尺寸与TextureTarget一致
4. ✅ 确保两个Custom节点使用完全相同的去畸变逻辑

如果修复后仍然不对应，可能是Surface材质的UV坐标系问题，需要在材质编辑器中调整。
