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
// 正确版：RGB后处理材质
// 从鱼眼像素坐标计算正常透视图像的UV，用于采样SceneTexture
// 这样输出的RGB图像是去畸变的正常透视图像
// ============================================

// 输入参数
float2 ScreenUV = ScreenUV;  // 鱼眼图像的UV (0-1)
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

// ========== 从鱼眼像素坐标计算正常透视UV（去畸变） ==========

// 1. 将屏幕UV转换为像素坐标（鱼眼图像上的像素位置）
float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);

// 2. 转换为以主点为中心的归一化坐标（鱼眼图像平面上的坐标）
float2 NormalizedCoord = (PixelCoord - float2(Cx, Cy)) / float2(Fx, Fy);

// 3. 计算鱼眼图像上的径向距离 r_d
float r_d = length(NormalizedCoord);

// 4. 使用Kannala-Brandt模型从r_d反推theta（逆畸变）
// r_d = theta * (1 + k1*theta^2 + k2*theta^4 + k3*theta^6 + k4*theta^8)
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

// 5. 计算去畸变后的归一化坐标（正常透视图像平面上的坐标）
// 对于等距投影模型：r = theta
float r = theta;

// 6. 计算去畸变后的归一化坐标（保持方向不变）
float2 OriginalNormalized;
if (r_d > 0.0001)
{
    float scale = r / r_d;
    OriginalNormalized = NormalizedCoord * scale;
}
else
{
    OriginalNormalized = float2(0.0, 0.0); // 中心点
}

// 7. 转换回像素坐标（正常透视图像的像素坐标）
float2 OriginalPixel = OriginalNormalized * float2(Fx, Fy) + float2(Cx, Cy);

// 8. 归一化到UV坐标（正常透视图像的UV）
float2 OriginalUV = OriginalPixel / float2(ImageWidth, ImageHeight);

// 9. 限制在0-1范围
OriginalUV = saturate(OriginalUV);

// 10. 返回去畸变后的UV，用于采样SceneTexture
// 这样输出的RGB图像是去畸变的正常透视图像
return OriginalUV;
