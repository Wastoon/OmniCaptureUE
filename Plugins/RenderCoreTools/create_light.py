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

# 协议头定义
MAGIC = 0x55453552  
VERSION = 0x0001
PAYLOAD_TYPE_COMMAND = 0

def send_command(sock, cmd_dict):
    payload = json.dumps(cmd_dict).encode("utf-8")
    payload_length = len(payload)
    header = struct.pack("<IHH I", MAGIC, VERSION, PAYLOAD_TYPE_COMMAND, payload_length)
    
    # 打印出来检查：正确的小端序 header 前四个字节应该是 b'RE5U' (即 52 45 35 55)
    print(f"Header hex: {header.hex()}") 
    
    sock.sendall(header + payload)

def main(cam_trajectory_file_name=None):
    host = "127.0.0.1"
    port = 9998
    actor_name = "CameraRigActor"
    LIGHT_JSON_PATH= r"dome_light.json"

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((host, port))
            print(f"[SUCCESS] Connected to UE5 Render Server")

            # --- 1. 发送灯光配置 (在同一次连接中发送) ---
            with open(LIGHT_JSON_PATH, 'r') as f:
                light_data = json.load(f)
            
            # 直接发送数组或包装好的命令
            light_cmd = {
                "cmd": "create_lights",
                "lights": light_data if isinstance(light_data, list) else light_data.get("lights", [])
            }
            print(light_cmd)
            send_command(s, light_cmd)
            print("Lights configuration sent.")
            time.sleep(3.0) # 等待灯光创建完成

    except Exception as e:
        print(f"[ERROR] {e}")

if __name__ == "__main__":
    main()