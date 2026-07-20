# 项目全局上下文 — Raspberry Pi 5 视觉识别系统

> 本文档用于在新对话中快速恢复完整项目上下文。包含所有已完成工作、架构决策、当前状态和待办事项。
>
> 生成时间: 2026-07-20 | 版本: v1.6.1 | 代码量: 5,148 行 | 文件数: 43

---

## 一、硬件环境

### 部署端 (当前开发机)
```
设备:   Raspberry Pi 5 Model B Rev 1.1
CPU:    ARM Cortex-A76 × 4 (ARMv8.2-A)
特性:   asimd (NEON), fphp (FP16), asimddp (INT8 dot product)
RAM:    8GB (可用 ~2.2GB)
磁盘:   58GB (可用 ~40GB)
GPU:    VideoCore VII (Mesa Vulkan 25.2.8, 当前未使用)
系统:   Ubuntu 24.04.4 LTS aarch64
摄像头: USB UVC (当前未连接, /dev/video0-37 为虚拟设备)
串口:   /dev/ttyAMA10 (实际物理 UART)
```

### 训练端 (独立设备)
```
设备:   Lenovo R9000P 2024
GPU:    RTX4060 Laptop GPU
系统:   Ubuntu 22.04 x86_64
```

---

## 二、软件环境 (已安装)

| 组件 | 版本 | 位置 |
|------|------|------|
| GCC | 13.3.0 | 系统 |
| G++ | 13.3.0 | 系统 |
| CMake | 3.28.3 | 系统 |
| Git | 2.43.0 | 系统 |
| OpenCV | 4.8.0 | `/usr/local` (源码编译) |
| libprotobuf | 3.21.12 | 系统 (仅运行时库, dev缺失) |
| yaml-cpp | 0.8.0 | 系统 (apt 安装) |
| NCNN | latest | `/usr/local` (源码编译, 静态库 libncnn.a) |
| Vulkan | 1.3.275 | Mesa 25.2.8 (仅库, 未使用) |

### 编译产物 (已验证)

| 可执行文件 | 大小 | 架构 | 动态依赖 |
|-----------|------|------|---------|
| vision_system | ~5MB | ARM aarch64 | libyaml-cpp.so (NCNN 静态链接) |
| vision_demo | ~5MB | ARM aarch64 | libyaml-cpp.so (NCNN 静态链接) |

> NCNN 以 `libncnn.a` 静态库链接进二进制，`ldd` 不显示 NCNN 依赖属正常行为。

---

## 三、项目结构 (43 文件, 5,148 行)

```
Cross-post/                              # Git仓库根目录
│
├── CMakeLists.txt                        # 双target构建 (vision_system + vision_demo)
│
├── main.cpp (378行)                      # 生产流水线: Camera→YOLO→Filter→HSV→Encoder→UART
├── src/vision_demo.cpp (492行)           # 调试Demo: 显示+录像+Benchmark
│
├── camera/camera.h+cpp (359行)          # V4L2 CameraConfig (YAML驱动, ioctl曝光/白平衡/FourCC)
├── yolo/yolo26.h+cpp (591行)            # NCNN推理 (3格式后处理/NMS/FP16+INT8)
├── color/hsv.h+cpp (237行)              # HSV 7色分类器 (红黄蓝绿黑青)
├── serial/serial.h+cpp (245行)          # POSIX termios (零外部依赖, select超时)
├── protocol/protocol.h (123行)          # 9字节二进制协议 (0xCD/0xDC)
│
├── pipeline/detection_filter.h (60行)   # 四级过滤器 (Confidence→Class→Top-K→ROI)
├── pipeline/packet_encoder.h (57行)     # DetectionResult→9字节编码
├── roi/roi.h+cpp (115行)                # ROI裁剪+坐标映射
├── logger/logger.h+cpp (152行)          # ANSI彩色日志 (DEBUG/INFO/WARN/ERROR)
│
├── config/config.h+cpp (338行)          # YAML配置管理 (AppConfig)
├── config/vision.yaml                    # 统一配置文件 (所有参数)
│
├── src/input/                           # FrameSource抽象输入层 (摄像头/视频/RSTP预留)
│   ├── frame_source.hpp                  # 抽象接口
│   ├── camera_source.hpp                 # CameraSource (基于camera.h)
│   ├── video_source.hpp                  # VideoSource (cv::VideoCapture, 支持loop)
│   └── frame_source_factory.hpp+cpp      # YAML→unique_ptr<FrameSource>
│
├── model/                               # 模型文件
│   ├── fp16/yolo26n.ncnn.{param,bin}     # FP16模型 (已就位, 3MB)
│   └── int8/yolo26n.ncnn.{param,bin}     # INT8模型 (已就位, 1.7MB)
│
├── tools/dataset_capture/               # 独立数据集采集工具
│   ├── CMakeLists.txt                    # 独立构建
│   ├── main.cpp (139行)                  # 入口 (auto-search YAML)
│   ├── config.h+cpp                       # 轻量配置
│   ├── dataset_capture.h+cpp (363行)     # 采集类 (图片+视频/Camera+Logger)
│   └── vision.yaml                        # 默认配置
│
├── data/videos/demo.avi                 # 测试视频
└── README.md + PROJECT_CONTEXT.md       # 文档
```

### 各模块代码量

| 模块 | 行数 | 复杂度 | 状态 |
|------|------|--------|------|
| vision_demo.cpp | 492 | 中 | 完成 |
| yolo/ | 591 | 高 (NCNN后处理3格式) | 完成 |
| main.cpp | 378 | 高 (多线程+Filter+三目标分支) | 完成 |
| dataset_capture/ | 502 | 中 (图片+视频双采集) | 完成 |
| camera/ | 359 | 中 (V4L2 ioctl) | 完成 |
| config/ | 338 | 低 (YAML) | 完成 |
| serial/ | 245 | 低 (POSIX termios) | 完成 |
| color/ | 237 | 低 (HSV) | 完成 |
| logger/ | 152 | 低 (ANSI) | 完成 |
| protocol/ | 123 | 低 (协议定义) | 完成 |
| src/input/ | 163 | 中 (工厂模式) | 完成 |
| pipeline/ | 117 | 低 (纯头文件) | 完成 |
| roi/ | 115 | 低 | 完成 |

---

## 四、架构决策记录

### ADR-1: NCNN CPU-Only (禁止Vulkan)
- 原因: ARM CPU+NEON+OpenMP 成熟稳定, 驱动依赖少, 便于调试
- 编译: `-DNCNN_VULKAN=OFF -DNCNN_OPENMP=ON`
- 代码: `net_.opt.use_vulkan_compute = false` 硬编码

### ADR-2: FP16 + INT8 双模型, 启动时选择
- 目录: `model/fp16/` + `model/int8/`
- CLI: `--model fp16` (调试) / `--model int8` (生产)
- 运行时不可切换 (需重启进程)

### ADR-3: YAML 统一配置, 禁止硬编码
- 所有参数由 `config/vision.yaml` 控制
- 优先级: CLI参数 > YAML > 代码默认值
- 修改无需重编译

### ADR-4: YOLO+HSV 分离
- YOLO 仅检测目标位置和类别
- HSV 仅对 Material 和 ColorRing 执行颜色分类
- Obstacle 跳过 HSV (固定黑色)
- 原因: 更少YOLO类别→更快推理, HSV确定性判定→无须训练

### ADR-5: 9字节二进制协议 (v1.5)
- 0xCD/0xDC 帧边界, 固定长度, 无CRC
- STM32 状态机解析: 等待0xCD→读Length→读数据→读0xDC
- 每目标独立发送, 非批量

### ADR-6: DetectionResult 唯一数据交换结构
- 禁止模块间传递自定义结构体
- 整个系统只使用 `DetectionResult` 和 `Object` 两种数据结构

### ADR-7: 工厂模式替代 if-else
- `FrameSourceFactory::createFromYaml()` 创建输入源
- 新增输入源只需实现 FrameSource 接口

### ADR-8: Detection Filter 固定流水线
- Confidence→Class→Top-K→ROI 四级过滤
- 不满足阈值直接丢弃, 后续步骤不执行

---

## 五、关键数据结构

### DetectionResult (最终输出)
```cpp
struct DetectionResult {
    protocol::TargetType target;   // Material=1, ColorRing=2, Obstacle=3
    protocol::ColorType  color;    // NONE=0, RED=1, YELLOW=2, BLUE=3, GREEN=4, BLACK=5, CYAN=6
    float  confidence;             // YOLO 检测置信度
    float  colorConfidence;        // HSV 颜色置信度 (Obstacle=0)
    int    cx, cy;                 // 中心像素坐标
};
```

### 协议格式 (9字节)
```
[0xCD] [0x06] [TargetType] [Color] [CX_h] [CX_l] [CY_h] [CY_l] [0xDC]
```

### 三目标 HSV 规则
| YOLO label | TargetType | HSV | 有效颜色 |
|-----------|------------|-----|---------|
| 0 | Material | 全6色 | RED/YELLOW/BLUE/GREEN/BLACK/CYAN |
| 1 | ColorRing | 仅3色 | RED/BLUE/GREEN |
| 2 | Obstacle | 跳过 | NONE (固定0) |

---

## 六、流水线执行流程

```
1. main.cpp 启动
2. 加载 config/vision.yaml → AppConfig
3. 初始化 Dependencies:
   ├── YOLO26Detector (NCNN, FP16或INT8)
   ├── DetectionFilter (Confidence≥0.45, Top-K≤10)
   ├── HSVClassifier (7色)
   ├── Camera (V4L2, MJPG 640×480@60fps)
   └── SerialPort (/dev/ttyAMA0, 115200)
4. 启动 3 个线程:
   ├── CameraThread → frameQueue(2)
   ├── DetectionThread:
   │   ├── frameQueue.pop()
   │   ├── ROI.apply() (可选)
   │   ├── yolo.detect() → vector<Object>
   │   ├── filter.apply() → 过滤后 vector<Object>
   │   ├── for each: 三目标HSV分支 → DetectionResult
   │   └── resultQueue.push(vector<DetectionResult>)
   └── SerialThread:
       ├── resultQueue.pop()
       ├── PacketEncoder::encode() → 9*N字节
       └── serial.write(frame)
5. main thread: 每秒状态打印, Ctrl+C 优雅退出
```

---

## 七、已完成功能清单 (✅)

- [x] C++17 CMake 工程, 双 target (vision_system + vision_demo)
- [x] YOLO26n NCNN 推理 (3种输出格式后处理, NMS, FP16/INT8)
- [x] HSV 7色识别 (红黄蓝绿黑青, 颜色置信度)
- [x] 9字节二进制协议 (0xCD/0xDC, TargetType/ColorType)
- [x] DetectionFilter 四级过滤 (Confidence/Class/Top-K/ROI)
- [x] PacketEncoder 统一编码
- [x] CameraConfig YAML驱动 (V4L2 ioctl曝光/白平衡/像素格式)
- [x] POSIX termios 串口 (零外部依赖)
- [x] ANSI 4级彩色日志 (INFO/WARN/ERROR/DEBUG)
- [x] YAML 配置管理 (yaml-cpp, CLI覆盖, 无硬编码)
- [x] ROI 裁剪 + 坐标映射
- [x] FrameSource 抽象 (CameraSource/VideoSource, 工厂模式)
- [x] Dataset Capture 独立工具 (图片+视频, 复用Camera+Logger)
- [x] FP16 + INT8 双模型 (启动时切换, 运行时不可变)
- [x] 多线程 SafeQueue (背压, 优雅关闭)
- [x] NCNN 训练指南 (数据增强策略, 导出流程)
- [x] ARM64 编译验证通过 (全部3个可执行文件)
- [x] NCNN 源码编译安装 (ARM aarch64, CPU+NEON+OpenMP)
- [x] 真实编译通过 — vision_system + vision_demo 均已生成 (~5MB, ARM aarch64)
- [x] 完整文档 (README.md + PROJECT_CONTEXT.md)

## 八、待完成任务 (❌/⚠️)

### 阻塞项 (需要 sudo)
- [x] ~~`sudo apt install protobuf-compiler libprotobuf-dev libyaml-cpp-dev`~~ ✅ 已完成
- [x] ~~编译安装 NCNN~~ ✅ 已完成 (ARM aarch64, CPU+NEON+OpenMP)
- [x] ~~真实编译~~ ✅ 已完成 — vision_system + vision_demo 均已生成 (~5MB)

### 需要物理硬件
- [ ] 连接 USB 摄像头 + 验证 `v4l2-ctl --list-devices`
- [ ] 连接 UART 串口 + 环回测试协议

### 需要训练端
- [ ] 在 RTX4060 笔记本上采集+标注 A/B/Obstacle 三类数据集
- [ ] 训练 YOLO26n 3类模型: `yolo detect train model=yolo26n.pt epochs=100 imgsz=320`
- [ ] 导出 FP16+INT8: `yolo export format=ncnn` → `model/`

### 需要实地标定
- [ ] 根据实际光照调整 `vision.yaml` color段 HSV 范围
- [ ] 验证7色识别准确率
- [ ] 根据实际场景调整 DetectionFilter 阈值

### 功能扩展 (低优先级)
- [ ] RTSP 网络流输入 (FrameSource 接口已预留)
- [ ] 多线程保存图片 (dataset_capture 已预留)
- [ ] STM32 上行数据接收 (`serial.read()` 已实现)
- [ ] 目标跟踪 (trackId 扩展接口已预留)
- [ ] ROS2 节点
- [ ] WebUI 远程监控

---

## 九、新对话恢复指南

### 第一步: 阅读文档
```bash
cat README.md              # 用户手册
cat PROJECT_CONTEXT.md     # 本文档 (开发上下文)
```

### 第二步: 确认环境
```bash
g++ --version                     # 应为 13.3.0
cmake --version                   # 应为 3.28+
pkg-config --modversion opencv4   # 应为 4.8.0
ls /usr/local/lib/libncnn.a       # NCNN 静态库 (已安装)
ls /usr/local/include/ncnn/net.h  # NCNN 头文件 (已安装)
dpkg -l | grep yaml-cpp           # libyaml-cpp-dev (已安装)
```

### 第三步: 编译项目
```bash
cd /home/shu/Cross-post
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && make -j4
# 产物: vision_system (~5MB) + vision_demo (~5MB), ARM aarch64
# ldd 仅显示 libyaml-cpp.so — NCNN 以 libncnn.a 静态链接, 属正常行为
```

### 第四步: 运行 Demo (无需硬件)
```bash
# 视频推理 (使用 data/videos/demo.avi)
./vision_demo --config ../config/vision.yaml

# 数据集采集 (需要USB摄像头)
cd ../tools/dataset_capture && mkdir build && cd build
cmake .. && make -j4 && ./dataset_capture
```

### 第五步: 修改参数 (无需重编译)
```bash
vim ../config/vision.yaml  # 修改任意参数后直接重新运行
```

### 关键调试命令
```bash
# 查看所有摄像头
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --all

# 查看模型文件
ls -lh model/fp16/ model/int8/

# 查看串口
ls /dev/tty* | grep -E "AMA|USB|S0"

# 测试串口环回 (TX→RX)
# 用杜邦线短接 TX/RX 引脚, 然后:
python3 -c "import serial; s=serial.Serial('/dev/ttyAMA0',115200); s.write(b'\xCD\x06\x01\x03\x01\x40\x00\xF0\xDC'); print(s.read(9))"
```

---

## 十、历史版本演进

| v1.0 | 07-11 | 基础: Camera→YOLO→HSV(3色)→Serial(12B+CRC) |
| v1.1 | 07-12 | 优化: 颜色置信度/ROI/YAML/FP16+INT8/Logger/多目标/数据增强 |
| v1.2 | 07-12 | 视频: FrameSource抽象/VideoSource/vision_demo/Benchmark |
| v1.3 | 07-12 | 合并: default.yaml+vision.yaml→单一vision.yaml |
| v1.4 | 07-13 | 采集: Dataset Capture (图片+视频, 复用Camera+Logger) |
| v1.5 | 07-16 | 协议: 9字节0xCD/0xDC, DetectionFilter, PacketEncoder, 7色HSV |
| v1.6 | 07-18 | 部署: NCNN 源码编译安装, yaml-cpp 安装, 真实编译通过 (ARM aarch64) |
| v1.6.1 | 07-20 | 修复: YAML bool as<int>()→as<bool>() 解析崩溃, 模型文件名 model→yolo26n, 默认FP16 |

---

## 十一、NCNN 模型转换排障全记录

> 2026-07-18 实战总结。在树莓派5上部署 YOLO26n NCNN 模型的完整排障过程。
> **最终方案**: 使用 `ultralytics.export(format='ncnn')` 生成无后处理的纯推理模型。

### 遇到的错误序列及解决

| # | 错误 | 根因 | 解决 |
|---|------|------|------|
| 1 | `fopen ... failed (错误码=-1)` | param 文件名 `model.ncnn.param` vs 实际 `yolo26n.ncnn.param` | 代码改为 `yolo26n.ncnn.param` |
| 2 | `param is too old` | magic 7767517 > NCNN 编译版本 20260712 | `git pull origin master && make -j4 && sudo make install` |
| 3 | `layer ReduceMax not exists` | pnnx 导出包含合并的后处理层，NCNN 不支持 | 改 param: `ReduceMax` → `Reduction 0=4` (临时方案) |
| 4 | `layer torch.topk not exists` | 同上，TopK 后处理层不应在推理图中 | **根本解决**: 用 ultralytics 官方 export |

### 最终方案: Ultralytics 官方导出

```bash
# 训练端 (x86_64)
conda activate yolo26
pip install ultralytics[export]

python -c "
from ultralytics import YOLO
model = YOLO('yolo26n.pt')
model.export(format='ncnn', imgsz=320)
"

# 验证无后处理层
grep -i topk yolo26n_ncnn_model/*.param  # 应无输出

# 重命名 + 传输
cd yolo26n_ncnn_model
mv yolo26n_ncnn_model.param yolo26n.ncnn.param
mv yolo26n_ncnn_model.bin   yolo26n.ncnn.bin
scp yolo26n.ncnn.* shu@rpi5:/home/shu/Cross-post/model/fp16/
```

### 核心原则

- **NCNN 不支持后处理层 (TopK/NMS/ReduceMax 等)**，这些应在 C++ 应用层完成
- **始终用 `model.export(format='ncnn')`** 而非手动 pnnx/onnx2ncnn
- **NCNN 版本 >= pnnx 导出版本**，否则 magic 不匹配
- **树莓派5 NEON 验证**: `cat /proc/cpuinfo | grep asimd` 应有输出

### 成功输出

```
[YOLO] 加载模型: ../model/fp16/yolo26n.ncnn.param
[YOLO]   类型=fp16 线程数=4 后端=CPU+NEON+OpenMP
[YOLO] 模型加载成功。
[YOLO]   输入尺寸: 320x320
```

### NCNN 编译 (RPi5 本地或交叉编译)

```bash
# 克隆最新版
git clone https://github.com/Tencent/ncnn.git
cd ncnn && mkdir -p build && cd build

# RPi5 本地编译 (ARM64, CPU+NEON+OpenMP, 无 Vulkan)
cmake -DCMAKE_BUILD_TYPE=Release \
      -DNCNN_VULKAN=OFF \
      -DNCNN_OPENMP=ON \
      -DNCNN_BUILD_TOOLS=ON \
      -DNCNN_BUILD_EXAMPLES=OFF \
      -DNCNN_BUILD_BENCHMARK=OFF \
      ..

# x86_64 交叉编译 ARM64 (含 Vulkan)
cmake -DCMAKE_BUILD_TYPE=Release \
      -DNCNN_VULKAN=ON \
      -DNCNN_BUILD_TOOLS=ON \
      -DNCNN_BUILD_EXAMPLES=ON \
      -DCMAKE_TOOLCHAIN_FILE=../toolchains/aarch64-linux-gnu.toolchain.cmake \
      ..

make -j4
sudo make install && sudo ldconfig
```

> 本地编译用第一组参数 (CPU+NEON)，交叉编译用第二组 (含 Vulkan + toolchain)。项目当前仅使用 CPU 后端。

---

## 十二、YAML 解析注意事项

> 2026-07-20 实战总结。yaml-cpp 0.8.0 的类型转换规则。

### as\<bool\>() vs as\<int\>()

- **`as<bool>()`** 可以从 YAML `true`/`false` 正确转换 ✅
- **`as<int>()`** 无法从 YAML `true`/`false` 转换，抛出 `TypedBadConversion<int>` ❌

### 代码规范

- YAML 布尔字段统一写 `true`/`false`（可读性）
- C++ 代码统一用 `as<bool>()` 读取（兼容性）
- 禁止用 `as<int>()` 读取 YAML 布尔值

### 已知 YAML 布尔字段

| YAML 路径 | C++ 类型 | 读取方式 |
|-----------|----------|---------|
| `camera.auto_exposure` | `bool` | `as<bool>()` |
| `camera.auto_white_balance` | `bool` | `as<bool>()` |
| `video.loop` | `bool` | `as<bool>()` |
| `roi.enabled` | `bool` | `as<bool>()` |
| `display.enable` | `bool` | `as<bool>()` |
| `output.save_video` | `bool` | `as<bool>()` |
| `benchmark.enable` | `bool` | `as<bool>()` |
| `log.use_color` | `bool` | `as<bool>()` |

---

*本文档在新对话开始时阅读, 可在 5 分钟内恢复完整项目上下文。*
