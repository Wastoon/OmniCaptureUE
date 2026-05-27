# 深度图可视化工具使用说明

## 功能

这些脚本用于可视化UE5导出的z-buffer深度图，支持：
- PNG格式深度图（8位或16位）
- RAW格式深度图（32位浮点）
- 多种可视化方法（colormap、grayscale、heatmap）
- 批量处理整个数据集

## 文件说明

- `visualize_depth.py` - 单个深度图可视化脚本
- `batch_visualize_depth.py` - 批量可视化脚本
- `README_VISUALIZATION.md` - 本说明文件

## 安装依赖

```bash
pip install numpy opencv-python matplotlib tqdm
```

## 使用方法

### 1. 单个文件可视化

```bash
python visualize_depth.py <rgb_image> <depth_image> [options]
```

**示例：**
```bash
# 基本使用
python visualize_depth.py frame_00001_rgb_00001.png frame_00001_depth_00001.png

# 使用colormap方法，反转深度
python visualize_depth.py frame_00001_rgb_00001.png frame_00001_depth_00001.png --method colormap --invert

# 可视化RAW格式深度图
python visualize_depth.py frame_00001_rgb_00001.png frame_00001_depth_00001.raw --width 1280 --height 720

# 指定输出路径并显示窗口
python visualize_depth.py frame_00001_rgb_00001.png frame_00001_depth_00001.png --output result.png --show
```

**参数说明：**
- `rgb` - RGB图像路径（必需）
- `depth` - 深度图路径（必需，支持.png或.raw）
- `--output, -o` - 输出路径（可选，默认自动生成）
- `--method, -m` - 可视化方法：`colormap`（默认）、`grayscale`、`heatmap`
- `--invert, -i` - 反转深度（近处亮）
- `--min-depth` - 自定义最小深度值
- `--max-depth` - 自定义最大深度值
- `--show, -s` - 显示可视化窗口
- `--width` - 图像宽度（RAW文件必需）
- `--height` - 图像高度（RAW文件必需）

### 2. 批量可视化

```bash
python batch_visualize_depth.py <input_directory> [options]
```

**示例：**
```bash
# 批量处理整个数据集
python batch_visualize_depth.py C:/UE5_Exports/dataset_001

# 指定输出目录和使用colormap方法
python batch_visualize_depth.py C:/UE5_Exports/dataset_001 --output-dir C:/Visualized --method colormap

# 跳过已存在的文件
python batch_visualize_depth.py C:/UE5_Exports/dataset_001 --skip-existing
```

**参数说明：**
- `input_dir` - 输入目录（包含RGB和深度图像）
- `--output-dir, -o` - 输出目录（默认：input_dir/visualized）
- `--method, -m` - 可视化方法
- `--invert, -i` - 反转深度
- `--skip-existing` - 跳过已存在的文件
- `--width` - 图像宽度（RAW文件，默认1280）
- `--height` - 图像高度（RAW文件，默认720）

## 可视化方法说明

### 1. Colormap（默认）
使用matplotlib的jet colormap，颜色从蓝色（远）到红色（近），适合观察深度变化。

### 2. Grayscale
灰度图，黑色=远，白色=近（或相反，如果使用--invert）。

### 3. Heatmap
使用OpenCV的COLORMAP_JET，效果类似colormap但实现方式不同。

## 输出文件

脚本会生成两个文件：
1. `*_visualized.png` - 深度可视化图像
2. `*_comparison.png` - RGB和深度的并排对比图

## 文件命名约定

脚本会自动识别以下命名模式：
- RGB图像：`*_rgb_*.png` 或 `*_rgb.png`
- 深度图像：`*_depth_*.png` 或 `*_depth.raw`
- 位姿文件：`*_pose_*.json` 或 `*_pose.json`

## 注意事项

1. **RAW文件尺寸**：如果使用RAW格式，必须指定`--width`和`--height`参数，或者脚本会尝试自动推断。

2. **深度范围**：z-buffer深度值在0-1范围，但分布不均匀（近处精度高，远处精度低）。脚本会自动计算有效范围进行归一化。

3. **无效值处理**：深度值为0或>=1的像素会被标记为黑色（无效区域）。

4. **性能**：批量处理大量文件时，建议使用`--skip-existing`参数避免重复处理。

## 示例输出

运行后会看到类似输出：
```
Loading depth image: frame_00001_depth_00001.png
Depth image loaded: (720, 1280), type: png
Depth range: [0.023456, 0.987654]
Visualizing depth with method: colormap
Depth visualization range: [0.023456, 0.987654]
Creating comparison view...
Saved depth visualization to: frame_00001_depth_00001_visualized.png
Saved comparison image to: frame_00001_depth_00001_comparison.png
```

## 故障排除

1. **无法加载深度图**：检查文件路径和格式是否正确
2. **RAW文件尺寸错误**：确保指定正确的`--width`和`--height`
3. **内存不足**：处理大图像时可能需要更多内存
4. **显示窗口不出现**：确保安装了GUI支持（Linux可能需要X11）

## 高级用法

### 自定义深度范围

如果知道深度范围，可以手动指定：
```bash
python visualize_depth.py rgb.png depth.png --min-depth 0.1 --max-depth 0.9
```

### 在Python代码中使用

```python
from visualize_depth import load_depth_image, visualize_depth
import cv2

# 加载深度图
depth, depth_type = load_depth_image('depth.raw')

# 可视化
depth_vis, min_depth, max_depth = visualize_depth(
    depth, 
    method='colormap',
    invert=False
)

# 保存
cv2.imwrite('visualized.png', depth_vis)
```
