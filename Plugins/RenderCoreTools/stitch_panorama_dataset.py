import argparse
import json
import os
from pathlib import Path

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")

import cv2
import numpy as np


DEFAULT_DATASET = (
    r"E:\UE_project\demo_createUE_exe\dev_uedemo\Saved\sim_result"
    r"\dataset_20260519_172057"
)


def quat_xyzw_to_matrix(q):
    x, y, z, w = [float(v) for v in q]
    n = x * x + y * y + z * z + w * w
    if n < 1e-12:
        return np.eye(3, dtype=np.float32)
    s = 2.0 / n
    xx, yy, zz = x * x * s, y * y * s, z * z * s
    xy, xz, yz = x * y * s, x * z * s, y * z * s
    wx, wy, wz = w * x * s, w * y * s, w * z * s
    return np.array(
        [
            [1.0 - (yy + zz), xy - wz, xz + wy],
            [xy + wz, 1.0 - (xx + zz), yz - wx],
            [xz - wy, yz + wx, 1.0 - (xx + yy)],
        ],
        dtype=np.float32,
    )


def make_equirect_rays(width, height):
    u = (np.arange(width, dtype=np.float32) + 0.5) / float(width)
    v = (np.arange(height, dtype=np.float32) + 0.5) / float(height)
    uu, vv = np.meshgrid(u, v)

    lon = (uu - 0.5) * (2.0 * np.pi)
    lat = (0.5 - vv) * np.pi

    rays = np.empty((height, width, 3), dtype=np.float32)
    rays[..., 0] = np.cos(lat) * np.cos(lon)
    rays[..., 1] = np.cos(lat) * np.sin(lon)
    rays[..., 2] = np.sin(lat)
    return rays


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def read_image(path, flags):
    img = cv2.imread(str(path), flags)
    if img is None:
        raise RuntimeError(f"Failed to read image: {path}")
    return img


def write_image(path, image):
    if not cv2.imwrite(str(path), image):
        raise RuntimeError(f"Failed to write image: {path}")


def first_existing_field(view, fields):
    for field in fields:
        value = view.get(field)
        if value:
            return value
    return None


def build_projection_maps(meta, width, height):
    views = meta["views"]
    base_view = next((v for v in views if v.get("face_name") == "Front"), views[0])
    base_rot = quat_xyzw_to_matrix(base_view["world_rot_quat"])
    rays_base = make_equirect_rays(width, height)
    flat_rays = rays_base.reshape(-1, 3)

    view_rots = np.stack([quat_xyzw_to_matrix(v["world_rot_quat"]) for v in views])
    relative_rots = np.einsum("ij,njk->nik", base_rot.T, view_rots)
    forwards = relative_rots[:, :, 0]

    dots = flat_rays @ forwards.T
    best = np.argmax(dots, axis=1).reshape(height, width)

    maps = []
    for idx, view in enumerate(views):
        rel = relative_rots[idx]
        local = flat_rays @ rel
        x = local[:, 0].reshape(height, width)
        y = local[:, 1].reshape(height, width)
        z = local[:, 2].reshape(height, width)

        eps = 1e-6
        plane_u = y / np.maximum(x, eps)
        plane_v = z / np.maximum(x, eps)
        valid = (best == idx) & (x > eps) & (np.abs(plane_u) <= 1.0) & (np.abs(plane_v) <= 1.0)

        src_w = int(view.get("width", 1024))
        src_h = int(view.get("height", 1024))
        map_x = ((plane_u + 1.0) * 0.5 * (src_w - 1)).astype(np.float32)
        map_y = ((1.0 - plane_v) * 0.5 * (src_h - 1)).astype(np.float32)
        map_x[~valid] = -1.0
        map_y[~valid] = -1.0
        maps.append((map_x, map_y, valid))

    return maps


def remap_set(frame_dir, views, maps, field_names, output_shape, read_flags, default_channels):
    output = None
    filled = np.zeros(output_shape[:2], dtype=bool)

    for view, (map_x, map_y, valid) in zip(views, maps):
        rel_name = first_existing_field(view, field_names)
        if not rel_name:
            continue
        src_path = frame_dir / rel_name
        if not src_path.exists():
            continue

        src = read_image(src_path, read_flags)
        if src.ndim == 2:
            src = src[..., None]
        if output is None:
            channels = src.shape[2] if src.ndim == 3 else default_channels
            output = np.zeros((*output_shape[:2], channels), dtype=src.dtype)

        sampled = cv2.remap(
            src,
            map_x,
            map_y,
            interpolation=cv2.INTER_LINEAR,
            borderMode=cv2.BORDER_CONSTANT,
            borderValue=0,
        )
        if sampled.ndim == 2:
            sampled = sampled[..., None]
        copy_mask = valid & ~filled
        output[copy_mask] = sampled[copy_mask]
        filled[copy_mask] = True

    return output, filled


def stitch_frame(frame_dir, width, height, overwrite=False):
    frame_dir = Path(frame_dir)
    meta_path = frame_dir / "views_metadata.json"
    if not meta_path.exists():
        return False

    rgb_out = frame_dir / "Stitched_Panorama_RGB.png"
    depth_out = frame_dir / "Stitched_Panorama_Depth.exr"
    position_out = frame_dir / "Stitched_Panorama_Position.exr"
    if not overwrite and rgb_out.exists() and depth_out.exists() and position_out.exists():
        print(f"Skip existing: {frame_dir}")
        return True

    meta = load_json(meta_path)
    views = meta["views"]
    maps = build_projection_maps(meta, width, height)

    rgb, _ = remap_set(
        frame_dir,
        views,
        maps,
        ("filename", "rgb_filename"),
        (height, width),
        cv2.IMREAD_COLOR,
        3,
    )
    if rgb is not None:
        write_image(rgb_out, rgb)

    depth, _ = remap_set(
        frame_dir,
        views,
        maps,
        ("depth_filename",),
        (height, width),
        cv2.IMREAD_UNCHANGED,
        1,
    )
    if depth is not None:
        write_image(depth_out, depth.astype(np.float32))

    position, _ = remap_set(
        frame_dir,
        views,
        maps,
        ("position_filename", "pos_filename"),
        (height, width),
        cv2.IMREAD_UNCHANGED,
        4,
    )
    if position is not None:
        write_image(position_out, position.astype(np.float32))

    print(f"Saved panorama set: {frame_dir}")
    return True


def iter_frame_dirs(dataset):
    dataset = Path(dataset)
    if (dataset / "views_metadata.json").exists():
        yield dataset
        return
    for child in sorted(dataset.iterdir()):
        if child.is_dir() and (child / "views_metadata.json").exists():
            yield child


def main():
    parser = argparse.ArgumentParser(description="Stitch cubemap pinhole views into equirectangular panoramas.")
    parser.add_argument("dataset", nargs="?", default=DEFAULT_DATASET)
    parser.add_argument("--width", type=int, default=4096)
    parser.add_argument("--height", type=int, default=2048)
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    dataset = Path(args.dataset)
    if not dataset.exists():
        raise SystemExit(f"Dataset path does not exist: {dataset}")

    frames = list(iter_frame_dirs(dataset))
    if not frames:
        raise SystemExit(f"No frame folders with views_metadata.json found under: {dataset}")

    print(f"Found {len(frames)} frame(s). Output resolution: {args.width}x{args.height}")
    ok = 0
    for frame in frames:
        try:
            ok += int(stitch_frame(frame, args.width, args.height, args.overwrite))
        except Exception as exc:
            print(f"Failed: {frame} -> {exc}")
    print(f"Done. Successful frames: {ok}/{len(frames)}")


if __name__ == "__main__":
    main()
