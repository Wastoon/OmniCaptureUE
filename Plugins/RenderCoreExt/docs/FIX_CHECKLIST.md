# RGB和深度对齐修复检查清单

## 已修复的C++代码问题 ✅

1. **参数传递不一致** - 已修复
   - 深度材质现在使用`TextureTarget`的尺寸，而不是`DepthRenderTarget`的尺寸
   - 确保`DepthRenderTarget`尺寸与`TextureTarget`一致

2. **添加了调试日志** - 已添加
   - 输出深度材质的参数值，方便调试

## 需要在材质编辑器中修复的问题 ⚠️

### 1. 深度节点中的Y轴翻转

**当前问题**：
```hlsl
float2 FlippedUV = float2(ScreenUV.x, 1.0 - ScreenUV.y);  // ❌ 有翻转
float2 PixelCoord = FlippedUV * float2(ImageWidth, ImageHeight);
```

**应该改为**（与RGB一致）：
```hlsl
float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);  // ✅ 无翻转
```

**操作步骤**：
1. 打开深度材质（`FisheyeDepthFromCubemapMaterial`）
2. 找到Custom节点（计算RayDirection的节点）
3. 删除或注释掉`FlippedUV`相关代码
4. 直接使用`ScreenUV`，与RGB材质保持一致

### 2. Surface材质的UV坐标系

**问题**：Surface材质在`DrawMaterialToRenderTarget`时，UV坐标系可能与Post Process不同。

**解决方案A**（推荐）：
- 如果发现深度图上下颠倒，在材质编辑器中：
  1. 在Custom节点前添加一个节点
  2. 使用`TextureCoordinate`节点获取UV
  3. 如果需要翻转Y，添加`Append`和`Subtract`节点：`float2(UV.x, 1.0 - UV.y)`
  4. 将翻转后的UV传递给Custom节点的`ScreenUV`输入

**解决方案B**：
- 如果Surface材质的UV已经是正确的（不需要翻转），直接使用`TextureCoordinate`的输出

### 3. 确保参数名称一致

检查两个材质中的参数名称是否完全相同：
- `Fx`, `Fy`, `Cx`, `Cy`
- `K1`, `K2`, `K3`, `K4`
- `ImageWidth`, `ImageHeight`

## 验证步骤

### 步骤1：检查尺寸一致性

运行游戏，查看日志：
```
Depth material params - Fx:500.0 Fy:500.0 Cx:640.0 Cy:360.0 Size:1280x720
```

确认：
- ImageWidth和ImageHeight与RGB材质相同
- DepthRenderTarget已调整到与TextureTarget相同的尺寸

### 步骤2：检查UV处理

1. 在RGB图像中心选择一个特征点
2. 在深度图相同位置检查深度值
3. 如果上下颠倒，应用解决方案A

### 步骤3：检查像素对应

1. 在RGB图像上选择一个明显的特征（如物体边缘）
2. 在深度图上相同位置应该对应相同的深度值
3. 如果不对应，检查Custom节点的逻辑是否完全一致

## 修正后的深度节点代码

使用`FisheyeDepthFromCubemap_FIXED.hlsl`中的代码：

```hlsl
// 与RGB材质完全相同的UV处理
float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);

// 与RGB材质完全相同的归一化坐标计算
float2 NormalizedCoord = (PixelCoord - float2(Cx, Cy)) / float2(Fx, Fy);

// 与RGB材质完全相同的去畸变逻辑
float r_d = length(NormalizedCoord);
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

## 如果仍然不对应

### 可能的原因和解决方案

1. **Surface材质的UV坐标系不同**
   - 在材质编辑器中，在Custom节点前添加UV变换
   - 测试是否需要翻转Y轴

2. **Cubemap采样方向错误**
   - 检查射线方向的坐标系（UE5: X-Forward, Y-Right, Z-Up）
   - 检查Cubemap捕获位置是否与相机位置一致

3. **材质参数未更新**
   - 确保每次`CaptureFisheyeScene()`时都更新材质参数
   - 检查动态材质实例是否正确创建

## 快速修复清单

- [ ] 更新深度节点的Custom代码，移除Y轴翻转
- [ ] 确保深度材质使用与RGB材质相同的参数名称
- [ ] 如果深度图上下颠倒，在材质编辑器中添加Y轴翻转节点
- [ ] 验证ImageWidth和ImageHeight参数一致
- [ ] 验证Fx, Fy, Cx, Cy, K1-K4参数一致
- [ ] 测试RGB和深度图的像素对应关系
