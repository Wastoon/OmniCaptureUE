import json
import socket
import struct
import time
from datetime import datetime


MAGIC = 0x55453552  # 'UE5R'
VERSION = 0x0001
PAYLOAD_TYPE_COMMAND = 0


HOST = "127.0.0.1"
PORT = 9998

METAHUMAN_ACTOR = "MyMetaHuman"
CAPTURE_ACTOR = "MyMetaHuman"
CAPTURE_COMPONENT = "DomeCam"

DEPTH_MAT_PATH = "/RenderCoreExt/Materials/M_pinhole_depth_PP.M_pinhole_depth_PP"
POSITION_MAT_PATH = "/RenderCoreExt/Materials/M_WorldPositionExport.M_WorldPositionExport"
NORMAL_MAT_PATH = "/RenderCoreExt/Materials/M_Normalview.M_Normalview"
SEMANTIC_MAT_PATH = None

# 每帧拍摄前等待的秒数（额外安全边距，适配慢机器）。
# C++ 侧已经内置等 2 个 GPU 帧，这里是额外的 Python 侧等待。
# 通常 0 即可；如果仍有撕裂可适当调大到 0.1。
EXTRA_CAPTURE_WAIT_SEC = 0.0

# 整个序列结束后，客户端需要保持连接多久（秒）。
# 17 相机 × 5 通道 × 2048px 会比较慢，按实际情况调整。
SEQUENCE_WAIT_SEC = 300


def send_command(sock, cmd_dict):
    payload = json.dumps(cmd_dict).encode("utf-8")
    header = struct.pack("!IHH I", MAGIC, VERSION, PAYLOAD_TYPE_COMMAND, len(payload))
    sock.sendall(header + payload)


def clear_cameras(sock, actor_name):
    send_command(sock, {"cmd": "clear_cameras", "actor": actor_name})
    print(f"[INFO] Cleared all cameras on {actor_name}")


def build_manual_dome_cameras():
    """
    17 个手动相机，覆盖上半球 + 顶点。
    pos  单位 cm，相对 Actor 原点；
    rot  [pitch, yaw, roll]，单位度，朝向 Actor 中心即可（C++ 侧忽略 roll）。
    fov  单位度。
    """
    return [
        # ── 第一环：地平面附近（Z=150） ──────────────────────────────────
        {"name": "cam_front",       "pos": [-200,    0, 150], "rot": [  0,   0, 0], "fov": 45},
        {"name": "cam_front_left",  "pos": [-141, -141, 150], "rot": [  0,  45, 0], "fov": 45},
        {"name": "cam_left",        "pos": [   0, -200, 150], "rot": [  0,  90, 0], "fov": 45},
        {"name": "cam_back_left",   "pos": [ 141, -141, 150], "rot": [  0, 135, 0], "fov": 45},
        {"name": "cam_back",        "pos": [ 200,    0, 150], "rot": [  0, 180, 0], "fov": 45},
        {"name": "cam_back_right",  "pos": [ 141,  141, 150], "rot": [  0, 225, 0], "fov": 45},
        {"name": "cam_right",       "pos": [   0,  200, 150], "rot": [  0, 270, 0], "fov": 45},
        {"name": "cam_front_right", "pos": [-141,  141, 150], "rot": [  0, 315, 0], "fov": 45},
        # ── 第二环：仰角 20°（Z=220） ─────────────────────────────────────
        {"name": "cam_upper_front",       "pos": [-170,    0, 220], "rot": [-20,   0, 0], "fov": 50},
        {"name": "cam_upper_front_left",  "pos": [-120, -120, 220], "rot": [-20,  45, 0], "fov": 50},
        {"name": "cam_upper_left",        "pos": [   0, -170, 220], "rot": [-20,  90, 0], "fov": 50},
        {"name": "cam_upper_back_left",   "pos": [ 120, -120, 220], "rot": [-20, 135, 0], "fov": 50},
        {"name": "cam_upper_back",        "pos": [ 170,    0, 220], "rot": [-20, 180, 0], "fov": 50},
        {"name": "cam_upper_back_right",  "pos": [ 120,  120, 220], "rot": [-20, 225, 0], "fov": 50},
        {"name": "cam_upper_right",       "pos": [   0,  170, 220], "rot": [-20, 270, 0], "fov": 50},
        {"name": "cam_upper_front_right", "pos": [-120,  120, 220], "rot": [-20, 315, 0], "fov": 50},
        # ── 顶点 ──────────────────────────────────────────────────────────
        {"name": "cam_top", "pos": [0, 0, 350], "rot": [-90, 0, 0], "fov": 65},
    ]


def configure_dome_camera(sock):
    cmd = {
        "cmd": "configure_camera",
        "actor": CAPTURE_ACTOR,
        "model": "dome",
        "component": CAPTURE_COMPONENT,
        "resolution": [2048, 2048],
        "dome_radius": 300,
        "dome_num_cameras": 32,
        "dome_layout": "manual",
        "dome_manual_cameras": build_manual_dome_cameras(),
        "dome_fov": 90,
        "dome_batch_size": 4,
        "dome_enabled_passes": 0x1F,   # RGB | Depth | Normal | Position | Semantic
        "depth_mat": DEPTH_MAT_PATH,
        "normal_mat": NORMAL_MAT_PATH,
        "pos_mat": POSITION_MAT_PATH,
        "semantic_mat": SEMANTIC_MAT_PATH,
    }
    send_command(sock, cmd)
    print(f"[INFO] Configured {CAPTURE_COMPONENT} on {CAPTURE_ACTOR}")


def set_actor_pose(sock, actor_name, pos=None, quat=None):
    cmd = {
        "cmd": "set_pose",
        "actor": actor_name,
        "pos": pos or [0.0, 0.0, 0.0],
        "rot": quat or [0.0, 0.0, 0.0, 1.0],
    }
    send_command(sock, cmd)
    print(f"[INFO] Set pose for {actor_name}")


def build_expression_frames():
    """
    关键帧列表（CTRL_* 曲线）。

    资产对应关系：
      - Face「动画类」Face_AnimBP_C = 主 AnimBP（双击打开 AnimGraph + 左侧预览视口）
      - Post Process ABP_*_FaceMesh_PostProcess = RigLogic（日志 PP= 那一项）
      - mh_arkit_mapping_anim = AnimSequence，在序列编辑器里看曲线时间轴，不是 AnimBP

    只写 CTRL_*；CTRL_rigLogic_OffOn=1.0 必须带。head_lod0_mesh__* 对标准 MetaHuman 无效。
    """
    rig_on = {"CTRL_rigLogic_OffOn": 1.0}

    return [
        # ── 帧 0：中性表情（基准） ────────────────────────────────────────
        {
            "frame": 0,
            "curves": {
                **rig_on,
                "CTRL_expressions_jawOpen": 0.0,
                "head_lod0_mesh__jaw_open": 0.0,
                "CTRL_expressions_mouthCornerPullL": 0.0,
                "CTRL_expressions_mouthCornerPullR": 0.0,
                "head_lod0_mesh__mouth_cornerPull_left": 0.0,
                "head_lod0_mesh__mouth_cornerPull_right": 0.0,
                "CTRL_expressions_mouthCornerUpL": 0.0,
                "CTRL_expressions_mouthCornerUpR": 0.0,
                "head_lod0_mesh__mouth_cornersUp_L": 0.0,
                "head_lod0_mesh__mouth_cornersUp_R": 0.0,
                "CTRL_expressions_browRaiseInL": 0.0,
                "CTRL_expressions_browRaiseInR": 0.0,
                "head_lod0_mesh__brow_raiseIn_L": 0.0,
                "head_lod0_mesh__brow_raiseIn_R": 0.0,
                "CTRL_expressions_eyeBlinkL": 0.0,
                "CTRL_expressions_eyeBlinkR": 0.0,
                "head_lod0_mesh__eye_blink_L": 0.0,
                "head_lod0_mesh__eye_blink_R": 0.0,
            },
        },
        # ── 帧 10：张嘴 + 眉毛上扬（惊讶） ──────────────────────────────
        {
            "frame": 10,
            "curves": {
                **rig_on,
                "CTRL_expressions_jawOpen": 0.85,
                "head_lod0_mesh__jaw_open": 0.85,
                "CTRL_expressions_mouthCornerPullL": 0.15,
                "CTRL_expressions_mouthCornerPullR": 0.15,
                "head_lod0_mesh__mouth_cornerPull_left": 0.15,
                "head_lod0_mesh__mouth_cornerPull_right": 0.15,
                "CTRL_expressions_mouthCornerUpL": 0.0,
                "CTRL_expressions_mouthCornerUpR": 0.0,
                "head_lod0_mesh__mouth_cornersUp_L": 0.0,
                "head_lod0_mesh__mouth_cornersUp_R": 0.0,
                "CTRL_expressions_browRaiseInL": 0.55,
                "CTRL_expressions_browRaiseInR": 0.55,
                "head_lod0_mesh__brow_raiseIn_L": 0.55,
                "head_lod0_mesh__brow_raiseIn_R": 0.55,
                "CTRL_expressions_eyeBlinkL": 0.0,
                "CTRL_expressions_eyeBlinkR": 0.0,
                "head_lod0_mesh__eye_blink_L": 0.0,
                "head_lod0_mesh__eye_blink_R": 0.0,
            },
        },
        # ── 帧 20：大笑（嘴角上扬 + 轻微张嘴） ───────────────────────────
        {
            "frame": 20,
            "curves": {
                **rig_on,
                "CTRL_expressions_jawOpen": 0.15,
                "head_lod0_mesh__jaw_open": 0.15,
                "CTRL_expressions_mouthCornerPullL": 0.85,
                "CTRL_expressions_mouthCornerPullR": 0.85,
                "head_lod0_mesh__mouth_cornerPull_left": 0.85,
                "head_lod0_mesh__mouth_cornerPull_right": 0.85,
                "CTRL_expressions_mouthCornerUpL": 0.35,
                "CTRL_expressions_mouthCornerUpR": 0.35,
                "head_lod0_mesh__mouth_cornersUp_L": 0.35,
                "head_lod0_mesh__mouth_cornersUp_R": 0.35,
                "CTRL_expressions_browRaiseInL": 0.25,
                "CTRL_expressions_browRaiseInR": 0.25,
                "head_lod0_mesh__brow_raiseIn_L": 0.25,
                "head_lod0_mesh__brow_raiseIn_R": 0.25,
                "CTRL_expressions_eyeBlinkL": 0.0,
                "CTRL_expressions_eyeBlinkR": 0.0,
                "head_lod0_mesh__eye_blink_L": 0.0,
                "head_lod0_mesh__eye_blink_R": 0.0,
            },
        },
        # ── 帧 30：眨眼 ───────────────────────────────────────────────────
        {
            "frame": 30,
            "curves": {
                **rig_on,
                "CTRL_expressions_jawOpen": 0.0,
                "head_lod0_mesh__jaw_open": 0.0,
                "CTRL_expressions_mouthCornerPullL": 0.0,
                "CTRL_expressions_mouthCornerPullR": 0.0,
                "head_lod0_mesh__mouth_cornerPull_left": 0.0,
                "head_lod0_mesh__mouth_cornerPull_right": 0.0,
                "CTRL_expressions_mouthCornerUpL": 0.0,
                "CTRL_expressions_mouthCornerUpR": 0.0,
                "head_lod0_mesh__mouth_cornersUp_L": 0.0,
                "head_lod0_mesh__mouth_cornersUp_R": 0.0,
                "CTRL_expressions_browRaiseInL": 0.0,
                "CTRL_expressions_browRaiseInR": 0.0,
                "head_lod0_mesh__brow_raiseIn_L": 0.0,
                "head_lod0_mesh__brow_raiseIn_R": 0.0,
                "CTRL_expressions_eyeBlinkL": 0.65,
                "CTRL_expressions_eyeBlinkR": 0.65,
                "head_lod0_mesh__eye_blink_L": 0.65,
                "head_lod0_mesh__eye_blink_R": 0.65,
            },
        },
        # ── 帧 39：回到中性（序列尾帧） ──────────────────────────────────
        {
            "frame": 39,
            "curves": {
                **rig_on,
                "CTRL_expressions_jawOpen": 0.0,
                "head_lod0_mesh__jaw_open": 0.0,
                "CTRL_expressions_mouthCornerPullL": 0.0,
                "CTRL_expressions_mouthCornerPullR": 0.0,
                "head_lod0_mesh__mouth_cornerPull_left": 0.0,
                "head_lod0_mesh__mouth_cornerPull_right": 0.0,
                "CTRL_expressions_mouthCornerUpL": 0.0,
                "CTRL_expressions_mouthCornerUpR": 0.0,
                "head_lod0_mesh__mouth_cornersUp_L": 0.0,
                "head_lod0_mesh__mouth_cornersUp_R": 0.0,
                "CTRL_expressions_browRaiseInL": 0.0,
                "CTRL_expressions_browRaiseInR": 0.0,
                "head_lod0_mesh__brow_raiseIn_L": 0.0,
                "head_lod0_mesh__brow_raiseIn_R": 0.0,
                "CTRL_expressions_eyeBlinkL": 0.0,
                "CTRL_expressions_eyeBlinkR": 0.0,
                "head_lod0_mesh__eye_blink_L": 0.0,
                "head_lod0_mesh__eye_blink_R": 0.0,
            },
        },
    ]


def preview_expression(sock):
    """向 UE 发送一帧表情预览（RigLogic 路径，不暂停 AnimBP）。"""
    cmd = {
        "cmd": "set_metahuman_expression",
        "actor": METAHUMAN_ACTOR,
        "curves": {
            "CTRL_rigLogic_OffOn": 1.0,
            # 张嘴 + 嘴角拉伸 + 眉毛轻扬
            "CTRL_expressions_jawOpen": 0.85,
            "head_lod0_mesh__jaw_open": 0.85,
            "CTRL_expressions_mouthCornerPullL": 0.6,
            "CTRL_expressions_mouthCornerPullR": 0.6,
            "head_lod0_mesh__mouth_cornerPull_left": 0.6,
            "head_lod0_mesh__mouth_cornerPull_right": 0.6,
            "CTRL_expressions_browRaiseInL": 0.3,
            "CTRL_expressions_browRaiseInR": 0.3,
            "head_lod0_mesh__brow_raiseIn_L": 0.3,
            "head_lod0_mesh__brow_raiseIn_R": 0.3,
        },
    }
    send_command(sock, cmd)
    print("[INFO] Preview expression sent (RigLogic curves injected)")


def start_metahuman_sequence(sock, export_path, frame_count=40):
    """
    启动逐帧拍摄序列。C++ 侧每帧：
      1. 插值 CTRL_* 曲线并注入 Face AnimInstance
      2. 保持 AnimBP 运行，等 RigLogic 稳定 2 个 GPU 帧
      3. DomeLightStageCamera.SaveAllData()
    """
    cmd = {
        "cmd": "capture_metahuman_sequence",
        "actor": METAHUMAN_ACTOR,
        "capture_actor": CAPTURE_ACTOR,
        "capture_component": CAPTURE_COMPONENT,
        "export_path": export_path,
        "frame_count": frame_count,
        "expression_frames": build_expression_frames(),
    }
    send_command(sock, cmd)
    print(f"[INFO] Started MetaHuman expression sequence: {frame_count} frames")
    print(f"[INFO] Export path: {export_path}")


def main():
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    base_path = (
        "E:/UE_project/demo_createUE_exe/dev_uedemo/Saved/sim_result/"
        f"metahuman_expr_{timestamp}"
    )

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.connect((HOST, PORT))
        print("[SUCCESS] Connected to UE Render Server")

        # 1. 清空旧相机
        clear_cameras(sock, CAPTURE_ACTOR)
        time.sleep(0.2)

        # 2. 配置穹顶相机（17 个手动相机，5 通道，2048px）
        configure_dome_camera(sock)
        time.sleep(1.0)   # 给 UE 时间创建并注册组件

        # 3. 设置 MetaHuman 位置
        set_actor_pose(sock, METAHUMAN_ACTOR, pos=[0.0, 0.0, 0.0])
        time.sleep(0.2)

        # 4. 预览一帧表情（可选，方便在编辑器里确认曲线有效）
        preview_expression(sock)
        time.sleep(0.5)   # 等视口刷新后再确认

        # 5. 启动序列拍摄（每帧经 RigLogic 驱动表情）
        start_metahuman_sequence(sock, base_path, frame_count=40)

        # 6. 保持连接直到拍摄完成
        #    每帧 = 3 个 UE tick（1 apply + 2 wait）+ DomeCam 拍摄时间
        #    17 相机 × 5 通道 × 2048px 在中端机上约 8~15s / 帧
        #    40 帧 × 15s ≈ 600s；保守起见设 SEQUENCE_WAIT_SEC=300，按需调大
        print(f"[INFO] Waiting {SEQUENCE_WAIT_SEC}s for sequence to finish...")
        time.sleep(SEQUENCE_WAIT_SEC)
        print("[INFO] Done.")


if __name__ == "__main__":
    main()