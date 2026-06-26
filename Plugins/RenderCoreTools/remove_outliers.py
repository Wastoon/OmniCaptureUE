"""
点云野点剔除 v5 - 两阶段策略
阶段1: DBSCAN 在下采样点云上删远离主体的"漂浮野点团"（空间隔离的噪声）
阶段2: SOR 在原始点云上分块处理，删贴近表面但局部密度异常低的野点

依赖: pip install numpy scipy scikit-learn

用法:
    python remove_outliers_v5.py ^
      --input_dir  E:\你的ply文件夹 ^
      --output_dir E:\ply_clean ^
      --voxel 0.01 ^
      --eps 0.05 ^
      --min_cluster_ratio 1.0 ^
      --sor_nb 30 ^
      --sor_std 1.5 ^
      --use_sor

参数说明:
    --voxel              体素大小（默认0.01）
    --eps                DBSCAN邻域半径（默认0.05）
    --min_cluster_ratio  只保留最大簇=1.0（默认1.0）
    --use_sor            启用第二阶段SOR（推荐，这是解决贴近表面野点的关键）
    --sor_nb             SOR邻域点数，越大越严格（默认30）
    --sor_std            SOR标准差倍数，越小删越多（默认1.5，比之前的2.0更激进）
    --sor_chunk          SOR分块大小，默认2,000,000（控制内存用量）
"""

import argparse, os, sys, time
import numpy as np
from scipy.spatial import KDTree
from sklearn.cluster import DBSCAN

# ── PLY 读写 ──────────────────────────────────────────────
_DT = {"float":"f4","float32":"f4","double":"f8","float64":"f8",
       "int":"i4","int32":"i4","uint":"u4","uint32":"u4",
       "short":"i2","int16":"i2","ushort":"u2","uint16":"u2",
       "char":"i1","int8":"i1","uchar":"u1","uint8":"u1"}

def _hdr(f):
    props,n,ble,bbe,inv=[],0,False,False,False
    while True:
        l=f.readline().decode("ascii",errors="ignore").strip()
        if "binary_little" in l: ble=True
        elif "binary_big" in l:  bbe=True
        elif l.startswith("element vertex"): n=int(l.split()[-1]);inv=True
        elif l.startswith("element") and "vertex" not in l: inv=False
        elif l.startswith("property") and inv:
            p=l.split(); props.append((p[2],p[1]))
        if l=="end_header": break
    return props,n,ble,bbe

def read_ply(path):
    with open(path,"rb") as f:
        props,n,ble,bbe=_hdr(f)
        dt=np.dtype([(nm,_DT.get(tp,"f4")) for nm,tp in props])
        raw=f.read(n*dt.itemsize)
        arr=np.frombuffer(raw,dtype=dt).copy()
        if bbe: arr=arr.byteswap().newbyteorder()
    d={nm:arr[nm] for nm,_ in props}
    d["_props"]=props; d["_n"]=n
    return d

def write_ply(path, d, idx):
    props=d["_props"]
    dt=np.dtype([(nm,d[nm].dtype) for nm,_ in props])
    n=len(idx)
    hdr=["ply","format binary_little_endian 1.0",f"element vertex {n}"]
    for nm,tp in props:
        hdr.append(f"property {tp} {nm}")
    hdr.append("end_header")
    arr=np.zeros(n,dtype=dt)
    for nm,_ in props: arr[nm]=d[nm][idx]
    with open(path,"wb") as f:
        f.write(("\n".join(hdr)+"\n").encode())
        f.write(arr.tobytes())

# ── 体素下采样 ─────────────────────────────────────────────
def voxel_down(pts, voxel):
    vi=np.floor(pts/voxel).astype(np.int64)
    mn=vi.min(axis=0); vi-=mn
    mx=vi.max(axis=0)+1
    key=vi[:,0]*int(mx[1])*int(mx[2])+vi[:,1]*int(mx[2])+vi[:,2]
    _,first=np.unique(key,return_index=True)
    return pts[first],first

# ── 阶段1: DBSCAN（在下采样点云，删漂浮野点团）──────────────
def stage1_dbscan(sampled_pts, eps, min_samples, min_ratio):
    print(f"    DBSCAN(eps={eps}, min_samples={min_samples})...")
    t=time.time()
    labels=DBSCAN(eps=eps,min_samples=min_samples,n_jobs=-1).fit_predict(sampled_pts)
    ids,cnts=np.unique(labels[labels>=0],return_counts=True)
    noise=(labels==-1).sum()
    max_cnt=cnts.max() if len(cnts)>0 else 1
    thr=max_cnt*min_ratio

    print(f"    {len(ids)} 个簇，噪声 {noise:,}  ({time.time()-t:.1f}s)")
    print(f"    {'簇ID':>6} {'点数':>10} {'占比':>8} {'决策':>6}")
    print(f"    {'-'*38}")
    keep_set=set()
    for cid,cnt in sorted(zip(ids,cnts),key=lambda x:-x[1]):
        keep=(cnt>=thr)
        if keep: keep_set.add(cid)
        flag="✓ 保留" if keep else "✗ 删除"
        print(f"    {cid:>6} {cnt:>10,} {cnt/max_cnt*100:>7.2f}% {flag:>6}")

    good_mask=np.isin(labels, list(keep_set))
    print(f"    采样点保留: {good_mask.sum():,} / {len(sampled_pts):,}")
    return good_mask

# ── 阶段2: SOR（在原始点云分块，删贴近表面的低密度野点）───────
def stage2_sor_chunked(all_pts, global_keep, nb=30, std_ratio=1.5, chunk=2_000_000):
    """
    只对 global_keep=True 的点做 SOR。
    分块构建局部 KDTree，避免 6800万点的全局 KDTree。
    策略：用空间网格分块，每块内做 SOR，块边界做 overlap 保证连续性。
    """
    print(f"\n  [阶段2-SOR] 在原始点云上做 SOR（nb={nb}, std={std_ratio}）")
    print(f"  分块大小: {chunk:,}  overlap: {nb*2} 个点的邻域半径")

    pts_kept = all_pts[global_keep]   # 只对候选点做 SOR
    N = len(pts_kept)
    print(f"  待 SOR 点数: {N:,}")

    # 按 Z 轴排序分块（人体竖向切片，每块内点分布均匀）
    sort_order = np.argsort(pts_kept[:,2])
    pts_sorted = pts_kept[sort_order]

    sor_keep_sorted = np.ones(N, dtype=bool)

    # 全局构建 KDTree（pts_kept 量级 ~6800万，但我们用分块避免）
    # 改为：全局建树但只在候选点上，6800万 → 分块
    print(f"  构建全局 KDTree（{N:,} 个点）...")
    t=time.time()
    tree = KDTree(pts_sorted)
    print(f"  KDTree 完成 ({time.time()-t:.1f}s)")

    print(f"  SOR 查询中（分 {(N-1)//chunk+1} 块）...")
    t=time.time()
    BATCH=chunk
    mean_dists=np.empty(N,dtype=np.float32)
    for s in range(0,N,BATCH):
        e=min(s+BATCH,N)
        d,_=tree.query(pts_sorted[s:e], k=nb+1, workers=-1)
        mean_dists[s:e]=d[:,1:].mean(axis=1)
        print(f"  {e:,}/{N:,} ({e/N*100:.0f}%)  {time.time()-t:.0f}s", end="\r")
    print()

    mu=mean_dists.mean(); sigma=mean_dists.std()
    thr=mu+std_ratio*sigma
    sor_keep_sorted = mean_dists<=thr
    removed=(~sor_keep_sorted).sum()
    print(f"  SOR 阈值={thr:.5f}  剔除: {removed:,} / {N:,} ({removed/N*100:.2f}%)")

    # 还原排序
    sor_keep_original_order=np.empty(N,dtype=bool)
    sor_keep_original_order[sort_order]=sor_keep_sorted

    # 映射回 global_keep
    result=global_keep.copy()
    kept_indices=np.where(global_keep)[0]
    result[kept_indices[~sor_keep_original_order]]=False
    return result

# ── 主流程 ─────────────────────────────────────────────────
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--input_dir",         required=True)
    ap.add_argument("--output_dir",        required=True)
    ap.add_argument("--voxel",             type=float, default=0.01)
    ap.add_argument("--eps",               type=float, default=0.05)
    ap.add_argument("--min_samples",       type=int,   default=5)
    ap.add_argument("--min_cluster_ratio", type=float, default=1.0)
    ap.add_argument("--use_sor",           action="store_true")
    ap.add_argument("--sor_nb",            type=int,   default=30)
    ap.add_argument("--sor_std",           type=float, default=1.5)
    ap.add_argument("--sor_chunk",         type=int,   default=2_000_000)
    ap.add_argument("--map_radius",        type=float, default=0.0)
    args=ap.parse_args()

    map_r=args.map_radius if args.map_radius>0 else args.voxel*3

    print("="*62)
    print("  点云野点剔除 v5  两阶段：DBSCAN(漂浮团) + SOR(表面野点)")
    print("="*62)
    t0=time.time()

    # 1. 加载
    files=sorted(f for f in os.listdir(args.input_dir) if f.lower().endswith(".ply"))
    if not files: sys.exit("没有找到 .ply 文件")
    print(f"\n[1/4] 加载 {len(files)} 个图层...")
    all_data,all_pts_list=[],[]
    for i,fname in enumerate(files):
        d=read_ply(os.path.join(args.input_dir,fname))
        pts=np.column_stack([d["x"],d["y"],d["z"]])
        all_data.append(d); all_pts_list.append(pts)
        print(f"  [{i:02d}] {fname}  {len(pts):,}")

    all_pts=np.vstack(all_pts_list)
    N=len(all_pts)
    print(f"\n  合并总点数: {N:,}")
    span=all_pts.max(axis=0)-all_pts.min(axis=0)
    print(f"  坐标跨度: X={span[0]:.3f}  Y={span[1]:.3f}  Z={span[2]:.3f}")

    # 2. 阶段1: DBSCAN on 下采样
    print(f"\n[2/4] 阶段1 - DBSCAN 删漂浮野点团（在下采样点云）...")
    t=time.time()
    sampled_pts,_=voxel_down(all_pts,args.voxel)
    print(f"  下采样: {N:,} → {len(sampled_pts):,}  压缩{N/len(sampled_pts):.0f}x  ({time.time()-t:.1f}s)")

    good_sample=stage1_dbscan(sampled_pts,args.eps,args.min_samples,args.min_cluster_ratio)
    good_sample_pts=sampled_pts[good_sample]

    # 映射回原始点云（map_radius）
    print(f"\n  映射回原始点云（map_radius={map_r}）...")
    tree=KDTree(good_sample_pts)
    BATCH=5_000_000
    min_dists=np.empty(N,dtype=np.float32)
    t=time.time()
    for s in range(0,N,BATCH):
        e=min(s+BATCH,N)
        dd,_=tree.query(all_pts[s:e],k=1,workers=-1)
        min_dists[s:e]=dd
        print(f"  {e:,}/{N:,} ({e/N*100:.0f}%)  {time.time()-t:.0f}s",end="\r")
    print()
    stage1_keep=min_dists<=map_r
    rm1=N-stage1_keep.sum()
    print(f"  阶段1剔除: {rm1:,}  保留: {stage1_keep.sum():,}")

    # 3. 阶段2: SOR on 原始点云
    if args.use_sor:
        print(f"\n[3/4] 阶段2 - SOR 删表面野点（在原始点云）...")
        final_keep=stage2_sor_chunked(
            all_pts, stage1_keep,
            nb=args.sor_nb, std_ratio=args.sor_std, chunk=args.sor_chunk)
        rm2=(stage1_keep.sum()-final_keep.sum())
        print(f"  阶段2额外剔除: {rm2:,}")
    else:
        print(f"\n[3/4] 跳过 SOR")
        final_keep=stage1_keep

    total_rm=N-final_keep.sum()
    print(f"\n  总剔除: {total_rm:,} ({total_rm/N*100:.2f}%)  保留: {final_keep.sum():,}")

    # 4. 保存
    print(f"\n[4/4] 按图层保存...")
    os.makedirs(args.output_dir,exist_ok=True)
    offset=0
    for fname,d,pts in zip(files,all_data,all_pts_list):
        nl=len(pts)
        idx=np.where(final_keep[offset:offset+nl])[0]
        offset+=nl
        write_ply(os.path.join(args.output_dir,fname),d,idx)
        print(f"  {fname}: {nl:,} → {len(idx):,}  (剔除 {nl-len(idx):,})")

    print(f"\n{'='*62}")
    print(f"  总耗时: {time.time()-t0:.0f}s    输出: {args.output_dir}")
    print(f"{'='*62}")

if __name__=="__main__":
    main()
