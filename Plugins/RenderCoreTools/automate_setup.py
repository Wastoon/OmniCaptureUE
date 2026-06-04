# Copyright (c) 2026 mengrongye.
#
# Author: mengrongye
# Contact: mengrongye@gmail.com
# Project: OmniCaptureUE / RenderCoreExt
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at:
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0

import socket
import json
import struct
import time
import numpy as np
from datetime import datetime

# 协议头定义
MAGIC = 0x55453552  # 'UE5R'
VERSION = 0x0001
PAYLOAD_TYPE_COMMAND = 0

def send_command(sock, cmd_dict):
    """发送命令到UE5服务器"""
    payload = json.dumps(cmd_dict).encode("utf-8")
    payload_length = len(payload)
    header = struct.pack("!IHH I", MAGIC, VERSION, PAYLOAD_TYPE_COMMAND, payload_length)
    sock.sendall(header + payload)

def clear_cameras(sock, actor_name):
    """清除Actor上所有的相机组件"""
    cmd = {
        "cmd": "clear_cameras",
        "actor": actor_name
    }
    send_command(sock, cmd)
    print(f"[INFO] Cleared all cameras on {actor_name}")

def configure_camera(sock, actor_name, component_name, position, rotation_quat, 
                     resolution=[1280, 720], model="pinhole", intrinsics=[], distortion=[],
                     rgb_mat=None, depth_mat=None, use_cubemap=False, cubemap_res=512,
                     exposure_bias=0.0, enable_tone_curve=True, use_hdr=True, warmup_frames=10):
    """配置或创建指定的相机组件"""
    cmd = {
        "cmd": "configure_camera",
        "actor": actor_name,
        "component": component_name,
        "pos": position,
        "rot": rotation_quat,
        "resolution": resolution,
        "model": model,
        "intrinsics": intrinsics,
        "distortion": distortion,
        "use_cubemap": use_cubemap,
        "cubemap_res": cubemap_res,
        "exposure_bias": exposure_bias,
        "enable_tone_curve": enable_tone_curve,
        "use_hdr": use_hdr,
        "warmup_frames": warmup_frames
    }
    if rgb_mat: cmd["rgb_mat"] = rgb_mat
    if depth_mat: cmd["depth_mat"] = depth_mat
    
    send_command(sock, cmd)
    print(f"[INFO] Configured camera: {component_name} (HDR={use_hdr}, WarmUp={warmup_frames})")

def main():
    host = "127.0.0.1"
    port = 8888
    actor_name = "CameraRigActor"
    
    # 材质路径 (请确保这些材质在UE项目中存在)
    RGB_MAT_PATH = "/Game/Materials/FisheyeRGBFromCubemapMaterial.FisheyeRGBFromCubemapMaterial"
    DEPTH_MAT_PATH = "/Game/Materials/FisheyeDepthFromCubemapMaterial.FisheyeDepthFromCubemapMaterial"

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((host, port))
            print(f"[SUCCESS] Connected to UE5 Render Server")

            # 1. 自动化清理
            clear_cameras(s, actor_name)
            time.sleep(0.2)

            # 2. 自动化配置
            # use_hdr=True: 使用 HDR 管线，色彩更真实，质感更好
            # warmup_frames=10: 每次捕获前先跑 10 帧，让 Lumen 和 TAA 收敛，消除噪点
            configure_camera(s, actor_name, "Cam_Fisheye_Front", [0, 0, 0], [0, 0, 0, 1], 
                             rgb_mat=RGB_MAT_PATH, depth_mat=DEPTH_MAT_PATH,
                             use_cubemap=True, cubemap_res=1024,
                             exposure_bias=0.0, enable_tone_curve=True,
                             use_hdr=False, warmup_frames=10,
                             intrinsics=[300, 300, 640, 360],
                             distortion=[0.3, 0.3, 0.02, 0.25])
            
            time.sleep(0.5)

            # 3. 渲染循环
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            base_path = f"E:/UE_project/demo_createUE_exe/dev_uedemo/Saved/sim_result/dataset_{timestamp}"
            
            for i in range(3):
                x = 1420.0 + i * 10.0
                set_pose_cmd = {
                    "cmd": "set_pose",
                    "actor": actor_name,
                    "pos": [x, 1420.0, 300.0],
                    "rot": [0, 0, 0, 1],
                    "export_path": base_path,
                    "frame_number": i
                }
                send_command(s, set_pose_cmd)
                print(f"[RENDER] Frame {i} saved to {base_path}")
                
                # 稍微等待一下，确保服务器处理完成
                time.sleep(0.2)

    except Exception as e:
        print(f"[ERROR] {e}")

if __name__ == "__main__":
    main()
