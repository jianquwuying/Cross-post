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
../scripts/autostart.sh doctor       # 环境自检（可执行文件/串口/摄像头/权限）
```

按 `Ctrl+C` 发送 SIGINT 优雅退出。

## 开机自启动（看门狗）

在桌面「启动应用程序」添加启动项，「命令」一栏填写（**必须写绝对路径**）：

```bash
/home/pi/Cross-post/scripts/autostart-gui.sh
```

本机已配置好该启动项：`~/.config/autostart/gnome-terminal.desktop`（含 `X-GNOME-Autostart-Delay=10`）。

> ⚠️ **`.desktop` 的 `Exec` 不经过 shell**，`$HOME`、`~`、环境变量都不会被展开；
> 写成 `$HOME/Cross-post/...` 会被 `desktop-file-validate` 判为非法
> （`reserved character '$' outside of a quote`），自启动项**根本不会执行**。
> 可用 `desktop-file-validate ~/.config/autostart/*.desktop` 自查。

`autostart-gui.sh` 是自启动入口，解决"登录初期终端起不来"的问题：

- 优先弹终端窗口跑看门狗（方便现场看日志），最多重试 3 次
- 登录初期 `gnome-terminal-server` / D-Bus 未就绪时，`gnome-terminal` 会报
  `Error constructing proxy for org.gnome.Terminal ... 已到超时限制` 并以退出码 1 失败，
  **此时终端不会弹出、看门狗也就不会启动**（这是本机曾出现过的故障）
- 重试仍失败则退化为**后台无窗口运行**，保证机器人一定跑起来；结果记在 `record/autostart-gui.log`

`autostart.sh` 同时承担启动/看门狗/停止/状态查询/环境自检（`start|stop|restart|status|doctor`）：

- visionpost 崩溃后自动重启；连续快速崩溃超过 `MAX_RESTARTS` 次（默认 10）则放弃并保持终端提示
- **硬件未就绪**（开机时摄像头/串口尚未枚举完）改走长间隔重试（`HW_RETRY_SEC`，默认 5s），
  **不消耗快速崩溃额度**，避免开机慢导致看门狗提前放弃
- 程序 stdout/stderr 同时显示在终端并写入 `record/visionpost.out.log`；
  快速崩溃时把最后几行输出写进看门狗日志，便于定位原因
- 串口放权对象自动识别：先取 `config/vision.yaml` 的 `serial.port`，再扫描
  `/dev/ttyACM*`、`/dev/ttyUSB*`、`/dev/ttyCH341USB*`（本机用 USB 转串口 `/dev/ttyACM0`，不用板载 UART）；
  先试免密 `sudo -n`，失败再回退密码管道（`SUDO_PASS`，本机为 1，可用 `AUTOSTART_SUDO_PASS` 覆盖）
- `bash scripts/autostart.sh doctor` 一键自检：可执行文件 / 配置 / 串口及权限 / 摄像头 / sudo


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

### 模式0 — TARGET 指定单色物块检测

只识别串口 `TARGET` 指定的那一种颜色（与模式1 的"按 TARGET 精准输出"对齐），
一帧只推一包，不再一次推 6 色结果。

- `TARGET`：**0=空闲**（不检测，按开关发无目标帧），**1–6=红/黄/蓝/绿/黑/青**
- 流水线：TARGET → ROI → BGR→HSV → 该颜色 `inRange`（红色双区间）→ 形态学开运算 → 轮廓 → 取最大面积 → `moments` 质心 → Kalman → 单包输出
- **内部索引 0–5，外部（协议）值 1–6**：数组索引与 Kalman key 用 `TARGET-1`，
  `DetectionResult.color_id` / 串口 `Color` 直接用外部值（1–6），SerialPort 层不做任何 ±1 转换
- 未检出时按无目标帧机制推送（`type=0`，`Color=` 该颜色 1–6）
- 二值化调试窗：当前 TARGET 颜色的**单通道掩码**（灰度，不再是 6 色 BGR 合成图）

### 模式1 — 多圆环数字识别（同心环剥除 + 归一化位图多角度 IoU，TARGET 指定输出）

视野内可**同时出现多个圆环**，算法逐个识别数字编号，只输出串口命令中 `TARGET` 指定的那一个。靶标实物为「**中心衬线数字 1/2/3 + 多层同心细环**」的印章式图案；摄像头固定在机械臂上可能旋转。单环识别原理：

1. 提取圆环外轮廓，得圆心与半径；
2. 中心方形裁剪（半宽 = `digit_roi_scale × 半径`）；
3. 数字区自适应二值化后，用**随半径缩放的开运算核**（`digit_open_ratio × 半径`）把同心细环剥掉、只留粗数字笔画 —— 环线宽度与靶标半径成比例，故开核也随半径缩放，远近通用；
4. 取靠近圆心的最大墨块作为数字轮廓；
5. 数字墨块与模板都按外接框归一化成 `digit_match_size²` 二值位图，模板预生成多角度实例
   （`±digit_match_angle_range`、步长 `digit_match_angle_step`），**取 IoU 最大者**判数字编号；
   若"最高 IoU − 次高 IoU < `digit_min_margin`"则**拒答**（该环不判号，宁可没有也不给错编号）。

> **为什么不用 `cv::matchShapes`（Hu 矩）**：实机实测其 `CONTOURS_MATCH_I1` 距离对"笔画粗细 /
> 渲染风格"极端敏感——真实相机的数字与任一模板的距离都是 **≈6**，而 2 与 3 两个模板之间只有
> **1.2**，判号被风格差异主导（同一模板自身缩放 1.1× 就会产生 1.34 的距离）。表现为
> 相机滚转 ±30° 即把 2 判成 3。改成归一化位图 IoU 后，同一姿态下正确数字 **0.70**、次优
> **0.44**（≈2 倍裕量），旋转由多角度实例覆盖。
>
> **置信度门槛**：真机 3 号靶标 126 个样本（±90° 旋转 / 0.5~1.3 缩放 / 离焦模糊 / 0.7~1.3 亮度）
> 中，正确判定的"最高−次高"裕量最小 **0.083**、中位 0.164，而唯一一例误判（严重离焦+过曝）
> 裕量只有 **0.056**；取 `digit_min_margin: 0.07` 可剔除该误判且不损失任何正确判定。

每帧的圆环处理顺序：

```
面积过滤(min_contour_area) → 形状过滤（圆度 + 长宽比 + 填充率，剔除线状/方形干扰）
   → 按面积降序 → 嵌套去重（同心环只保留最大外环）
      → 取前 max_rings_per_frame 个 → 逐环识别数字（归一化位图多角度 IoU）
      → 单个环识别失败只跳过该环
      → 同编号冲突（同编号出现两次）→ 整帧丢弃（Kalman 计一次丢失）
      → 按 TARGET 筛选 → 见下方「推送逻辑矩阵」
```

**TARGET>0 时的推送逻辑矩阵**（由 `serial.enable_no_target_frame` 总控）：

| 场景 | TARGET 识别 | Kalman | 画面有其他圆环 | 推送 |
|------|------------|--------|--------------|------|
| 1 | ✅ | 正常 | 无关 | `type=2, color=TARGET, x, y`（真实滤波坐标） |
| 2 | ❌ | **未超时** | 无关 | `type=2, color=TARGET, x_pred, y_pred`（预测坐标，保持连续）；**不推**其他环 |
| 3a | ❌ | **已超时/冷启动** | ✅ N 个 | 为**每个**其他圆环推一帧 `type=2, color=该环编号, x=0, y=0`（零位包，不给坐标）；**不推** `type=0` |
| 3b | ❌ | **已超时/冷启动** | ❌ 无 | `type=0, color=TARGET, x=0, y=0`（无目标帧） |

开关关闭时：场景 1 照常；场景 2/3a/3b 均**不推送任何包**。

- **形状过滤**（`enable_shape_filter`，默认开）：三道校验依次为
  ① `圆度 = 4π·面积/周长²`（`arcLength(c,true)`）剔线状干扰，
  ② `minAreaRect` 长宽比剔长条/畸形块，
  ③ `填充率 = 面积/(π·外接圆半径²)` 剔方形/矩形块。
  实测：真实靶标外环 圆度 0.885 / 长宽比 1.00 / 填充率 0.98；桌边线 圆度 0.018；
  正方形块 圆度 0.790 / 长宽比 1.00 / 填充率 0.64 —— 注意正方形**圆度并不低**
  （理论值 π/4≈0.785），只靠圆度+长宽比拦不住，必须靠填充率。
  触发时 Debug 日志会打印具体数值。现场若怀疑误杀，把 `enable_shape_filter`
  置 `false` 即可一键回退
- **嵌套去重**：真机上同心圆环二值化后是**断开的独立轮廓**（外环/中环/内环各一个），
  判定"当前轮廓是已保留轮廓的嵌套碎片"需**同时**满足
  ① 包围盒被包含（坐标比较带 2px 容差）② 两轮廓圆心距 < `0.5 × 外环半径`；
  丢弃前者、只留最大外环 —— 保证「一个物理靶标 = 一个候选」。
  加上圆心校验是为了避免两个独立靶标在画面上重叠时被误合并（那会直接导致 TARGET 找不到环）
- `TARGET=0`：圆环空闲，不检测、不输出（`max_rings_per_frame` 不参与）
- 成功 → `target_type=2`，`color_id=TARGET 编号(1/2/3)`，`x/y=滤波后圆心`；Kalman key 用 TARGET 编号
- 只对 TARGET 指定圆环做 Kalman 更新；其他圆环即使检出也不进滤波器
- **零位包**（`type=2` 且 `x=y=0`）：语义是"识别到该编号圆环、但不是 TARGET，坐标无效"；
  与 `type=0`（无目标）独立。UI 不会绘制零位包（避免在 (0,0) 画圈）
- 同编号冲突：整帧丢弃（优先级高于零位包），此时**不推**零位包
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

继续采集，不检测。`serial.enable_no_target_frame` 开启时按 `send_interval_ms`
周期发送无目标帧（`TargetType=0`，`Color=0`）作为心跳；关闭时不发送。

---

## 发送节流机制

视觉算法始终 60fps 全速运行，串口推送受 YAML 控制：

| 触发条件                       | 行为                                    |
|-------------------------------|-----------------------------------------|
| 模式切换（`MODE` 或 `TARGET` 变化）| **立即抢断发送**，重置计时器            |
| 距上次发送 ≥ `send_interval_ms`| 允许发送                                |
| 无有效目标                     | 开启 `serial.enable_no_target_frame` 时发送**无目标帧**（`TargetType=0`）并按周期推送；关闭时什么都不发 |
| 目标短暂丢失（Kalman 未超时）  | 发送 **Kalman 预测坐标**（`TargetType` 仍为 1/2/3），保证坐标连续；超时复位后才转无目标帧 |
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
0xCD  0x01  MODE(0-3)  TARGET(0-6)  0xDC
```

| 字节 | 字段     | 值                                                         |
|------|----------|------------------------------------------------------------|
| 0    | `0xCD`   | 帧头                                                        |
| 1    | `0x01`   | 命令类型                                                     |
| 2    | `MODE`   | 0=颜色物块  1=圆环  2=圆柱  3=空闲                             |
| 3    | `TARGET` | `MODE=0`：0=空闲，1–6=红/黄/蓝/绿/黑/青；<br>`MODE=1`：0=空闲，1–3=指定输出的圆环编号（不受颜色编号影响）；<br>`MODE=2/3`：忽略（内部强制为 0） |
| 4    | `0xDC`   | 帧尾                                                        |

校验：第 1 字节为 `0x01`、`MODE ≤ 3`、`TARGET ≤ 6`、帧尾为 `0xDC`，任一不满足则丢弃该帧并继续搜寻 `0xCD`；
按模式的 TARGET 范围（模式0 ≤6、模式1 ≤3）由 `VisionPipeline::setMode` 处理，
越界时**仍切换模式**并打 WARN，把 TARGET 当 0（空闲）。

### 发送 — 检测结果（9 字节）

```
0xCD  0x06  TargetType  Color  X_H  X_L  Y_H  Y_L  0xDC
```

| 字节 | 字段         | 值                                                        |
|------|-------------|-----------------------------------------------------------|
| 0    | `0xCD`      | 帧头                                                       |
| 1    | `0x06`      | 数据类型                                                   |
| 2    | TargetType  | 0=**无目标帧**  1=颜色物块  2=圆环  3=圆柱                    |
| 3    | Color       | 有目标时：模式0=**颜色 1–6**(1=红,2=黄,3=蓝,4=绿,5=黑,6=青)，模式1=TARGET 编号(1–3)；<br>无目标帧(0)时：模式0=丢失的颜色(1–6)，模式1=丢失的 TARGET 编号（TARGET=0 时为 0），模式2/3=0 |
| 4-5  | X           | `uint16_t` 大端序                                          |
| 6-7  | Y           | `uint16_t` 大端序                                          |
| 8    | `0xDC`      | 帧尾                                                       |

- **无目标帧**：`X=Y=0`，`Color` 标识是哪个目标丢失；由 `serial.enable_no_target_frame`
  控制（默认 `true`），用于让主控区分"视觉系统挂了"与"当前视野确实没目标"。
  置 `false` 则完全回退（无目标不发、也不发预测坐标）
- **非 TARGET 圆环零位包**：`TargetType=2` 且 `X=Y=0` —— 表示"识别到 `Color` 编号的圆环、
  但它不是 TARGET，坐标无效"（模式1 专用；Kalman 超时/冷启动且画面有其他圆环时逐环各推一帧）
- 模式0 每帧只推 TARGET 指定颜色的**一包**结果（不再逐色推 6 包）
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
| `serial`    | `port`, `baudrate`, `send_interval_ms`, `enable_no_target_frame` |
| `kalman`    | `process_noise_cov`, `measurement_noise_cov`, `init_frames`, `lost_frame_limit` |
| `recording` | `record_mode`, `output_dir`, `fourcc`, `fps`                 |
| `roi`       | `x`, `y`, `width`, `height`                                  |
| `mode0`     | `min_contour_area`, `morph_kernel_size`, 6 色 HSV 双区间（识别的颜色由串口 TARGET 指定） |
| `mode1`     | `gaussian_kernel_size`, `adaptive_block_size`, `adaptive_c`, `min_contour_area`, `max_rings_per_frame`, `enable_shape_filter`, `min_circularity`, `max_aspect_ratio`, `min_fill_ratio`, `morph_open_kernel_size`, `digit_roi_scale`, `digit_open_ratio`, `digit_adaptive_block_ratio`, `invert_digit`, `digit_match_size`, `digit_match_angle_range`, `digit_match_angle_step`, `digit_min_margin`, `template_paths` |
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
| 模式1 同编号冲突 | 丢弃整帧，不发送，Kalman 计一次丢失              |
| 模式1 TARGET 未检出 | 不发送，Kalman 计一次丢失                    |
| 模式1 靶标突然不输出（怀疑形状过滤误杀） | `mode1.enable_shape_filter: false` 一键回退；Debug 日志会打印被过滤轮廓的圆度/长宽比/填充率，据此下调 `min_circularity`、放宽 `max_aspect_ratio` 或下调 `min_fill_ratio` |

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
│   └── VisionPipeline.cpp      # 模式0(HSV+轮廓) 模式1(多圆环+归一化位图IoU) 模式2(minAreaRect) + Kalman
├── tools/
│   ├── Logger.h                # 双通道日志单例（控制台彩色 + 文件纯文本 flush）
│   ├── FPSCounter.h            # 1 秒滑动窗帧率计算
│   ├── ThreadSafeFrameBuffer.h # 模板单槽帧缓冲（覆盖旧帧）
│   ├── ThreadSafeSendQueue.h   # 生产者-消费者队列 + DetectionResult 定义
│   ├── ColorOrder.h            # 6 色枚举 + 名称数组
│   └── capture_templates.py    # 模式1 数字模板制作工具（OpenCV Python）
└── scripts/
    ├── autostart-gui.sh        # 开机自启动入口（优先弹终端，失败退化后台运行）
    └── autostart.sh            # 看门狗 + 自检（start/stop/restart/status/doctor）
```
