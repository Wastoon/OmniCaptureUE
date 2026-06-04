/*
 * Copyright (c) 2026 mengrongye.
 *
 * Author: mengrongye
 * Contact: mengrongye@gmail.com
 * Project: OmniCaptureUE / RenderCoreExt
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// ============================================
// 修正版：RGB后处理材质
// 与深度节点使用相同的去畸变逻辑，确保像素对应
// ============================================

// 输入参数
float2 ScreenUV = ScreenUV;
float Fx = Fx;
float Fy = Fy;
float Cx = Cx;
float Cy = Cy;
float ImageWidth = ImageWidth;
float ImageHeight = ImageHeight;
float K1 = K1;
float K2 = K2;
float K3 = K3;
float K4 = K4;

// ========== 去畸变逻辑（与深度节点完全一致） ==========

// 1. 将屏幕UV转换为像素坐标
float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);

// 2. 转换为以主点为中心的归一化坐标
float2 NormalizedCoord = (PixelCoord - float2(Cx, Cy)) / float2(Fx, Fy);

// 3. 计算畸变后的径向距离 r_d
float x = NormalizedCoord.x;
float y = NormalizedCoord.y;
float r_d = length(NormalizedCoord);

// 4. 使用Kannala-Brandt模型求解theta（逆畸变）
float theta = r_d; // 初始猜测
float theta2, theta4, theta6, theta8;
float f, f_prime;
const int max_iterations = 10;
const float tolerance = 0.0001;

// 牛顿迭代求解theta
for (int i = 0; i < max_iterations; i++)
{
    theta2 = theta * theta;
    theta4 = theta2 * theta2;
    theta6 = theta4 * theta2;
    theta8 = theta6 * theta2;
    
    f = theta * (1.0 + K1 * theta2 + K2 * theta4 + K3 * theta6 + K4 * theta8) - r_d;
    f_prime = 1.0 + 3.0 * K1 * theta2 + 5.0 * K2 * theta4 + 7.0 * K3 * theta6 + 9.0 * K4 * theta8;
    
    if (abs(f_prime) < 0.0001)
        break;
    
    float theta_new = theta - f / f_prime;
    
    if (abs(theta_new - theta) < tolerance)
    {
        theta = theta_new;
        break;
    }
    
    theta = max(theta_new, 0.0);
}

// 5. 计算原始径向距离 r = theta
float r = theta;

// 6. 计算原始归一化坐标（去畸变后的）
float2 OriginalNormalized;
if (r_d > 0.0001)
{
    float scale = r / r_d;
    OriginalNormalized = NormalizedCoord * scale;
}
else
{
    OriginalNormalized = float2(0.0, 0.0);
}

// 7. 转换回像素坐标
float2 OriginalPixel = OriginalNormalized * float2(Fx, Fy) + float2(Cx, Cy);

// 8. 归一化到UV坐标
float2 OriginalUV = OriginalPixel / float2(ImageWidth, ImageHeight);

// 9. 限制在0-1范围
OriginalUV = saturate(OriginalUV);

// 10. 使用去畸变后的UV采样SceneTexture
// 注意：这里返回的是UV坐标，材质中应该连接到SceneTexture的UVs输入
return OriginalUV;
