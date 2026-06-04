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

import argparse
import json
import socket
import struct
import time
from datetime import datetime


MAGIC = 0x55453552  # UE5R
VERSION = 0x0001
PAYLOAD_TYPE_COMMAND = 0

DEFAULT_CAPTURE_ACTOR = "MyMetaHuman"
DEFAULT_CAPTURE_COMPONENT = "DomeCam"

DEPTH_MAT_PATH = "/RenderCoreExt/Materials/M_pinhole_depth_PP.M_pinhole_depth_PP"
POSITION_MAT_PATH = "/RenderCoreExt/Materials/M_WorldPositionExport.M_WorldPositionExport"
NORMAL_MAT_PATH = "/RenderCoreExt/Materials/M_Normalview.M_Normalview"
SEMANTIC_MAT_PATH = None


def load_arkit_expression_frames(path):
    if not path:
        return None

    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    if data is None:
        return None

    if isinstance(data, dict):
        for key in ("arkit_expression_frames", "arkit_frames", "frames", "blendshape_frames"):
            if key in data:
                data = data[key]
                break

    if not isinstance(data, list):
        raise ValueError(
            "ARKit JSON must be a list of frames or an object containing "
            "'arkit_expression_frames', 'arkit_frames', 'frames', or 'blendshape_frames'."
        )

    normalized = []
    for index, item in enumerate(data):
        if not isinstance(item, dict):
            raise ValueError(f"ARKit frame {index} must be an object.")

        coefficients = None
        for key in ("coefficients", "blendshapes", "curves", "arkit"):
            if isinstance(item.get(key), dict):
                coefficients = item[key]
                break
        if coefficients is None:
            coefficients = {
                key: value
                for key, value in item.items()
                if key not in {"frame", "time", "timestamp"}
            }

        normalized.append(
            {
                "frame": int(item.get("frame", index)),
                "coefficients": {
                    str(key): float(value)
                    for key, value in coefficients.items()
                    if value is not None
                },
            }
        )

    return normalized


def send_command(sock, command):
    payload = json.dumps(command).encode("utf-8")
    header = struct.pack("!IHHI", MAGIC, VERSION, PAYLOAD_TYPE_COMMAND, len(payload))
    sock.sendall(header + payload)


def build_manual_dome_cameras():
    return [
        {"name": "cam_front", "pos": [-200, 0, 150], "rot": [0, 0, 0], "fov": 45},
        {"name": "cam_front_left", "pos": [-141, -141, 150], "rot": [0, 45, 0], "fov": 45},
        {"name": "cam_left", "pos": [0, -200, 150], "rot": [0, 90, 0], "fov": 45},
        {"name": "cam_back_left", "pos": [141, -141, 150], "rot": [0, 135, 0], "fov": 45},
        {"name": "cam_back", "pos": [200, 0, 150], "rot": [0, 180, 0], "fov": 45},
        {"name": "cam_back_right", "pos": [141, 141, 150], "rot": [0, 225, 0], "fov": 45},
        {"name": "cam_right", "pos": [0, 200, 150], "rot": [0, 270, 0], "fov": 45},
        {"name": "cam_front_right", "pos": [-141, 141, 150], "rot": [0, 315, 0], "fov": 45},
        {"name": "cam_upper_front", "pos": [-170, 0, 220], "rot": [-20, 0, 0], "fov": 50},
        {"name": "cam_upper_front_left", "pos": [-120, -120, 220], "rot": [-20, 45, 0], "fov": 50},
        {"name": "cam_upper_left", "pos": [0, -170, 220], "rot": [-20, 90, 0], "fov": 50},
        {"name": "cam_upper_back_left", "pos": [120, -120, 220], "rot": [-20, 135, 0], "fov": 50},
        {"name": "cam_upper_back", "pos": [170, 0, 220], "rot": [-20, 180, 0], "fov": 50},
        {"name": "cam_upper_back_right", "pos": [120, 120, 220], "rot": [-20, 225, 0], "fov": 50},
        {"name": "cam_upper_right", "pos": [0, 170, 220], "rot": [-20, 270, 0], "fov": 50},
        {"name": "cam_upper_front_right", "pos": [-120, 120, 220], "rot": [-20, 315, 0], "fov": 50},
        {"name": "cam_top", "pos": [0, 0, 350], "rot": [-90, 0, 0], "fov": 65},
    ]


def clear_cameras(sock, actor_name):
    send_command(sock, {"cmd": "clear_cameras", "actor": actor_name})
    print(f"[INFO] Cleared cameras on {actor_name}")


def configure_dome_camera(sock, args):
    command = {
        "cmd": "configure_camera",
        "actor": args.capture_actor,
        "model": "dome",
        "component": args.capture_component,
        "resolution": [args.resolution, args.resolution],
        "dome_radius": args.dome_radius,
        "dome_num_cameras": 32,
        "dome_layout": "manual",
        "dome_manual_cameras": build_manual_dome_cameras(),
        "dome_fov": 90,
        "dome_batch_size": args.dome_batch_size,
        "dome_enabled_passes": args.dome_enabled_passes,
        "depth_mat": DEPTH_MAT_PATH,
        "normal_mat": NORMAL_MAT_PATH,
        "pos_mat": POSITION_MAT_PATH,
        "semantic_mat": SEMANTIC_MAT_PATH,
    }
    send_command(sock, command)
    print(f"[INFO] Configured {args.capture_component} on {args.capture_actor}")


def main():
    parser = argparse.ArgumentParser(
        description="Drive a UE LevelSequence frame-by-frame through RenderCoreExt."
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9998)
    parser.add_argument("--sequence", default="driveface")
    parser.add_argument("--metahuman-actor", default=None)
    parser.add_argument("--capture-actor", default=DEFAULT_CAPTURE_ACTOR)
    parser.add_argument("--capture-component", default=DEFAULT_CAPTURE_COMPONENT)
    parser.add_argument(
        "--arkit-json",
        default=None,
        help=(
            "Optional ARKit coefficient sequence JSON. When omitted or JSON is null, "
            "UE uses the existing Face animation in the Level Sequence."
        ),
    )
    parser.add_argument("--export-path", default="")
    parser.add_argument("--start-frame", type=int, default=0)
    parser.add_argument("--end-frame", type=int, default=-1)
    parser.add_argument("--frame-count", type=int, default=0)
    parser.add_argument("--frame-step", type=int, default=1)
    parser.add_argument("--wait-frames", type=int, default=2)
    parser.add_argument("--seek-only", action="store_true")
    parser.add_argument("--single-frame", type=int, default=None)
    parser.add_argument("--skip-configure", action="store_true")
    parser.add_argument("--keep-cameras", action="store_true")
    parser.add_argument("--resolution", type=int, default=2048)
    parser.add_argument("--dome-radius", type=float, default=300)
    parser.add_argument("--dome-batch-size", type=int, default=4)
    parser.add_argument("--dome-enabled-passes", type=lambda value: int(value, 0), default=0x1F)
    parser.add_argument("--configure-wait-sec", type=float, default=1.0)
    parser.add_argument("--hold-seconds", type=float, default=0.0)
    args = parser.parse_args()
    arkit_expression_frames = load_arkit_expression_frames(args.arkit_json)
    if args.arkit_json:
        frame_count = len(arkit_expression_frames or [])
        print(f"[INFO] Loaded ARKit JSON: {args.arkit_json} ({frame_count} frames)")

    export_path = args.export_path
    if not export_path and not args.seek_only:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        export_path = (
            "E:/UE_project/demo_createUE_exe/dev_uedemo/Saved/sim_result/"
            f"driveface_sequence_{stamp}"
        )

    if args.single_frame is not None:
        command = {
            "cmd": "set_sequence_frame",
            "actor": args.metahuman_actor or args.capture_actor,
            "sequence_actor": args.sequence,
            "frame_number": args.single_frame,
            "capture_actor": args.capture_actor,
            "capture_component": args.capture_component,
            "export_path": export_path,
            "sequence_wait_frames": args.wait_frames,
            "render_after_seek": not args.seek_only,
            "arkit_expression_frames": arkit_expression_frames,
        }
    else:
        command = {
            "cmd": "capture_level_sequence",
            "actor": args.metahuman_actor or args.capture_actor,
            "sequence_actor": args.sequence,
            "capture_actor": args.capture_actor,
            "capture_component": args.capture_component,
            "export_path": export_path,
            "start_frame": args.start_frame,
            "end_frame": args.end_frame,
            "frame_count": args.frame_count,
            "frame_step": args.frame_step,
            "sequence_wait_frames": args.wait_frames,
            "render_after_seek": not args.seek_only,
            "arkit_expression_frames": arkit_expression_frames,
        }

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.connect((args.host, args.port))
        if not args.seek_only and not args.skip_configure:
            if not args.keep_cameras:
                clear_cameras(sock, args.capture_actor)
                time.sleep(0.2)
            configure_dome_camera(sock, args)
            time.sleep(args.configure_wait_sec)
        send_command(sock, command)
        if args.hold_seconds > 0:
            print(f"[INFO] Holding connection for {args.hold_seconds:.1f}s...")
            time.sleep(args.hold_seconds)

    print("[OK] Command sent:")
    print(
        "[OK] ARKit override: "
        f"{len(arkit_expression_frames or [])} frames"
        if arkit_expression_frames
        else "[OK] ARKit override: disabled; UE Level Sequence Face animation will be used"
    )
    print(json.dumps(command, indent=2))


if __name__ == "__main__":
    main()
