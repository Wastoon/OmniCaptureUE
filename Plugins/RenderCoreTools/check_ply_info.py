"""
快速检查 PLY 点云的坐标范围和点数
用法：python check_ply_info.py --input_dir ./ply文件夹
"""
import argparse, os, sys, struct
import numpy as np

_DTYPE_MAP = {
    "float":"f4","float32":"f4","double":"f8","float64":"f8",
    "int":"i4","int32":"i4","uint":"u4","uint32":"u4",
    "short":"i2","int16":"i2","ushort":"u2","uint16":"u2",
    "char":"i1","int8":"i1","uchar":"u1","uint8":"u1",
}

def read_header(f):
    props, n, bin_le, bin_be, in_v = [], 0, False, False, False
    while True:
        line = f.readline().decode("ascii", errors="ignore").strip()
        if "binary_little" in line: bin_le = True
        elif "binary_big" in line:  bin_be = True
        elif line.startswith("element vertex"):
            n = int(line.split()[-1]); in_v = True
        elif line.startswith("element") and "vertex" not in line:
            in_v = False
        elif line.startswith("property") and in_v:
            p = line.split(); props.append((p[2], p[1]))
        if line == "end_header": break
    return props, n, bin_le, bin_be

def sample_pts(path, max_pts=200_000):
    """只读前 max_pts 个点用于快速统计"""
    with open(path, "rb") as f:
        props, n, bin_le, bin_be = read_header(f)
        dt = np.dtype([(nm, _DTYPE_MAP.get(tp,"f4")) for nm, tp in props])
        read_n = min(n, max_pts)
        raw = f.read(read_n * dt.itemsize)
        arr = np.frombuffer(raw, dtype=dt).copy()
        if bin_be: arr = arr.byteswap().newbyteorder()
    return arr, n, dt

ap = argparse.ArgumentParser()
ap.add_argument("--input_dir", required=True)
args = ap.parse_args()

files = sorted(f for f in os.listdir(args.input_dir) if f.lower().endswith(".ply"))
if not files:
    sys.exit("没有找到 .ply 文件")

print(f"{'='*70}")
print(f"{'文件名':<35} {'点数':>12}  {'X范围':>20}  {'Y范围':>20}  {'Z范围':>20}")
print(f"{'='*70}")

all_mins, all_maxs = [], []
for fname in files:
    arr, n_total, dt = sample_pts(os.path.join(args.input_dir, fname))
    has_xyz = all(k in dt.names for k in ("x","y","z"))
    if not has_xyz:
        print(f"{fname:<35} {'无XYZ属性':>12}")
        continue
    x,y,z = arr["x"], arr["y"], arr["z"]
    xr = f"[{x.min():.1f}, {x.max():.1f}]"
    yr = f"[{y.min():.1f}, {y.max():.1f}]"
    zr = f"[{z.min():.1f}, {z.max():.1f}]"
    print(f"{fname:<35} {n_total:>12,}  {xr:>20}  {yr:>20}  {zr:>20}")
    all_mins.append([x.min(), y.min(), z.min()])
    all_maxs.append([x.max(), y.max(), z.max()])

if all_mins:
    mn = np.min(all_mins, axis=0)
    mx = np.max(all_maxs, axis=0)
    span = mx - mn
    print(f"{'='*70}")
    print(f"全局范围:  X[{mn[0]:.2f} ~ {mx[0]:.2f}]  Y[{mn[1]:.2f} ~ {mx[1]:.2f}]  Z[{mn[2]:.2f} ~ {mx[2]:.2f}]")
    print(f"模型跨度:  X={span[0]:.2f}  Y={span[1]:.2f}  Z={span[2]:.2f}  最大跨度={span.max():.2f}")
    print()
    # 推荐参数
    max_span = span.max()
    voxel    = round(max_span / 1000, 2)   # 采样后约100万点以内
    eps      = round(max_span / 200, 2)
    print(f"【推荐参数】（基于模型跨度 {max_span:.1f}）")
    print(f"  --voxel {voxel}  --eps {eps}  --min_samples 10 --use_sor")
    print(f"  采样后预估点数: ~{int(68_583_605 * (voxel**3) / max_span**3 * 1e6):,}（粗估）")
