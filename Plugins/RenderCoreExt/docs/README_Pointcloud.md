# Point Cloud Visualization Tool

This tool reads position EXR files and RGB PNG images from the RenderCoreExt plugin to create and visualize colored point clouds.

## Installation

Install required Python packages:

```bash
pip install -r requirements.txt
```

## Usage

### Single View

Process a single position/RGB pair:

```bash
python visualize_pointcloud.py View_000_P0_Y0_Position.exr --rgb View_000_P0_Y0.png
```

Save to file:

```bash
python visualize_pointcloud.py View_000_P0_Y0_Position.exr --rgb View_000_P0_Y0.png -o output.ply
```

### Multi-View (Entire Frame)

Process all views in a frame directory automatically:

```bash
python visualize_pointcloud.py Frame_0000/
```

Save merged point cloud:

```bash
python visualize_pointcloud.py Frame_0000/ -o frame_0000.ply
```

### Advanced Options

**Subsample** (reduce point density for faster processing):
```bash
python visualize_pointcloud.py Frame_0000/ --subsample 2
```

**Filter outliers** (remove points beyond certain distance):
```bash
python visualize_pointcloud.py Frame_0000/ --max-distance 1000
```

**No visualization** (just save):
```bash
python visualize_pointcloud.py Frame_0000/ -o output.ply --no-viz
```

## Examples

```bash
# Visualize single view
python visualize_pointcloud.py data/Frame_0000/ERPView_000_P-45_Y-90_Position.exr \
    --rgb data/Frame_0000/ERPView_000_P-45_Y-90.png

# Process and save all views in a frame
python visualize_pointcloud.py data/Frame_0000/ -o pointcloud_frame0.ply

# Subsample and filter for faster preview
python visualize_pointcloud.py data/Frame_0000/ --subsample 4 --max-distance 500
```

## Interactive Controls

In the Open3D viewer:
- **Mouse drag**: Rotate view
- **Scroll**: Zoom in/out
- **Shift + Mouse drag**: Pan
- **Ctrl + Mouse drag**: Roll
- **Q**: Quit

## Supported Output Formats

- `.ply` - Polygon File Format (recommended)
- `.pcd` - Point Cloud Data
- `.xyz` - XYZ ASCII format

## Notes

- Position EXR files contain world-space XYZ coordinates in RGB channels
- The script automatically merges multiple views and removes duplicates using voxel downsampling
- For large point clouds, use `--subsample` to reduce memory usage
