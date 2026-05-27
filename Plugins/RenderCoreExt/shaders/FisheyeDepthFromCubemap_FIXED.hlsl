// ============================================
// 修正版：深度Cubemap采样节点
// 与RGB材质使用完全相同的UV处理逻辑，确保像素对应
// ============================================

// 输入：屏幕UV坐标 (0-1范围)
// 注意：Surface材质在DrawMaterialToRenderTarget时，UV坐标系可能与Post Process不同
// 需要确保与RGB材质使用相同的UV处理

float2 ScreenUV = ScreenUV;

// 输入：相机内参（必须与RGB材质完全相同）
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

// ========== 关键修正：与RGB材质使用完全相同的UV处理 ==========

// 1. 将屏幕UV转换为像素坐标
// 注意：Surface材质在DrawMaterialToRenderTarget时，UV可能已经翻转
// 但为了与RGB材质一致，我们使用相同的处理（不翻转）
// 如果发现上下颠倒，可以在材质编辑器中调整，而不是在这里翻转
float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);

// 2. 转换为以主点为中心的归一化坐标（与RGB材质完全一致）
float2 NormalizedCoord = (PixelCoord - float2(Cx, Cy)) / float2(Fx, Fy);

// 3. 计算鱼眼图像上的径向距离 r_d（与RGB材质完全一致）
float r_d = length(NormalizedCoord);

// 4. 使用Kannala-Brandt模型从r_d反推theta（逆畸变，与RGB材质完全一致）
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

// 5. 计算方位角 phi（与RGB材质使用相同的归一化坐标）
float phi = atan2(NormalizedCoord.y, NormalizedCoord.x);

// 6. 将theta和phi转换为3D射线方向（球坐标系转笛卡尔坐标系）
float sinTheta = sin(theta);
float cosTheta = cos(theta);
float cosPhi = cos(phi);
float sinPhi = sin(phi);

// UE5 Cubemap坐标系: X-Forward, Y-Right, Z-Up
// 从球坐标转换为笛卡尔坐标（与RGB材质使用相同的theta和phi）
float3 RayDirection = float3(
    cosTheta,                    // X (Forward)
    sinTheta * cosPhi,           // Y (Right)
    sinTheta * sinPhi            // Z (Up)
);

// 7. 返回3D射线方向，用于从Cubemap采样深度
return RayDirection;
