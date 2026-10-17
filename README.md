# RaspberryPi5 视觉识别系统

> **26年第十二届工程实践与创新大赛** · 参赛作品

树莓派 5 上的实时视觉终端：纯传统 OpenCV 流水线完成目标检测与跟踪，经串口把目标坐标上报主控。无深度学习、无 GPU 依赖。

**可执行程序：** `visionpost`

---

## 设备与运行环境

| 项目 | 规格 |
|------|------|
| 主控 | Raspberry Pi 5 Model B（ARM Cortex-A76 ×4，8 GB） |
| 系统 | Ubuntu 24.04.4 LTS（aarch64） |
| 摄像头 | USB UVC，MJPG 640×480 @ 60 FPS（V4L2） |
| 串口 | USB 转串口 `/dev/ttyACM0`，115200 8N1（不使用板载 UART） |
| 工具链 | GCC 13.3+ / CMake 3.28+ / OpenCV 4.8+ / yaml-cpp 0.8+ |

```bash
sudo apt install build-essential cmake libopencv-dev libyaml-cpp-dev
```

---

## 系统架构

```
CameraThread ──(单槽帧缓冲)──> ProcessThread ──(发送队列)──> SerialThread
      │                          │                             │
      └── 录制原始视频            └── 限频门控 + UI            /dev/ttyACM0
                              模式命令（atomic<uint8_t>）────────┘
```

| 线程 | 职责 |
|------|------|
| **CameraThread** | 采集 → 录制原始帧 → 写入单槽缓冲（覆盖旧帧，始终处理最新画面） |
| **ProcessThread** | 视觉算法 + Kalman（全速 60 fps）→ 节流判定 → 发送队列 → UI 绘制 |
| **SerialThread** | `poll` 接收模式命令 → 排空发送队列 → 非阻塞写 UART（包间 100 µs） |

线程安全：帧缓冲 `mutex` 单槽覆盖；发送队列 `mutex + condition_variable`；当前模式 `atomic<uint8_t>`；UART fd 仅 SerialThread 独占。

---

## 视觉模式

主控发送 `MODE0`–`MODE3` 切换，**模式变化即清空全部 Kalman 跟踪器**。

| 模式 | 功能 | 每周期输出 |
|------|------|-----------|
| **0** | 6 色物块「逐槽位快照」：红/黄/蓝/绿/黑/青，每色独立跑 HSV → 轮廓 → 质心 | **6 包**，`Color` = 颜色 1–6 |
| **1** | 多圆环数字识别：同心环剥除 + 归一化位图多角度 IoU，按优先级 **2 > 1 > 3** | **1 包**，`Color` = 环号 1–3 |
| **2** | 黑色圆柱：`minAreaRect` 长宽比 + 面积过滤 | **1 包**，`Color` = 0 |
| **3** | 空闲：不检测，仅心跳 | 1 包 `type=0` |

### 模式0 — 逐槽位快照

每个颜色一个槽位（协议值 1=红 … 6=青），**每帧固定 6 包**：

| 槽位状态 | 推送 |
|---------|------|
| 检出 | `type=1, Color=槽位, x, y`（Kalman 滤波后；初始化前为原始测量值） |
| 未检出但未超时 | `type=1, Color=槽位, x_pred, y_pred` |
| 超时 / 冷启动 | `type=0, Color=槽位, 0, 0`（0 值包） |

红色因 HSV 色相环绕使用双区间 `H[0,10] ∪ H[170,180]`。内部索引 / Kalman key = 槽位−1。

### 模式1 — 多圆环数字识别

靶标为「中心衬线数字 1/2/3 + 多层同心细环」的印章图案，视野内可同时出现多个。

```
ROI → 灰度 → 高斯模糊 → 自适应阈值 → 开运算 → 轮廓
  → 面积过滤 → 形状过滤（圆度 / minAreaRect 长宽比 / 填充率）
  → 嵌套去重（同心环只留最大外环）→ 取前 max_rings_per_frame 个
  → 逐环：圆心 + 半径 → 中心方形数字 ROI → 自适应二值化
         → 开运算剥同心细环（核 ∝ 半径）→ 取靠近圆心的最大墨块
         → 归一化位图 vs 多角度模板 IoU
  → 按优先级 2 > 1 > 3，每周期只推 1 包
```

| 优先级链上该环号状态 | 推送 |
|---------------------|------|
| 本帧识别到 | `type=2, Color=环号, x, y`（滤波坐标；Kalman 初始化前为原始圆心） |
| 未出现但 Kalman 未超时 | `type=2, Color=环号, 预测坐标` |
| 三者都不可用 | `type=0, Color=2, 0, 0`（0 值包） |

- 选择**严格按环号**：环2 的预测坐标可压过环1 的真实检出；三个环的 Kalman 每帧都推进
- 同编号冲突只废该环号，其余环号照常（不再整帧丢弃）
- **判号置信度门槛**：最高 IoU − 次高 IoU < `digit_min_margin` 时拒答 —— 宁可「没识别到」也不给错编号
- 数字匹配已弃用 `cv::matchShapes`/Hu 矩：实机验证其对笔画粗细极敏感，改为归一化二值位图 + 多角度模板 IoU

---

## 串口协议

### 接收 — 模式命令（5 字节）

```
0xCD  0x01  MODE(0-3)  TARGET(任意)  0xDC
```

`TARGET` 位**保留但完全忽略**（不校验、不参与任何判断）。校验 `0x01`、`MODE ≤ 3`、`0xDC`，失败即丢弃并继续搜寻 `0xCD`。

### 发送 — 检测结果（9 字节）

```
0xCD  0x06  TargetType  Color  X_H  X_L  Y_H  Y_L  0xDC
```

| 字段 | 含义 |
|------|------|
| `TargetType` | 0=无目标 1=颜色物块 2=圆环 3=圆柱 |
| `Color` | 槽位号：模式0=颜色 1–6；模式1=环号 1–3；模式2/3=0 |
| `X` / `Y` | `uint16_t` 大端序 |

- **0 值包**：`TargetType=0` 且 `X=Y=0` —— 表示该 `Color` 槽位当前无目标
- 每周期包数：模式0 → 6 包；模式1 → 1 包；模式2/3 → 1 包
- `serial.enable_no_target_frame`：`true` 补齐预测坐标与 0 值包；`false` 只播报真实检出

---

## 构建与部署

```bash
# 编译（Release，-O3）
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4

# 运行（默认读取 ../config/vision.yaml）
./visionpost
./visionpost /path/to/custom.yaml
```

### 看门狗与自启动

```bash
scripts/autostart.sh            # start：前台监管，崩溃自动拉起，Ctrl+C 整体退出
scripts/autostart.sh stop       # 优雅停止
scripts/autostart.sh status     # 运行状态
scripts/autostart.sh restart    # 重启
scripts/autostart.sh doctor     # 环境自检（可执行文件/串口/摄像头/权限）
```

- 快速崩溃连续超过 `MAX_RESTARTS`（10）次则放弃；**硬件未就绪**（摄像头/串口未枚举）走 5 s 长间隔重试，不消耗崩溃额度
- 串口权限自动放行：先读 `serial.port`，再扫描 `/dev/ttyACM*`、`/dev/ttyUSB*`、`/dev/ttyCH341USB*`
- 开机自启入口为 `scripts/autostart-gui.sh`，优先弹终端并重试 3 次，失败退化为后台运行

> `.desktop` 的 `Exec` 不经过 shell，**必须写绝对路径**；`$HOME`、`~` 不会展开，写成变量会被判非法而不执行。

---

## 调试指南

| 开关 / 手段 | 作用 |
|------------|------|
| `ui.imshow_enable` | 主窗口：显示处理后的画面（状态栏由 `ui_draw_enable` 控制） |
| `ui.ui_draw_enable` | 叠加层：ROI 框、圆环、检测点与标签、状态栏（FPS / 延迟 / 模式 / 队列 / 串口） |
| `ui.show_binary` | 二值化调试窗：模式0=6 色掩码合成；模式1=数字 ROI 二值图拼接（无有效数字时回退环级二值图）；模式2=黑色掩码 |
| `-DCMAKE_BUILD_TYPE=Debug` | 输出 `LOG_DEBUG`，含每包 9 字节 HEX、判号 IoU 与裕量 |
| `scripts/autostart.sh doctor` | 一键定位硬件与权限问题 |

> 状态栏中的 `Cam FPS` 恒为 `0.0`、`Serial` 恒为 `OK`：两者是占位值，尚未接入真实数据。

**常见问题**

| 现象 | 排查 |
|------|------|
| 配置加载失败 | YAML 路径错误或缺顶层节；具体缺键在被首次读取时打印键名并退出（`logging.level`、`kalman.stable_distance_threshold` 当前无调用方，缺失不触发退出） |
| 摄像头打不开 | 设备索引 / 权限 / 被占用；可设 `camera.video_path` 用视频离线测试 |
| 串口打不开 | 跑 `doctor`；确认 `/dev/ttyACM0` 存在且可写 |
| 模式1 圆环检测不到 | 调 `min_contour_area` / `morph_open_kernel_size` / `adaptive_c` |
| 模式1 判号不稳 | 先看二值化调试窗：数字 ROI 应只剩完整数字、无环弧；再调 `digit_open_ratio` / `digit_min_margin` |
| 真靶标被形状过滤误杀 | 临时置 `mode1.enable_shape_filter: false`；Debug 日志会打印被过滤轮廓的圆度/长宽比/填充率 |
| UI 显示 `????` | `cv::putText` 只支持 ASCII，所有 UI 文本必须纯英文 |
| 串口粘包/丢包 | Debug 编译查看 HEX；包间 100 µs 间隔已内置 |

---

## 配置

**所有参数来自 `config/vision.yaml`，代码中无硬编码默认值；任一被读取的键缺失或类型不符即 `exit(EXIT_FAILURE)`（`logging.level` 与 `kalman.stable_distance_threshold` 当前没有调用方，缺失不会触发退出）。** 修改后无需重新编译。

| 配置节 | 内容 |
|--------|------|
| `camera` | 设备索引 / 视频路径 / 分辨率 / 帧率 |
| `ui` | 窗口与叠加层开关、二值化调试窗 |
| `serial` | 端口、波特率、发送间隔、无目标帧开关 |
| `kalman` | 过程/测量噪声、初始化帧数、超时帧数 |
| `recording` | 原始帧录制（编码、帧率、输出目录） |
| `roi` | 感兴趣区域 |
| `mode0` | 6 色 HSV 阈值、最小轮廓面积 |
| `mode1` | 圆环检测、形状过滤、数字识别与匹配参数 |
| `mode2` | 黑色 HSV 阈值、长宽比、面积 |
| `logging` | 级别、文件输出 |

---

## 目录结构

```
├── CMakeLists.txt              # C++17 / ARM NEON / OpenCV 6 组件
├── config/vision.yaml          # 唯一配置来源
├── templates/                  # 模式1 数字模板 num1/2/3.png
├── include/                    # ConfigManager / CameraCapture / SerialPort / VisionPipeline
├── src/                        # main + 四个实现文件
├── tools/                      # Logger / FPSCounter / 锁封装 / ColorOrder / 模板与 HSV 工具
└── scripts/                    # autostart.sh（看门狗）/ autostart-gui.sh（自启入口）
```
