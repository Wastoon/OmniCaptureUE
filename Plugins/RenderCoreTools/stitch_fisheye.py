import os
os.environ["OPENCV_IO_ENABLE_OPENEXR"]="1"
import json
import numpy as np
import cv2
import sys
from concurrent.futures import ThreadPoolExecutor

# ===================== 配置 (已更新为您的最新参数) =====================
TARGET_RESOLUTION_W = 2720
TARGET_RESOLUTION_H = 3056
FISHEYE_FOV = 180.0
PROJECTION_TYPE = 'equidis62_bilinear'

omni_cx = 1365.5953776041667
omni_cy = 1523.2508138020833
omni_fx = 2003.3527018229167
omni_fy = 2003.2957356770833
K_FISHEYE = [omni_fx, omni_fy, omni_cx, omni_cy]

TAN_WEIGHTS = [1.091309, 1.569686]

# 4 组 8 维参数 (d0, d1, d2, d3)
DCS = np.array([
    [-0.21649757027626038, 0.4868817627429962, -1.9327325820922852, 2.7237048149108887, -1.6815117597579956, 0.38897672295570374, 8.189813524950296e-05, -6.762190605513752e-05],
    [-0.21800976991653442, 0.6687560677528381, -2.7871830463409424, 4.2800092697143555, -2.9389259815216064, 0.7648929357528687, 7.334971087402664e-06, -0.00017349305562675],
    [-0.09532871842384338, -0.18340863287448883, -0.35361433029174805, 0.842681884765625, -0.5731412768363953, 0.13299871981143951, -5.892991248401813e-05, 0.00013068236876279116],
    [-0.16541393101215363, 0.21858032047748566, -1.2697675228118896, 1.8749427795410156, -1.1472108364105225, 0.2589150071144104, -2.7661604690365493e-05, -0.00013212341582402587]
])

# ===================== 工具函数 =====================
def load_metadata(folder_path):
    json_path = os.path.join(folder_path, "views_metadata.json")
    with open(json_path, 'r') as f:
        data = json.load(f)
    return data

def ue_rotator_to_matrix(pitch, yaw, roll=0):
    """根据您的原始逻辑构建旋转矩阵"""
    cp, sp = np.cos(np.deg2rad(pitch)), np.sin(np.deg2rad(pitch))
    cy, sy = np.cos(np.deg2rad(yaw)), np.sin(np.deg2rad(yaw))
    # R_pitch (环绕 Y 轴), R_yaw (环绕 Z 轴)
    R_p = np.array([[cp, 0, -sp], [0, 1, 0], [sp, 0, cp]])
    R_y = np.array([[cy, -sy, 0], [sy, cy, 0], [0, 0, 1]])
    return (R_y @ R_p).T

# ===================== Equidis62 Bilinear 核心逻辑 =====================

def distort_eq62_bilinear(xy_u, tan_weights, dcs):
    """
    完全复刻您的 C++ 逻辑 (Vectorized)
    xy_u: (N, 2)
    """
    ux, uy = xy_u[:, 0], xy_u[:, 1]
    tanx, tany = tan_weights
    
    # 1. 双线性插值权重
    fx_w = np.clip((ux + tanx) / (2.0 * tanx), 0.0, 1.0)
    fy_w = np.clip((uy + tany) / (2.0 * tany), 0.0, 1.0)
    
    w00 = (1.0 - fx_w) * (1.0 - fy_w)
    w01 = fx_w * (1.0 - fy_w)
    w10 = (1.0 - fx_w) * fy_w
    w11 = fx_w * fy_w
    
    # 插值参数 bl_dcs (k1...k6, p1, p2)
    bl_dcs = (w00[:, None] * dcs[0] + w01[:, None] * dcs[1] + 
              w10[:, None] * dcs[2] + w11[:, None] * dcs[3])
    
    # 2. 畸变计算
    r2 = ux*ux + uy*uy
    r = np.sqrt(r2)
    theta = np.arctan(r)
    theta2 = theta * theta
    
    # Horner 展开计算 poly
    poly = bl_dcs[:, 5]
    for i in range(4, -1, -1):
        poly = poly * theta2 + bl_dcs[:, i]
    poly = poly * theta2 + 1.0
    
    p = theta * poly
    scaling = np.where(r > 1e-7, p / r, poly)
    
    # 径向分量
    xr, yr = ux * scaling, uy * scaling
    # 切向分量
    xy = ux * uy
    xt = 2.0 * bl_dcs[:, 6] * xy + bl_dcs[:, 7] * (r2 + 2.0 * ux*ux)
    yt = 2.0 * bl_dcs[:, 7] * xy + bl_dcs[:, 6] * (r2 + 2.0 * uy*uy)
    
    x_distort, y_distort = xr + xt, yr + yt
    
    # 3. 雅可比矩阵计算 (用于牛顿法)
    dt_dr = 1.0 / (r2 + 1.0)
    dr_dx = np.where(r > 1e-7, ux / r, 0.0)
    dr_dy = np.where(r > 1e-7, uy / r, 0.0)
    
    # 多项式导数
    dpoly_dtheta = 13.0 * bl_dcs[:, 5]
    for i in range(4, -1, -1):
        dpoly_dtheta = dpoly_dtheta * theta2 + (2*i + 3) * bl_dcs[:, i]
    # 注意：这里 C++ 逻辑中最终会乘以 theta2 并加 1
    dpoly_dtheta = dpoly_dtheta * theta2 + 1.0
    
    dp_dr = dpoly_dtheta * dt_dr
    
    inv_r = np.where(r > 1e-7, 1.0 / r, 0.0)
    dp_over_r_dx = dr_dx * (dp_dr * r - p) / (r2 + 1e-12)
    dp_over_r_dy = dr_dy * (dp_dr * r - p) / (r2 + 1e-12)
    
    # 径向 Jacobian
    dRx_dx = np.where(r > 1e-7, p * inv_r + ux * dp_over_r_dx, poly)
    dRx_dy = ux * dp_over_r_dy
    dRy_dx = uy * dp_over_r_dx
    dRy_dy = np.where(r > 1e-7, p * inv_r + uy * dp_over_r_dy, poly)
    
    # 切向 Jacobian
    dTx_dx = 2.0 * bl_dcs[:, 6] * uy + bl_dcs[:, 7] * 6.0 * ux
    dTx_dy = 2.0 * bl_dcs[:, 6] * ux + bl_dcs[:, 7] * 2.0 * uy
    dTy_dx = 2.0 * bl_dcs[:, 7] * uy + bl_dcs[:, 6] * 2.0 * ux
    dTy_dy = 2.0 * bl_dcs[:, 7] * ux + bl_dcs[:, 6] * 6.0 * uy
    
    J11, J12, J21, J22 = dRx_dx + dTx_dx, dRx_dy + dTx_dy, dRy_dx + dTy_dx, dRy_dy + dTy_dy
    
    # 4. 双线性插值贡献 (Bilinear Chain Rule)
    # 这部分计算非常密集，但对边缘精度至关重要
    theta_x_r = np.where(r > 1e-7, theta * ux / r, ux)
    theta_y_r = np.where(r > 1e-7, theta * uy / r, uy)
    
    t_pows = [theta2**(i+1) for i in range(6)]
    J_params_x = np.hstack([theta_x_r[:, None] * np.array(t_pows).T, (2.0*xy)[:, None], (r2+2*ux*ux)[:, None]])
    J_params_y = np.hstack([theta_y_r[:, None] * np.array(t_pows).T, (r2+2*uy*uy)[:, None], (2.0*xy)[:, None]])
    
    dfx_dux = 0.5 / tanx
    dfy_duy = 0.5 / tany
    
    diff_x = (1.0 - fy_w)[:, None] * (dcs[1] - dcs[0]) + fy_w[:, None] * (dcs[3] - dcs[2])
    diff_y = (1.0 - fx_w)[:, None] * (dcs[2] - dcs[0]) + fx_w[:, None] * (dcs[3] - dcs[1])
    
    J11 += np.sum(J_params_x * diff_x, axis=1) * dfx_dux
    J12 += np.sum(J_params_x * diff_y, axis=1) * dfy_duy
    J21 += np.sum(J_params_y * diff_x, axis=1) * dfx_dux
    J22 += np.sum(J_params_y * diff_y, axis=1) * dfy_duy

    return np.stack([x_distort, y_distort], axis=-1), np.stack([J11, J12, J21, J22], axis=-1)

def get_fisheye_rays_eq62_bl(res_w, res_h, K, tan_weights, dcs):
    """
    生成鱼眼光线 (Vectorized Inverse Projection)
    """
    fx, fy, cx, cy = K
    u_idx = np.arange(res_w)
    v_idx = np.arange(res_h)
    u_grid, v_grid = np.meshgrid(u_idx, v_idx)
    
    # 像素坐标到相机平面
    uu = (u_grid.flatten() + 0.5) / res_w
    vv = 1.0 - (v_grid.flatten() + 0.5) / res_h # 翻转 V
    
    xd = (uu * res_w - cx - 0.5) / fx
    yd = (vv * res_h - cy - 0.5) / fy
    target = np.stack([xd, yd], axis=-1)
    
    xy_u = target.copy()
    
    # 牛顿迭代求解未畸变坐标
    for i in range(15):
        val, jac = distort_eq62_bilinear(xy_u, tan_weights, dcs)
        err = target - val
        err2 = np.sum(err**2, axis=1)
        
        if np.max(err2) < 1e-10: break
        
        det = jac[:, 0] * jac[:, 3] - jac[:, 1] * jac[:, 2]
        inv_det = 1.0 / np.where(np.abs(det) < 1e-8, 1e-8, det)
        
        dux = inv_det * (jac[:, 3] * err[:, 0] - jac[:, 1] * err[:, 1])
        duy = inv_det * (-jac[:, 2] * err[:, 0] + jac[:, 0] * err[:, 1])
        
        xy_u += np.stack([dux, duy], axis=-1)
        
    mag = np.sqrt(xy_u[:,0]**2 + xy_u[:,1]**2 + 1.0)
    # 变换到 Unreal: (dir.z, -dir.x, -dir.y)
    rays = np.stack([1.0/mag, -xy_u[:,0]/mag, -xy_u[:,1]/mag], axis=-1)
    
    # 简单的 FOV 遮罩
    theta = np.arctan(np.sqrt(xy_u[:,0]**2 + xy_u[:,1]**2))
    mask = theta < (np.deg2rad(FISHEYE_FOV) / 2.0)
    
    return rays.reshape(res_h, res_w, 3), mask.reshape(res_h, res_w)

# ===================== 主拼接逻辑 =====================

def stitch_frame(folder):
    print(f"Stitching {folder} with Equidistant 6+2 Bilinear...")
    metadata = load_metadata(folder)
    views = metadata['views'] if isinstance(metadata, dict) else metadata
    
    # 1. 预计算所有子视图的变换矩阵
    view_mats = np.array([ue_rotator_to_matrix(v['pitch'], v['yaw']) for v in views])
    view_forwards = view_mats[:, 0, :]
    
    # 2. 生成目标鱼眼光线
    rays, mask = get_fisheye_rays_eq62_bl(TARGET_RESOLUTION_W, TARGET_RESOLUTION_H, K_FISHEYE, TAN_WEIGHTS, DCS)
    flat_rays = rays[mask]
    
    # 3. 匹配最合适的 Tile
    best_v = np.argmax(flat_rays @ view_forwards.T, axis=1)
    
    # 4. 执行拼接 (带双线性采样)
    final_rgb = np.zeros((len(flat_rays), 3), dtype=np.uint8)
    
    unique_views = np.unique(best_v)
    for v_idx in unique_views:
        midx = np.where(best_v == v_idx)[0]
        img_path = os.path.join(folder, views[v_idx]['filename'])
        img = cv2.imread(img_path)
        if img is None: continue
        img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        
        # 将鱼眼光线旋转回该子视图坐标系
        r_loc = flat_rays[midx] @ view_mats[v_idx].T
        x, y, z = r_loc[:,0], r_loc[:,1], r_loc[:,2]
        x = np.maximum(x, 0.001)
        
        # 投影到 Pinhole (90deg FOV)
        # u = y/x, v = z/x
        u_p = (y/x + 1.0) * 0.5 * (img.shape[1] - 1)
        v_p = (1.0 - z/x) * 0.5 * (img.shape[0] - 1)
        
        # 采样 (这里使用 OpenCV 的内置映射会更稳健)
        u_p = np.clip(u_p, 0, img.shape[1]-1).astype(np.float32)
        v_p = np.clip(v_p, 0, img.shape[0]-1).astype(np.float32)
        
        # 简单的双线性采样
        u0 = np.floor(u_p).astype(int)
        v0 = np.floor(v_p).astype(int)
        u1 = np.minimum(u0 + 1, img.shape[1]-1)
        v1 = np.minimum(v0 + 1, img.shape[0]-1)
        du = (u_p - u0)[:, None]
        dv = (v_p - v0)[:, None]
        
        colors = (img[v0, u0]*(1-du)*(1-dv) + img[v0, u1]*du*(1-dv) +
                  img[v1, u0]*(1-du)*dv + img[v1, u1]*du*dv)
        final_rgb[midx] = colors.astype(np.uint8)

    # 5. 生成结果图
    res_rgb = np.zeros((TARGET_RESOLUTION_H, TARGET_RESOLUTION_W, 3), dtype=np.uint8)
    res_rgb[mask] = final_rgb
    out_path = os.path.join(folder, "Stitched_Fisheye.png")
    cv2.imwrite(out_path, cv2.cvtColor(res_rgb, cv2.COLOR_RGB2BGR))
    print(f"Success: {out_path}")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        stitch_frame(sys.argv[1])
    else:
        print("Please provide frame folder.")
