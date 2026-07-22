# RaspberryPi5 视觉系统

基于 Raspberry Pi 5 的嵌入式实时视觉系统。

**可执行程序：** `visionpost`

---

## 硬件需求

| 组件       | 规格                                    |
|------------|----------------------------------------|
| 主控       | Raspberry Pi 5 Model B（ARM Cortex-A76 ×4） |
| 系统       | Ubuntu 24.04.4 LTS（aarch64）           |
| 摄像头     | USB UVC，MJPG 640×480 @ 60 FPS         |
| 串口       | `/dev/ttyUSB0`（可配置）               |

---

## 软件依赖

| 依赖        | 版本   |
|-------------|--------|
| GCC         | 13.3+  |
| CMake       | 3.28+  |
| OpenCV      | 4.8+   |
| yaml-cpp    | 0.8+   |

Ubuntu 安装命令：

```bash
sudo apt install build-essential cmake libopencv-dev libyaml-cpp-dev
```

---

## 编译

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
```

可执行文件 `visionpost` 生成在 `build/` 目录下。
配置文件始终读取源码树中的 `config/vision.yaml`（不复制到 build）。

---

## 运行

```bash
# 默认配置（build/ 下运行时自动读取 ../config/vision.yaml）：
./build/visionpost

# 自定义配置文件：
./build/visionpost /path/to/custom.yaml

# 使用便捷脚本：
./scripts/start.sh
```

按 `Ctrl+C`（SIGINT）停止，或使用：

```bash
./scripts/stop.sh
```

---

## 配置文件（`config/vision.yaml`）

所有参数均来自 YAML 文件。**代码中没有任何硬编码默认值** ——
缺少必需的键时程序会立即退出并打印错误信息。

### 主要配置节

| 配置节       | 说明                                                   |
|-------------|--------------------------------------------------------|
| `camera`    | 设备索引、视频文件回退、分辨率、帧率                       |
| `ui`        | 窗口开关、叠加层开关、窗口标题、`show_binary` 二值化调试窗 |
| `serial`    | 串口路径、波特率、`send_interval_ms` 发送间隔            |
| `kalman`    | 过程/测量噪声、初始化帧数、超时帧数                        |
| `recording` | 原始视频录制（无叠加层）                                  |
| `roi`       | 感兴趣区域（x, y, w, h）                                |
| `mode0`     | 6 色 HSV 范围（红色有双区间）                            |
| `mode1`     | 轮廓法圆环检测：自适应阈值 + findContours + 三重几何校验   |
| `mode2`     | 黑色 HSV 范围、长宽比/面积阈值                           |
| `logging`   | 日志级别、文件输出                                       |

### 视频文件测试

将 `camera.video_path` 设置为 `.mp4` 文件路径即可用视频代替摄像头：

```yaml
camera:
  video_path: "/path/to/test.mp4"
```

---

## 视觉模式

由远端系统通过串口命令控制：

| 命令     | 模式  | 说明                                   |
|----------|------|----------------------------------------|
| `MODE0`  | 0    | 6 颜色物块检测 + Kalman                 |
| `MODE1`  | 1    | 圆环检测（轮廓法）                       |
| `MODE2`  | 2    | 黑色圆柱检测                            |
| `MODE3`  | 3    | 空闲（仅采集，不检测）                    |

### 模式0 —— 颜色物块

检测 6 种颜色：**红、黄、蓝、绿、黑、青**。

- 红色使用双 HSV 区间：H 0–10 ∪ H 170–180
- 每种颜色独立跟踪，各有独立的 Kalman 滤波器
- Kalman：2-D `[x, y]`，恒等转移矩阵，**无速度模型**
- 二值化调试窗：各颜色掩码合成图（各颜色以对应 BGR 色彩渲染）

### 模式1 —— 圆环检测（轮廓法 + 三重几何校验）

- 灰度化 → 高斯模糊 → 自适应阈值二值化 → `findContours(RETR_EXTERNAL)`
- `RETR_EXTERNAL` 只取最外层轮廓，自动忽略内部数字"1、2、3"和内圈
- 按面积取前 3 大轮廓，`moments` 质心 + `minEnclosingCircle` 半径
- **三重校验（YAML 可独立开关）：**
  1. 数量校验：有效轮廓 ≥ 3
  2. 共线性校验：`max_y − min_y ≤ collinearity_tolerance_px`
  3. 半径一致性校验：`max_r − min_r ≤ radius_tolerance_px`
- **任意校验失败 → 丢弃整帧**（不更新 Kalman、不发送）
- 通过后按 X 坐标排序：左 = 1，中 = 2，右 = 3
- 性能远超 HoughCircles（树莓派5 可达 50+ FPS）
- 二值化调试窗：自适应阈值二值图

### 模式2 —— 黑色圆柱

- 黑色 HSV 掩码 → 形态学闭运算 → 轮廓 → `minAreaRect`
- 过滤条件：`长宽比 > 阈值` **且** `面积 > 阈值`
- 二值化调试窗：形态学处理后的黑色掩码

### 模式3 —— 空闲

摄像头继续采集，但不执行检测、不发送数据。

---

## 发送节流机制

视觉算法（含 Kalman）始终 60fps 全速运行。串口推送受 `serial.send_interval_ms` 控制：

- **限频发送**：距上次发送时间 < `send_interval_ms` 时跳过，等待下一帧
- **模式切换抢占**：模式变化时无视计时器，立即发送当前帧结果
- **目标出现优先**：无有效目标时不更新计时器，目标重新出现后第一帧立即发出
- **连续多包间隔**：同一帧内连续发送多个包之间插入 100µs 保护间隙，防止粘包

---

## 串口协议

### 接收（4 字节）—— 模式命令

```
0xCD  0x01  MODE  0xDC
```

`MODE` 为一个字节：`0x00` – `0x03`。

### 发送（9 字节）—— 检测结果

```
0xCD  0x06  TargetType  Color  X_H  X_L  Y_H  Y_L  0xDC
```

| 字节 | 字段         | 说明                                                |
|------|-------------|-----------------------------------------------------|
| 0    | `0xCD`      | 帧头                                                |
| 1    | `0x06`      | 数据帧标记                                           |
| 2    | `TargetType`| 1 = 颜色物块，2 = 圆环，3 = 圆柱                     |
| 3    | `Color`     | 模式0：颜色索引（0=红,1=黄,2=蓝,3=绿,4=黑,5=青）<br>模式1：圆编号（1–3）<br>模式2：未使用 |
| 4–5  | `X`         | uint16_t，大端序                                     |
| 6–7  | `Y`         | uint16_t，大端序                                     |
| 8    | `0xDC`      | 帧尾                                                |

---

## 系统架构

```
CameraThread ──(单槽缓冲)──> ProcessThread ──(发送队列)──> SerialThread
      │                         │    │                          │
      └── 录制原始视频          │    └── 限频门控               /dev/ttyUSB0
                              模式命令（atomic）──────────────────┘
```

- **CameraThread：**采集帧，仅保留最新帧，录制原始视频（无 UI 叠加）
- **ProcessThread：**全速运行视觉管线 + Kalman，经限频门控推入发送队列，绘制 UI
- **SerialThread：**接收模式命令，排空发送队列并写入 UART（非阻塞 poll，包间 100µs 间隔）

### 安全保证

- 仅 SerialThread 接触 UART 文件描述符
- ProcessThread **绝不**直接调用 `SerialPort::write()` —— 结果通过 `ThreadSafeSendQueue` 流转
- 每次模式切换时 Kalman 滤波器全部重置
- 摄像头故障触发最多 5 次重连，之后程序退出
- 串口发送 DEBUG 模式可输出完整 HEX 包日志

---

## 目录结构

```
├── CMakeLists.txt              # CMake 构建配置
├── README.md                   # 本文件
├── .gitignore
├── 核心记忆.md                 # 项目记忆文件
├── config/
│   └── vision.yaml             # 视觉系统配置文件
├── include/
│   ├── ConfigManager.h         # YAML 配置管理（单例）
│   ├── CameraCapture.h         # USB 摄像头封装
│   ├── SerialPort.h            # 串口通信（非阻塞）
│   └── VisionPipeline.h        # 视觉管线 + Kalman 滤波器
├── src/
│   ├── main.cpp                # 程序入口 + 线程编排 + 限频门控
│   ├── ConfigManager.cpp
│   ├── CameraCapture.cpp
│   ├── SerialPort.cpp          # 含 HEX 调试日志
│   └── VisionPipeline.cpp      # 模式0（HSV+轮廓）模式1（轮廓法）模式2（minAreaRect）
├── tools/
│   ├── Logger.h                # 彩色日志宏
│   ├── FPSCounter.h            # 帧率计数器
│   ├── ThreadSafeFrameBuffer.h # 线程安全单槽帧缓冲
│   ├── ThreadSafeSendQueue.h   # 线程安全发送队列 + DetectionResult
│   └── ColorOrder.h            # 颜色定义与顺序
└── scripts/
    ├── start.sh
    ├── stop.sh
    └── restart.sh
```
