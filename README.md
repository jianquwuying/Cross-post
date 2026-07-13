# 视觉识别系统 — YOLO26n + NCNN + OpenCV 树莓派5

基于 Raspberry Pi 5 Ubuntu 24.04 ARM64 的实时视觉识别系统。
YOLO26n 目标检测 + HSV 颜色识别 + UART 串口通信，支持摄像头/视频双输入模式。

## 系统架构

```
输入源                    推理引擎                  输出
┌──────────────┐    ┌──────────────────┐    ┌─────────────────┐
│ CameraSource │    │  ROI (可选)       │    │  UART Serial    │
│ VideoSource  │───▶│  YOLO26n NCNN    │───▶│  12字节二进制协议 │
│ (未来: RTSP)  │    │  HSV Color       │    │  cv::imshow     │
└──────────────┘    │  FP16 / INT8     │    │  VideoWriter    │
      ▲             └──────────────────┘    └─────────────────┘
      │
  YAML 配置切换
  (无需重编译)
```

| 组件 | 技术栈 |
|------|--------|
| 推理引擎 | YOLO26n + NCNN (CPU + NEON + OpenMP) |
| 精度模式 | FP16 (高精度) / INT8 (高帧率) |
| 图像处理 | OpenCV 4.8 (BGR→HSV, 形态学, 绘制) |
| 颜色识别 | HSV 掩膜 + 颜色置信度 (红/蓝/黄) |
| 串口协议 | 12字节二进制包 + CRC8 校验 |
| 配置管理 | YAML (yaml-cpp), 修改无需重编译 |
| 日志系统 | ANSI 4级彩色终端日志 |
| 构建系统 | CMake 3.20+, C++17, ARM64 |

---

## 快速开始

### 环境要求

| 组件 | 版本 | 安装命令 |
|------|------|---------|
| GCC | 13.3.0+ | 系统自带 |
| CMake | 3.20+ | 系统自带 |
| OpenCV | 4.8.0 | 已安装于 `/usr/local` |
| yaml-cpp | 0.8.0 | `sudo apt install libyaml-cpp-dev` |
| protobuf | 3.21+ | `sudo apt install protobuf-compiler libprotobuf-dev` |
| NCNN | latest | 从源码编译 (见下方) |

### 1. 编译 NCNN

```bash
git clone https://github.com/Tencent/ncnn.git
cd ncnn && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DNCNN_VULKAN=OFF \
      -DNCNN_OPENMP=ON \
      -DNCNN_BUILD_TOOLS=ON \
      -DNCNN_BUILD_BENCHMARK=ON \
      -DNCNN_BUILD_EXAMPLES=OFF \
      ..
make -j4
sudo make install
sudo ldconfig
```

### 2. 编译项目

```bash
cd /home/shu/Cross-post
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j4
```

产出三个可执行文件:

| 程序 | 用途 |
|------|------|
| `vision_system` | 生产部署: 摄像头 + YOLO + HSV + 串口 |
| `vision_demo` | 离线调试: 摄像头/视频 + YOLO + HSV + 显示 + 录像 |
| `dataset_capture` | 数据集采集: 摄像头原始图片保存, 用于 YOLO 标注 |

### 3. 放置模型

```
model/
├── fp16/
│   ├── model.ncnn.param
│   └── model.ncnn.bin
└── int8/
    ├── model.ncnn.param
    ├── model.ncnn.bin
    └── calibration.table
```

### 4. 运行

```bash
# 生产模式 (串口输出)
./vision_system                               # FP16 默认
./vision_system --model int8                  # INT8 高帧率
./vision_system --config my_config.yaml       # 自定义配置

# Demo 模式 (显示+录像+Benchmark)
./vision_demo                                 # 使用 config/vision.yaml
./vision_demo --config config/vision.yaml     # input.type: camera | video

# 数据集采集 (原始图片, 不推理)
cd tools/dataset_capture && mkdir build && cd build
cmake .. && make -j4
./dataset_capture                             # 使用 vision.yaml
```

---

## 配置说明

所有配置由单一文件 `config/vision.yaml` 控制，修改无需重编译。通过 `input.type` 切换运行模式。

```yaml
# ── 输入源选择 ──────────────────────────
input:
  type: "camera"          # "camera" | "video"

# ── 摄像头 (input.type=camera) ──────────
camera:
  id: 0; width: 640; height: 480; fps: 30
  fourcc: "MJPG"; buffer_size: 1

# ── 视频 (input.type=video) ─────────────
video:
  path: "./data/test.mp4"
  loop: false

# ── YOLO 推理 ──────────────────────────
yolo:
  model_dir: "model"
  model_type: "int8"      # fp16 | int8
  input_width: 320; input_height: 320
  num_threads: 4
  conf_threshold: 0.25; nms_threshold: 0.45

# ── 检测控制 ──────────────────────────
detect:
  skip_frames: 0

# ── ROI 裁剪 ──────────────────────────
roi:
  enabled: false
  x: 0; y: 0; width: 640; height: 480

# ── HSV 颜色识别 ──────────────────────
color:
  threshold: 0.15; conf_threshold: 0.15
  morph_kernel: 3; blur_kernel: 3
  red1:  { h_low: 0,   s_low: 100, v_low: 100, h_high: 10,  s_high: 255, v_high: 255 }
  red2:  { h_low: 170, s_low: 100, v_low: 100, h_high: 180, s_high: 255, v_high: 255 }
  blue:  { h_low: 100, s_low: 100, v_low: 50,  h_high: 130, s_high: 255, v_high: 255 }
  yellow:{ h_low: 20,  s_low: 100, v_low: 100, h_high: 35,  s_high: 255, v_high: 255 }

# ── 串口 ──────────────────────────────
serial:
  device: "/dev/ttyAMA0"; baudrate: 115200
  data_bits: 8; stop_bits: 1; parity: "N"

# ── 显示 (vision_demo) ──────────────────
display:
  enable: true
  window_name: "YOLO26n Vision"

# ── 输出 (vision_demo) ──────────────────
output:
  save_video: false; path: "./result.mp4"

# ── Benchmark (vision_demo) ─────────────
benchmark:
  enable: true

# ── 日志 ──────────────────────────────
log:
  level: "info"; use_color: true
```

两个程序共用同一配置文件。`vision_system` 忽略 display/output/benchmark 节 (无串口时自动降级)。`vision_demo` 忽略 serial 节。

---

## 二进制通信协议

```
字节:  [0]    [1]    [2]   [3]      [4]        [5]     [6:7]   [8:9]   [10]     [11]
字段:  0xAA   0x55   数量  类别     颜色       颜色置信  cx      cy      YOLO置信  CRC8
```

| 字节 | 字段 | 说明 |
|------|------|------|
| 0-1 | 同步头 | 0xAA 0x55, 接收端重同步 |
| 2 | 数量 | 本帧检测目标数 (1-32) |
| 3 | 类别 | 0=A类, 1=B类 |
| 4 | 颜色 | 0=未知, 1=红, 2=蓝, 3=黄 |
| 5 | 颜色置信度 | ×100 量化 (0-100), B类恒为0 |
| 6-7 | cx | 中心X, uint16大端序, 0-10000 |
| 8-9 | cy | 中心Y, uint16大端序, 0-10000 |
| 10 | YOLO置信度 | ×100 量化 (0-100) |
| 11 | CRC8 | 多项式 0x07, 校验字节 [2..10] |

每包 12 字节，自包含同步头和校验，支持流内重同步。

---

## 性能预估

### 树莓派5 (Cortex-A76 × 4, NEON, 无GPU)

| 输入尺寸 | FP16 | INT8 | 适用场景 |
|----------|------|------|---------|
| 320×320  | 15-18 FPS | 22-28 FPS | 实时检测 (推荐) |
| 416×416  | 8-12 FPS  | 12-18 FPS | 精度优先 |
| 640×640  | 3-5 FPS   | 6-10 FPS  | 高精度离线 |

> 启用 50% ROI 裁剪可额外提升 30-40%。检测目标数量和 HSV 计算量会影响实际 FPS。

### 性能优化优先级

1. **INT8 量化** — FPS 提升最大 (~60%)
2. **ROI 裁剪** — 计算量与裁剪面积成正比
3. **跳帧控制** (`detect.skip_frames`) — 降采样检测
4. **输入尺寸** — 320×320 为实时性最优解
5. **OpenMP 线程** — 默认 4 线程 (匹配 4 核)
6. **ARM NEON** — NCNN 自动启用, 无需配置

---

## 项目结构

```
Cross-post/
├── CMakeLists.txt                  # 两个 target: vision_system + vision_demo
├── main.cpp                        # 生产流水线 (Camera → YOLO → HSV → Serial)
├── camera/
│   └── camera.h/cpp                # V4L2 采集 (MJPEG/YUYV, EMA FPS)
├── yolo/
│   └── yolo26.h/cpp                # NCNN 推理 (3种输出格式, NMS, FP16/INT8)
├── color/
│   └── hsv.h/cpp                   # HSV 颜色分类 (双阈值, 形态学, 颜色置信度)
├── serial/
│   └── serial.h/cpp                # POSIX termios 串口 (零依赖)
├── protocol/
│   └── protocol.h                  # 12字节二进制协议 + CRC8
├── config/
│   ├── config.h/cpp                # YAML 配置管理
│   └── vision.yaml                 # 统一配置文件
├── roi/
│   └── roi.h/cpp                   # ROI 裁剪 + 坐标映射
├── logger/
│   └── logger.h/cpp                # ANSI 彩色日志 (4级)
├── src/input/
│   ├── frame_source.hpp            # FrameSource 抽象接口
│   ├── camera_source.hpp           # CameraSource 实现
│   ├── video_source.hpp            # VideoSource 实现 (支持循环)
│   └── frame_source_factory.hpp/cpp # 工厂: YAML → unique_ptr<FrameSource>
├── src/vision_demo.cpp             # Demo 程序 (显示/录像/Benchmark)
├── model/
│   ├── fp16/.gitkeep
│   └── int8/.gitkeep
├── tools/dataset_capture/          ⭐ v1.4 新增
│   ├── CMakeLists.txt              # 独立构建 (OpenCV + yaml-cpp)
│   ├── main.cpp                    # 入口
│   ├── config.h/cpp                # 轻量配置
│   ├── dataset_capture.h/cpp       # 采集类 (增量保存/快捷键/叠加显示)
│   └── vision.yaml                 # 默认配置
└── README.md
```

---

## 设计决策

### YOLO26n × YOLO11n

YOLO26n 胜出: C2PSA 注意力模块、更低 FLOPs (7.8G vs 8.1G)、更高 mAP (37.5% vs 37.0%)、针对边缘优化的无锚框头。

### NCNN × ONNX Runtime × TensorRT

NCNN 胜出: 无 CUDA 依赖、ARM NEON 原生优化、极小内存占用、支持 FP16/INT8。

### YOLO + HSV 分离 × YOLO 包含颜色

分离胜出: 更少类别→更快推理；HSV 确定性判定→无需训练颜色；光照变化通过调整 HSV 范围应对, 比重训模型简单。

### CPU + NEON × Vulkan GPU

CPU 胜出: ARM CPU 方案成熟稳定、驱动依赖少、便于调试。树莓派5 GPU Vulkan 加速收益不确定。INT8 量化 + OpenMP 多线程已可达到实时帧率。

### 工厂模式 × if-else 分支

工厂模式胜出: `FrameSourceFactory::createFromYaml()` 避免硬编码输入源类型, 新增 RTSP 等输入源只需实现 FrameSource 接口, 无需修改调用代码。

---

## 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v1.0 | 2026-07-11 | 基础框架: Camera → YOLO → HSV → Serial |
| v1.1 | 2026-07-12 | 7项优化: 颜色置信度/ROI/YAML/FP16+INT8/Logger/多目标/数据增强 |
| v1.2 | 2026-07-12 | 视频推理: FrameSource 抽象/VideoSource/vision_demo/Benchmark |
| v1.3 | 2026-07-12 | YAML 合并: default.yaml + vision.yaml → 单一 vision.yaml |
| v1.4 | 2026-07-13 | Dataset Capture: 原始数据集采集工具 (独立构建, 不影响推理模块) |

---

## ⚠️ 上线前检查清单

| # | 任务 | 位置 | 完成标志 |
|---|------|------|---------|
| 1 | `sudo apt install protobuf-compiler libprotobuf-dev libyaml-cpp-dev` | RPi5 | `dpkg -l \| grep yaml-cpp-dev` 有输出 |
| 2 | 编译 NCNN (CPU + NEON + OpenMP) | RPi5 | `/usr/local/lib/libncnn.a` 存在 |
| 3 | 连接摄像头, `v4l2-ctl --list-devices` 验证 | RPi5 | 能看到 `/dev/video0` |
| 4 | 准备 A/B 两类标注数据集 (~500张/类) | 笔记本 | `data.yaml` + images + labels 齐全 |
| 5 | 训练 `yolo detect train model=yolo26n.pt epochs=100 imgsz=320` | 笔记本 | mAP@0.5 > 0.85 |
| 6 | 导出 NCNN: FP16 + INT8 → `model/fp16/` + `model/int8/` | 笔记本 | param + bin 文件就位 |
| 7 | `cmake .. && make -j4 && ./vision_system --model int8` | RPi5 | 稳定输出检测结果 |

---

## 许可证

MIT
