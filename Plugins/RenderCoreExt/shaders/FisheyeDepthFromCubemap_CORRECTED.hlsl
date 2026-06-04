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
// 修正版：深度Cubemap采样节点
// 与RGB材质使用相同的去畸变逻辑，确保像素对应
// ============================================

// 输入：屏幕UV坐标 (0-1范围)
float2 ScreenUV = ScreenUV;

// 输入：相机内参
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

// ========== 关键修正：使用与RGB材质完全相同的去畸变逻辑 ==========

// 1. 将屏幕UV转换为像素坐标（注意：不需要翻转Y，与RGB保持一致）
float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);

// 2. 转换为以主点为中心的归一化坐标
float2 NormalizedCoord = (PixelCoord - float2(Cx, Cy)) / float2(Fx, Fy);

// 3. 计算畸变后的径向距离 r_d（这是鱼眼图像上的径向距离）
float x = NormalizedCoord.x;
float y = NormalizedCoord.y;
float r_d = length(NormalizedCoord);

// 4. 使用Kannala-Brandt模型求解theta（逆畸变，与RGB材质完全一致）
// 从r_d反推theta，使用牛顿迭代法
float theta = r_d; // 初始猜测
float theta2, theta4, theta6, theta8;
float f, f_prime;
const int max_iterations = 10;
const float tolerance = 0.0001;

for (int i = 0; i < max_iterations; i++)
{
    theta2 = theta * theta;
    theta4 = theta2 * theta2;
    theta6 = theta4 * theta2;
    theta8 = theta6 * theta2;
    
    // f(theta) = theta * (1 + k1*theta^2 + k2*theta^4 + k3*theta^6 + k4*theta^8) - r_d
    f = theta * (1.0 + K1 * theta2 + K2 * theta4 + K3 * theta6 + K4 * theta8) - r_d;
    
    // f'(theta) = 1 + 3*k1*theta^2 + 5*k2*theta^4 + 7*k3*theta^6 + 9*k4*theta^8
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

// 5. 计算原始径向距离 r = theta（去畸变后的）
float r = theta;

// 6. 计算原始归一化坐标（去畸变后的，对应正常透视图像）
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

// 7. 计算方位角 phi
float phi = atan2(OriginalNormalized.y, OriginalNormalized.x);

// 8. 将去畸变后的坐标转换为3D射线方向（用于从Cubemap采样）
// 对于等距投影：r = theta，所以可以直接使用theta
float sinTheta = sin(theta);
float cosTheta = cos(theta);

// UE5 Cubemap坐标系: X-Forward, Y-Right, Z-Up
// 从去畸变后的归一化坐标计算3D方向
float3 RayDirection = float3(
    cosTheta,                    // X (Forward)
    sinTheta * cos(phi),         // Y (Right)
    sinTheta * sin(phi)          // Z (Up)
);

// 9. 从Cubemap采样深度（使用计算出的射线方向）
// 注意：这里返回的是3D方向，材质中应该使用SampleCubemap节点采样
return RayDirection;
