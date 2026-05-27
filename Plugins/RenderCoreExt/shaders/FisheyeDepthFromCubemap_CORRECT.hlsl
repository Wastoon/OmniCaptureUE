// ============================================
// 正确版：深度Cubemap采样节点
// 从鱼眼像素坐标计算真实的3D射线方向，用于从Cubemap采样深度
// ============================================

// 输入：屏幕UV坐标 (0-1范围，这是鱼眼图像的UV)
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

// ========== 从鱼眼像素坐标计算3D射线方向 ==========

// 1. 将屏幕UV转换为像素坐标（鱼眼图像上的像素位置）
float2 PixelCoord = ScreenUV * float2(ImageWidth, ImageHeight);

// 2. 转换为以主点为中心的归一化坐标（鱼眼图像平面上的坐标）
float2 NormalizedCoord = (PixelCoord - float2(Cx, Cy)) / float2(Fx, Fy);

// 3. 计算鱼眼图像上的径向距离 r_d
float r_d = length(NormalizedCoord);

// 4. 使用Kannala-Brandt模型从r_d反推theta（逆畸变）
// r_d = theta * (1 + k1*theta^2 + k2*theta^4 + k3*theta^6 + k4*theta^8)
// 我们需要从r_d反推theta，使用牛顿迭代法
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

// 5. 计算方位角 phi（从归一化坐标）
float phi = atan2(NormalizedCoord.y, NormalizedCoord.x);

// 6. 将theta和phi转换为3D射线方向（球坐标系转笛卡尔坐标系）
// theta: 与光轴的夹角（0到π）
// phi: 方位角（0到2π）
float sinTheta = sin(theta);
float cosTheta = cos(theta);
float cosPhi = cos(phi);
float sinPhi = sin(phi);

// UE5 Cubemap坐标系: X-Forward, Y-Right, Z-Up
// 从球坐标转换为笛卡尔坐标
float3 RayDirection = float3(
    cosTheta,                    // X (Forward) - 沿光轴方向
    sinTheta * cosPhi,           // Y (Right) - 水平方向
    sinTheta * sinPhi            // Z (Up) - 垂直方向
);

// 7. 返回3D射线方向，用于从Cubemap采样深度
// 注意：这个方向是从相机中心指向场景的射线方向
// 在材质中，使用SampleCubemap节点，将RayDirection连接到Direction输入
return RayDirection;
