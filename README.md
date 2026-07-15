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
| 摄像头控制 | CameraConfig YAML 驱动, V4L2 ioctl (曝光/白平衡/像素格式) |
| 配置管理 | YAML (yaml-cpp), 严格解析, 缺失字段直接报错退出 |
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
| `dataset_capture` | 数据集采集: 共用 Camera 类, 自动保存原始图片, 用于 YOLO 标注 |

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

### 摄像头参数 (CameraConfig)

**所有字段必须完整填写，缺失直接报错退出。程序不设任何默认值。**

```yaml
# ── 输入源选择 ──────────────────────────
input:
  type: "camera"          # "camera" | "video"

# ── 摄像头 (input.type=camera 时生效) ──
camera:
  device: 0               # /dev/videoX 设备索引
  backend: "V4L2"         # 后端: "V4L2" | "ANY"
  width: 640              # 采集宽度
  height: 480             # 采集高度
  fps: 60                 # 目标帧率 (MJPG 下全分辨率可达 60fps)
  fourcc: "MJPG"          # 像素格式: "MJPG" (低CPU/高帧率) | "YUYV" (未压缩/低帧率)
  buffer_size: 1          # V4L2 缓冲区数量 (1=最低延迟)
  auto_exposure: false    # true=自动曝光 | false=手动曝光
  exposure: 156           # 手动曝光值 (1-10000), 仅 auto_exposure=false 时生效
  auto_white_balance: false  # true=自动白平衡 | false=手动白平衡
  white_balance: 4600     # 白平衡色温/Kelvin (2800-6500), 仅 auto_wb=false 时生效
```

启动后程序按顺序初始化:

```
Open /dev/videoX (V4L2)
  → Set FourCC (MJPG/YUYV)
  → Set Resolution
  → Set FPS
  → Set Buffer Size
  → Set Exposure (auto/manual)
  → Set White Balance (auto/manual, V4L2 ioctl)
  → Verify & Print ANSI Table
```

示例输出:

```
┌──────────────────────────────────────────────────────┐
│               Camera Parameters                      │
├──────────┬──────────────┬──────────────┬─────────────┤
│ Param    │ Requested    │ Actual       │ Status      │
├──────────┼──────────────┼──────────────┼─────────────┤
│ device   │ 0            │ 0            │ OK          │
│ backend  │ V4L2         │ V4L2         │ OK          │
│ fourcc   │ MJPG         │ MJPG         │ OK          │
│ width    │ 640          │ 640          │ OK          │
│ height   │ 480          │ 480          │ OK          │
│ fps      │ 60           │ 60           │ OK          │
│ buf_size │ 1            │ 1            │ OK          │
│ auto_exp │ false        │ false        │ OK          │
│ exposure │ 156          │ 166          │ WARN        │
│ auto_wb  │ false        │ false        │ OK          │
│ wb_temp  │ 4600         │ 4600         │ OK          │
└──────────┴──────────────┴──────────────┴─────────────┘
```

`WARN` 表示驱动未完全支持请求值 (曝光在 V4L2 中为离散步进, 会就近取值)。

### 视频参数

```yaml
# ── 视频 (input.type=video) ─────────────
video:
  path: "./data/test.mp4"
  loop: false
```

### YOLO 检测

```yaml
# ── YOLO 推理 ──────────────────────────
yolo:
  model_dir: "model"
  model_type: "int8"      # fp16 | int8
  input_width: 320
  input_height: 320
  num_threads: 4
  conf_threshold: 0.25
  nms_threshold: 0.45
```

### 检测控制

```yaml
detect:
  skip_frames: 0          # 0=每帧检测, 1=隔帧, 2=每3帧
```

### ROI 裁剪

```yaml
roi:
  enabled: false
  x: 0; y: 0
  width: 640; height: 480
```

### HSV 颜色识别

```yaml
color:
  threshold: 0.15; conf_threshold: 0.15
  morph_kernel: 3; blur_kernel: 3
  red1:  { h_low: 0,   s_low: 100, v_low: 100, h_high: 10,  s_high: 255, v_high: 255 }
  red2:  { h_low: 170, s_low: 100, v_low: 100, h_high: 180, s_high: 255, v_high: 255 }
  blue:  { h_low: 100, s_low: 100, v_low: 50,  h_high: 130, s_high: 255, v_high: 255 }
  yellow:{ h_low: 20,  s_low: 100, v_low: 100, h_high: 35,  s_high: 255, v_high: 255 }
```

### 串口

```yaml
serial:
  device: "/dev/ttyAMA0"; baudrate: 115200
  data_bits: 8; stop_bits: 1; parity: "N"
```

### 显示 & 输出 (vision_demo)

```yaml
display:
  enable: true; window_name: "YOLO26n Vision"

output:
  save_video: false; path: "./result.mp4"

benchmark:
  enable: true
```

### 日志

```yaml
log:
  level: "info"           # debug | info | warn | error
  use_color: true
```

三个程序共用同一套 `CameraConfig`。`vision_system` 忽略 display/output/benchmark 节，`vision_demo` 忽略 serial 节。

---

## 摄像头参数查询

使用 `v4l2-ctl` 查看当前摄像头支持的格式和参数:

```bash
# 列出所有视频设备
v4l2-ctl --list-devices

# 查看全部参数 (当前格式、控制项等)
v4l2-ctl -d /dev/video0 --all

# 列出所有支持的分辨率/帧率组合
v4l2-ctl -d /dev/video0 --list-formats-ext

# 查看所有 controls 菜单
v4l2-ctl -d /dev/video0 --list-ctrls-menus
```

当前硬件: USB Camera (uvcvideo), `/dev/video0`, S/N: 200901010001

| 格式 | 最高分辨率@帧率 |
|------|----------------|
| MJPG | 1280×720 @ 60fps (全分辨率可达 60fps) |
| YUYV | 1280×720 @ 10fps / 640×480 @ 30fps |

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
│   └── camera.h/cpp                # V4L2 采集封装: CameraConfig + Camera
│                                   #   YAML驱动初始化, V4L2 ioctl白平衡/曝光
│                                   #   参数验证+ANSI表格输出, EMA FPS
├── yolo/
│   └── yolo26.h/cpp                # NCNN 推理 (3种输出格式, NMS, FP16/INT8)
├── color/
│   └── hsv.h/cpp                   # HSV 颜色分类 (双阈值, 形态学, 颜色置信度)
├── serial/
│   └── serial.h/cpp                # POSIX termios 串口 (零依赖)
├── protocol/
│   └── protocol.h                  # 12字节二进制协议 + CRC8
├── config/
│   ├── config.h/cpp                # YAML 配置管理 (摄像头段严格解析)
│   └── vision.yaml                 # 统一配置文件 (所有模块共享)
├── roi/
│   └── roi.h/cpp                   # ROI 裁剪 + 坐标映射
├── logger/
│   └── logger.h/cpp                # ANSI 彩色日志 (4级: debug/info/warn/error)
├── src/input/
│   ├── frame_source.hpp            # FrameSource 抽象接口
│   ├── camera_source.hpp           # CameraSource — 封装 Camera 类为 FrameSource
│   ├── video_source.hpp            # VideoSource — 视频文件 (支持循环)
│   └── frame_source_factory.hpp/cpp # 工厂: YAML → unique_ptr<FrameSource>
├── src/vision_demo.cpp             # Demo 程序 (显示/录像/Benchmark)
├── model/
│   ├── fp16/.gitkeep
│   └── int8/.gitkeep
├── tools/dataset_capture/          ⭐ v1.5 重构
│   ├── CMakeLists.txt              # 独立构建, 链接 camera/ + logger/
│   ├── main.cpp                    # 入口
│   ├── config.h/cpp                # 采集+显示配置 (摄像头共用 CameraConfig)
│   ├── dataset_capture.h/cpp       # 采集类: 使用 Camera 类, Logger 日志
│   └── vision.yaml                 # 默认配置 (与主配置摄像头段一致)
└── README.md
```

---

## 设计决策

### CameraConfig — 单一真实数据源

`CameraConfig` 是唯一摄像头配置结构体, 无任何 C++ 默认值。`vision_system`、`vision_demo` 和 `dataset_capture` 三个程序共用同一结构体和同一套 YAML 格式。YAML 缺失字段直接 `exit(1)`, 杜绝静默降级。

### V4L2 直接 ioctl × 纯 OpenCV

混合方案: 分辨率/FPS/像素格式/曝光 通过 OpenCV `cv::CAP_PROP_*` 设置 (已有成熟映射); 白平衡通过 V4L2 `VIDIOC_S_CTRL` ioctl 直接控制 (OpenCV 未封装 `CAP_PROP_AUTO_WB`)。

### 严格 YAML 解析 × 默认值回退

摄像头参数采用严格解析: `CameraConfig::fromYaml()` 检查每个字段是否存在, 缺失即 `exit(1)` 并打印明确错误信息。其他模块 (YOLO/HSV/串口) 保持 `IsDefined()` 默认值回退。

### 参数验证 × 静默设置

设置完成后通过 `cap_.get()` 和 `VIDIOC_G_CTRL` 读取实际值, 与 YAML 请求值逐项比对, 输出 ANSI 表格。不一致时标 `WARN` 而非静默忽略, 帮助排查驱动兼容性问题。

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

## 任务完成度

### Camera 模块

| 功能 | 状态 | 说明 |
|------|:--:|------|
| V4L2 视频采集 | ✅ | OpenCV CAP_V4L2, MJPG/YUYV |
| 分辨率/帧率配置 | ✅ | YAML 驱动, 无默认值 |
| 像素格式 (FourCC) | ✅ | MJPG (低CPU) / YUYV (未压缩) |
| 缓冲区大小 | ✅ | V4L2 buffer_size, 1=最低延迟 |
| 自动/手动曝光 | ✅ | CAP_PROP_AUTO_EXPOSURE + CAP_PROP_EXPOSURE |
| 自动/手动白平衡 | ✅ | V4L2 ioctl VIDIOC_S_CTRL |
| 参数验证表格 | ✅ | 读取实际值 + ANSI 表格 + WARN 不匹配 |
| 严格 YAML 解析 | ✅ | CameraConfig::fromYaml(), 缺失字段 exit(1) |
| EMA 平滑 FPS | ✅ | 指数移动平均, 0.1 平滑因子 |
| 多后端支持 | ✅ | V4L2 / ANY / DSHOW |

### NCNN 推理

| 功能 | 状态 | 说明 |
|------|:--:|------|
| NCNN 引擎 | ✅ | CPU + NEON + OpenMP |
| FP16 半精度 | ✅ | use_fp16_packed + use_fp16_storage |
| INT8 量化推理 | ✅ | use_int8_inference, FPS 提升 ~60% |
| Packing Layout | ✅ | 优化内存布局 |
| Light Mode | ✅ | 立即释放中间 blob |
| 多线程 | ✅ | num_threads (YAML 可配, 默认4) |
| YOLO26n 模型 | ✅ | C2PSA 注意力, 7.8G FLOPs |
| NMS 后处理 | ✅ | 按类别 IoU 抑制 |
| 3 种输出格式 | ✅ | (N,6) / (1,nc+4,N) / (1,nc+4,H,W) |
| 推理计时 (EMA) | ✅ | avgInferenceMs |
| ncnn::Net move 修复 | ✅ | unique_ptr 包装, vision_system 编译通过 |

### HSV 颜色识别

| 功能 | 状态 | 说明 |
|------|:--:|------|
| 多颜色支持 | ✅ | 红/蓝/黄, YAML 配置 HSV 范围 |
| 双阈值红色 | ✅ | red1 (0-10°) + red2 (170-180°) |
| 形态学降噪 | ✅ | 高斯模糊 + 开运算 |
| 颜色置信度 | ✅ | 像素比 × 饱和度权重 |
| 形态学/模糊核大小 | ✅ | YAML 可配 |

### 输入源

| 功能 | 状态 | 说明 |
|------|:--:|------|
| CameraSource | ✅ | 封装 Camera 类为 FrameSource |
| VideoSource | ✅ | 视频文件, 支持循环播放 |
| FrameSourceFactory | ✅ | YAML → unique_ptr\<FrameSource\> |
| YAML 自动发现 | ✅ | /proc/self/exe 相对搜索 + CWD 回退 |
| 命令行 --config | ✅ | 显式指定路径 |
| 位置参数 | ✅ | ./app path/to/config.yaml |

### ROI 裁剪

| 功能 | 状态 | 说明 |
|------|:--:|------|
| ROI 启用/禁用 | ✅ | YAML 控制 |
| 坐标裁剪 + 映射 | ✅ | 裁剪 + 坐标归一化 |
| 性能提升 | ✅ | 50% 裁剪 → ~40% FPS 提升 |

### 串口通信

| 功能 | 状态 | 说明 |
|------|:--:|------|
| POSIX termios | ✅ | 零依赖 |
| 12 字节二进制协议 | ✅ | 同步头 + CRC8 |
| 流内重同步 | ✅ | 0xAA 0x55 同步头 |
| 波特率/数据位/停止位 | ✅ | YAML 可配 |

### Dataset Capture

| 功能 | 状态 | 说明 |
|------|:--:|------|
| 图片采集 | ✅ | 定时 JPEG 保存 |
| 视频录制 | ✅ | OpenCV VideoWriter, MJPG AVI |
| 图片+视频并行 | ✅ | 共用同一帧, 互不影响 |
| 时间戳视频命名 | ✅ | capture_YYYYMMDD_HHMMSS.avi |
| CameraConfig 共用 | ✅ | 与主程序同一套配置 |
| Logger 日志 | ✅ | 与主程序同一套 ANSI 日志 |
| 快捷键 | ✅ | ESC / SPACE / S / R |
| 增量编号 | ✅ | 扫描已有文件, 不覆盖 |
| OSD 叠加显示 | ✅ | FPS / 保存数 / REC 状态 |

### 配置与日志

| 功能 | 状态 | 说明 |
|------|:--:|------|
| YAML 统一配置 | ✅ | yaml-cpp, 修改无需重编译 |
| 4 级 ANSI 日志 | ✅ | DEBUG/INFO/WARN/ERROR |
| 彩色终端输出 | ✅ | 绿/黄/红/青, 可禁用 |
| CLI 参数覆盖 | ✅ | 命令行 > YAML |

### 构建与部署

| 功能 | 状态 | 说明 |
|------|:--:|------|
| CMake 构建 | ✅ | 3.20+, C++17 |
| vision_system 目标 | ✅ | 生产部署 |
| vision_demo 目标 | ✅ | 离线调试 + Benchmark |
| dataset_capture 目标 | ✅ | 独立构建, 链接 camera/ + logger/ |
| RPi5 ARM64 适配 | ✅ | Cortex-A76 NEON |

### 待完成

| 功能 | 状态 | 说明 |
|------|:--:|------|
| RTSP 输入源 | ⬜ | FrameSource 接口可扩展 |
| Vulkan GPU 加速 | ⬜ | ncnn Vulkan 后端 (RPi5 GPU 驱动待验证) |
| FP16 算术加速 | ⬜ | use_fp16_arithmetic (需测试精度影响) |
| Winograd 卷积 | ⬜ | use_winograd_convolution |
| 视频自动分段 | ⬜ | max_duration / split_duration |
| 多摄像头支持 | ⬜ | 当前仅单路 |
| 远程 Web 监控 | ⬜ | HTTP + MJPEG 流 |
| 单元测试 | ⬜ | Google Test |
| 模型热加载 | ⬜ | 运行时切换 FP16/INT8 |

> **已完成 48 项 / 待完成 9 项 — 完成度 84%**

---

## 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v1.0 | 2026-07-11 | 基础框架: Camera → YOLO → HSV → Serial |
| v1.1 | 2026-07-12 | 7项优化: 颜色置信度/ROI/YAML/FP16+INT8/Logger/多目标/数据增强 |
| v1.2 | 2026-07-12 | 视频推理: FrameSource 抽象/VideoSource/vision_demo/Benchmark |
| v1.3 | 2026-07-12 | YAML 合并: default.yaml + vision.yaml → 单一 vision.yaml |
| v1.4 | 2026-07-13 | Dataset Capture: 原始数据集采集工具 (独立构建, 不影响推理模块) |
| v1.5 | 2026-07-15 | Camera YAML Control: CameraConfig 统一配置, 零默认值, 严格解析, V4L2 ioctl 白平衡, 参数验证+ANSI 表格, dataset_capture 共用 Camera 类+Logger |
| v1.6 | 2026-07-15 | NCNN 修复: ncnn::Net → unique_ptr 解决移动构造编译错误, vision_system/vision_demo 通过编译 |
| v1.7 | 2026-07-15 | Video Recorder: 视频录制功能 (OpenCV VideoWriter, MJPG AVI), 图片+视频并行采集, 时间戳命名, 录制状态 OSD |

---

## ⚠️ 上线前检查清单

| # | 任务 | 位置 | 完成标志 |
|---|------|------|---------|
| 1 | `sudo apt install protobuf-compiler libprotobuf-dev libyaml-cpp-dev` | RPi5 | `dpkg -l \| grep yaml-cpp-dev` 有输出 |
| 2 | 编译 NCNN (CPU + NEON + OpenMP) | RPi5 | `/usr/local/lib/libncnn.a` 存在 |
| 3 | 连接摄像头, `v4l2-ctl --list-devices` 验证 | RPi5 | 能看到 `/dev/video0` |
| 4 | `v4l2-ctl -d /dev/video0 --list-formats-ext` 确认支持格式 | RPi5 | 确认 MJPG/YUYV 参数填入 YAML |
| 5 | 准备 A/B 两类标注数据集 (~500张/类) | 笔记本 | `data.yaml` + images + labels 齐全 |
| 6 | 训练 `yolo detect train model=yolo26n.pt epochs=100 imgsz=320` | 笔记本 | mAP@0.5 > 0.85 |
| 7 | 导出 NCNN: FP16 + INT8 → `model/fp16/` + `model/int8/` | 笔记本 | param + bin 文件就位 |
| 8 | `cmake .. && make -j4 && ./vision_system --model int8` | RPi5 | 启动输出 ANSI Camera 参数表格, 稳定检测 |

---

## 许可证

MIT
