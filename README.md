# RaspberryPi5 视觉系统

基于 Raspberry Pi 5 的嵌入式实时视觉系统，用于竞赛环境下的目标检测与跟踪。

**可执行程序：** `visionpost`

---

## 硬件环境

| 组件   | 规格                                         |
|--------|----------------------------------------------|
| 主控   | Raspberry Pi 5 Model B（ARM Cortex-A76 ×4）  |
| 系统   | Ubuntu 24.04.4 LTS（aarch64）                |
| 摄像头 | USB UVC，MJPG 640×480 @ 60 FPS               |
| 串口   | `/dev/ttyUSB0`（YAML 可配置）                 |

---

## 软件依赖

| 依赖      | 版本   | 用途                          |
|-----------|--------|-------------------------------|
| GCC       | 13.3+  | C++17 编译，ARM NEON 自动向量化 |
| CMake     | 3.28+  | 构建系统                       |
| OpenCV    | 4.8+   | 传统视觉算法（无深度学习）       |
| yaml-cpp  | 0.8+   | YAML 配置文件解析               |

安装命令：

```bash
sudo apt install build-essential cmake libopencv-dev libyaml-cpp-dev
```

---

## 编译与运行

```bash
# 编译（Release 模式，O3 优化）
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4

# 运行（默认读取 ../config/vision.yaml）
./visionpost

# 自定义配置
./visionpost /path/to/custom.yaml

# 看门狗脚本（整合自启动，自动设置串口权限）
../scripts/autostart.sh              # = start：前台监管，崩溃自动拉起，Ctrl+C 整体退出
../scripts/autostart.sh stop         # 优雅停止
../scripts/autostart.sh status       # 查看运行状态
```

按 `Ctrl+C` 发送 SIGINT 优雅退出。

## 开机自启动（看门狗）

在桌面「启动应用程序」添加启动项，命令粘贴（会自动弹出终端实时显示看门狗日志）：

```bash
gnome-terminal --title="Visionpost 看门狗" -- bash /home/shu/Cross-post/scripts/autostart.sh
```

`autostart.sh` 同时承担启动/看门狗/停止/状态查询（`start|stop|restart|status`）：
visionpost 崩溃后自动重启，连续快速崩溃超过 `MAX_RESTARTS` 次（默认 10）则放弃并保持终端提示；
串口权限通过 `echo <密码> | sudo -S chmod 666 /dev/ttyACM0` 自动放权（密码在脚本头部 `SUDO_PASS` 修改）。


---

## 系统架构

```
CameraThread ──(单槽帧缓冲)──> ProcessThread ──(发送队列)──> SerialThread
      │                         │    │                          │
      └── 录制原始视频          │    └── 限频门控               /dev/ttyUSB0
                              模式/目标命令（atomic）─────────────┘
```

### 三线程模型

| 线程            | 职责                                                              |
|-----------------|-------------------------------------------------------------------|
| **CameraThread** | 采集帧 → 单槽缓冲（仅保留最新帧，丢弃旧帧）→ 录制原始视频（无 UI） |
| **ProcessThread**| 获取最新帧 → 视觉算法 + Kalman（全速 60fps）→ 限频门控 → 推入发送队列 → 绘制 UI |
| **SerialThread** | poll 接收模式命令 → 排空发送队列 → 非阻塞写入 UART（包间 100µs 间隔） |

### 线程安全约束

| 资源           | 生产者          | 消费者          | 机制                                    |
|----------------|-----------------|-----------------|-----------------------------------------|
| 帧缓冲         | CameraThread    | ProcessThread   | `std::mutex` 单槽覆盖                    |
| 发送队列       | ProcessThread   | SerialThread    | `std::mutex` + `std::condition_variable` |
| 当前模式/目标  | SerialThread    | ProcessThread   | `std::atomic<uint8_t>` × 2               |
| Kalman 映射表  | ProcessThread   | ProcessThread   | 单线程访问（模式切换延迟处理）             |
| UART fd        | SerialThread 独享 | —             | ProcessThread **严禁**直接写串口         |
| 调试图像       | ProcessThread   | ProcessThread   | `std::mutex`                            |

---

## 视觉模式

远端主控通过串口发送 `MODE0`–`MODE3` + `TARGET` 切换。模式或目标变化时 Kalman 滤波器全部重置。

### 模式0 — 颜色物块检测

检测 6 种颜色：**红、黄、蓝、绿、黑、青**（可通过 `mode0.enabled_colors` 临时屏蔽部分颜色）。

- 流水线：ROI → BGR→HSV → 逐色 `inRange` → 形态学开运算 → 轮廓 → 取最大面积 → `moments` 质心 → Kalman
- 红色使用双 HSV 区间：`H 0–10 ∪ H 170–180`（`bitwise_or` 合并）
- 每种颜色独立跟踪，各自持有独立的 Kalman 滤波器
- 某颜色检测失败不影响其他颜色
- **颜色启用过滤**：`enabled_colors` 列表之外的整色跳过（不检测/不发送）；`color_id` 仍为全局 0-5 索引，屏蔽不重编号
- 二值化调试窗：6 色掩码合成图（各色以 BGR 色彩渲染于黑底）

### 模式1 — 多圆环数字识别（同心环剥除 + Hu矩 argmin，TARGET 指定输出）

视野内可**同时出现多个圆环**，算法逐个识别数字编号，只输出串口命令中 `TARGET` 指定的那一个。靶标实物为「**中心衬线数字 1/2/3 + 多层同心细环**」的印章式图案；摄像头固定在机械臂上可能旋转。单环识别原理：

1. 提取圆环外轮廓，得圆心与半径；
2. 中心方形裁剪（半宽 = `digit_roi_scale × 半径`）；
3. 数字区自适应二值化后，用**随半径缩放的开运算核**（`digit_open_ratio × 半径`）把同心细环剥掉、只留粗数字笔画 —— 环线宽度与靶标半径成比例，故开核也随半径缩放，远近通用；
4. 取靠近圆心的最大墨块作为数字轮廓；
5. `cv::matchShapes`（Hu 矩，旋转不变）与 3 个模板对比，**取三模板中最小者（argmin）** 判数字编号 —— 该字体绝对匹配分偏高且不稳，不做绝对阈值。

每帧的圆环处理顺序：

```
面积过滤(min_contour_area) → 按面积降序 → 候选数 > max_simultaneous_rings ?
   ├─ 是 → 判定画面拥挤，整帧丢弃（不输出，Kalman 计一次丢失）
   └─ 否 → 取前 max_rings_per_frame 个 → 逐环识别数字
            → 单个环识别失败只跳过该环
            → 同编号冲突（同编号出现两次）→ 整帧丢弃（Kalman 计一次丢失）
            → 按 TARGET 筛选 → 命中则 Kalman 输出，未命中则不输出
```

- `TARGET=0`：圆环空闲，不检测、不输出（`max_rings_per_frame`/`max_simultaneous_rings` 均不参与）
- 成功 → `target_type=2`，`color_id=TARGET 编号(1/2/3)`，`x/y=滤波后圆心`；Kalman key 用 TARGET 编号
- 只对 TARGET 指定圆环做 Kalman 更新；其他圆环即使检出也不进滤波器
- 二值化调试窗：所有已识别圆环的数字 ROI（剥环后）二值图，统一高度水平拼接；无有效数字时回退显示环级二值图

数字模板（`templates/num1.png` / `num2.png` / `num3.png`）需为**正视角、纯白底黑字**的灰度图，程序首次进入模式1时自动加载一次。

制作模板用配套工具（摄像头实时框选 → 自动二值化 → 保存）：

```bash
python3 tools/capture_templates.py              # 实时画面按 1/2/3 键框选对应数字
python3 tools/capture_templates.py --image pic.jpg   # 从已有图片取图
```

### 模式2 — 黑色圆柱检测

- 流水线：ROI → BGR→HSV → 黑色 `inRange` → 形态学闭运算 → 轮廓 → `minAreaRect`
- 过滤条件：`长宽比 > aspect_ratio_threshold` **且** `面积 > min_area`
- 取最大有效候选 → Kalman 滤波
- 二值化调试窗：形态学处理后的黑色掩码

### 模式3 — 空闲

继续采集，不检测，不发送。

---

## 发送节流机制

视觉算法始终 60fps 全速运行，串口推送受 YAML 控制：

| 触发条件                       | 行为                                    |
|-------------------------------|-----------------------------------------|
| 模式切换（`MODE` 或 `TARGET` 变化）| **立即抢断发送**，重置计时器            |
| 距上次发送 ≥ `send_interval_ms`| 允许发送                                |
| 无有效目标                     | 不发送，**不更新计时器**（目标重现立即发出）  |
| 同一帧连续多包                  | 包间 `sleep(100µs)` 防粘包               |

---

## Kalman 滤波器

- **状态：** 2-D `[x, y]`，**无速度模型**
- **转移矩阵：** `I₂`（恒等）
- **测量矩阵：** `I₂`（恒等）
- **噪声转换：** YAML 标量 → `eye(2,2) * scalar`

```cpp
kf.processNoiseCov      = cv::Mat::eye(2, 2, CV_32F) * yaml_scalar;
kf.measurementNoiseCov  = cv::Mat::eye(2, 2, CV_32F) * yaml_scalar;
```

- 初始化需连续 N 帧稳定检测（`kalman.init_frames`）
- 连续丢失 M 帧后超时复位：`statePost = statePre`（防漂移）
- 模式切换 → 全量清空重建

---

## 串口协议

### 接收 — 模式命令（5 字节）

```
0xCD  0x01  MODE(0-3)  TARGET(0-3)  0xDC
```

| 字节 | 字段     | 值                                                         |
|------|----------|------------------------------------------------------------|
| 0    | `0xCD`   | 帧头                                                        |
| 1    | `0x01`   | 命令类型                                                     |
| 2    | `MODE`   | 0=颜色物块  1=圆环  2=圆柱  3=空闲                             |
| 3    | `TARGET` | 仅 `MODE=1` 有效：0=圆环空闲，1/2/3=指定输出的圆环编号；其他模式忽略 |
| 4    | `0xDC`   | 帧尾                                                        |

校验：第 1 字节为 `0x01`、`MODE ≤ 3`、`TARGET ≤ 3`、帧尾为 `0xDC`，任一不满足则丢弃该帧并继续搜寻 `0xCD`。

### 发送 — 检测结果（9 字节）

```
0xCD  0x06  TargetType  Color  X_H  X_L  Y_H  Y_L  0xDC
```

| 字节 | 字段         | 值                                                        |
|------|-------------|-----------------------------------------------------------|
| 0    | `0xCD`      | 帧头                                                       |
| 1    | `0x06`      | 数据类型                                                   |
| 2    | TargetType  | 1=颜色物块  2=圆环  3=圆柱                                  |
| 3    | Color       | 模式0：颜色索引(0=红,1=黄,2=蓝,3=绿,4=黑,5=青)；模式1：TARGET 编号(1-3) |
| 4-5  | X           | `uint16_t` 大端序                                          |
| 6-7  | Y           | `uint16_t` 大端序                                          |
| 8    | `0xDC`      | 帧尾                                                       |

- 接收端帧同步解析：搜寻 `0xCD` 后读固定长度，无效自动跳过
- Debug 模式输出完整 HEX 日志

---

## 配置文件（`config/vision.yaml`）

**所有参数来自 YAML，无硬编码默认值。缺少键 → `LOG_ERROR` + `exit(EXIT_FAILURE)`。**

### 配置节总览

| 配置节       | 关键参数                                                     |
|-------------|-------------------------------------------------------------|
| `camera`    | `index`, `video_path`, `width`, `height`, `fps`             |
| `ui`        | `imshow_enable`, `ui_draw_enable`, `show_binary`, `window_name` |
| `serial`    | `port`, `baudrate`, `send_interval_ms`                      |
| `kalman`    | `process_noise_cov`, `measurement_noise_cov`, `init_frames`, `lost_frame_limit` |
| `recording` | `record_mode`, `output_dir`, `fourcc`, `fps`                 |
| `roi`       | `x`, `y`, `width`, `height`                                  |
| `mode0`     | `min_contour_area`, `morph_kernel_size`, `enabled_colors`, 6 色 HSV 双区间 |
| `mode1`     | `gaussian_kernel_size`, `adaptive_block_size`, `adaptive_c`, `min_contour_area`, `max_rings_per_frame`, `max_simultaneous_rings`, `morph_open_kernel_size`, `digit_roi_scale`, `digit_open_ratio`, `invert_digit`, `template_paths` |
| `mode2`     | HSV 黑色阈值, `aspect_ratio_threshold`, `min_area`, `morph_kernel_size` |
| `logging`   | `level`, `log_to_file`, `log_file_path`                       |

### 视频测试

```yaml
camera:
  video_path: "/path/to/test.mp4"   # 非空时优先使用视频文件
```

---

## 日志系统

双通道输出，单例模式，线程安全：

- **控制台：** ANSI 彩色（INFO=绿 WARN=黄 ERROR=红 DEBUG=蓝）
- **文件：** 纯文本（无颜色码），`std::flush` 立即落盘防崩溃丢失
- Debug 日志仅在 `-DCMAKE_BUILD_TYPE=Debug` 时编译
- 启用：`log_to_file: true` + `log_file_path: "../vision.log"`

---

## 调试功能

| 功能           | YAML 控制              | 说明                                         |
|---------------|------------------------|----------------------------------------------|
| 主窗口         | `ui.imshow_enable`     | OpenCV 窗口，含 FPS/延迟/模式/队列/串口状态     |
| 叠加层         | `ui.ui_draw_enable`    | 目标位置圆圈+标签、ROI 矩形框                   |
| 二值化调试窗   | `ui.show_binary`       | 模式0: 彩色掩码合成  模式1: 各圆环数字ROI二值图（水平拼接）  模式2: 黑色掩码 |
| 串口 HEX 日志  | `-DCMAKE_BUILD_TYPE=Debug` | 每包 9 字节完整打印                          |

---

## 异常处理

| 场景           | 策略                                      |
|---------------|-------------------------------------------|
| 摄像头断连     | release → sleep 1s → reopen，最多 5 次，超限退出 |
| 串口发送失败   | `LOG_WARN` 提示，继续运行                   |
| 空帧           | 返回 `nullopt`，调用方 sleep 1ms 后重试      |
| 配置键缺失     | 立即 `exit(EXIT_FAILURE)` 并打印错误键名      |
| 模式1 单环识别失败 | 跳过该圆环，继续处理其他圆环                    |
| 模式1 同编号冲突 / 画面拥挤 | 丢弃整帧，不发送，Kalman 计一次丢失      |
| 模式1 TARGET 未检出 | 不发送，Kalman 计一次丢失                    |

---

## 目录结构

```
├── CMakeLists.txt              # CMake 构建（C++17, ARM NEON, OpenCV 6 模块）
├── README.md                   # 本文件
├── .gitignore
├── 核心记忆.md                 # AI 对话快速恢复项目记忆
├── templates/                 # 模式1 数字模板 num1/2/3.png（白底黑字）
├── config/
│   └── vision.yaml             # 唯一配置来源
├── include/
│   ├── ConfigManager.h         # YAML 配置单例（模板 getRequired 安全读取）
│   ├── CameraCapture.h         # V4L2 摄像头封装（5 次重试）
│   ├── SerialPort.h            # 非阻塞 UART（termios 8N1 + poll/select）
│   └── VisionPipeline.h        # 4 模式视觉调度 + Kalman 管理 + 调试图像
├── src/
│   ├── main.cpp                # 入口 + 三线程编排 + 限频门控 + UI 绘制
│   ├── ConfigManager.cpp       # 42 个 YAML 访问器
│   ├── CameraCapture.cpp       # 异常安全读帧 + 断连重连
│   ├── SerialPort.cpp          # 帧同步解析 + HEX 日志 + 波特率自适应
│   └── VisionPipeline.cpp      # 模式0(HSV+轮廓) 模式1(多圆环Hu矩+matchShapes) 模式2(minAreaRect) + Kalman
├── tools/
│   ├── Logger.h                # 双通道日志单例（控制台彩色 + 文件纯文本 flush）
│   ├── FPSCounter.h            # 1 秒滑动窗帧率计算
│   ├── ThreadSafeFrameBuffer.h # 模板单槽帧缓冲（覆盖旧帧）
│   ├── ThreadSafeSendQueue.h   # 生产者-消费者队列 + DetectionResult 定义
│   ├── ColorOrder.h            # 6 色枚举 + 名称数组
│   └── capture_templates.py    # 模式1 数字模板制作工具（OpenCV Python）
└── scripts/
    └── autostart.sh            # 唯一自启动脚本：开机注册 + 看门狗（start/stop/restart/status）
```
