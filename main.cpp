/// @file main.cpp
/// @brief 多线程实时视觉识别流水线 — 集成版。
///
/// 集成优化:
///   - 颜色置信度机制 (ColorResult.confidence 完整传播)
///   - ROI 预裁剪模块 (降低计算量, 减少背景误检)
///   - YAML 配置管理 (修改参数无需重新编译)
///   - FP16/INT8 双模型部署 (启动时选择, 运行时不可切换)
///   - 终端彩色日志系统 (ANSI INFO/WARN/ERROR)
///   - 多目标检测 (vector<ColoredObject>, 扩展接口预留)
///
/// 数据流:
///   摄像头 → [ROI裁剪] → YOLO检测 → HSV颜色分类 → 串口发送
///
/// Raspberry Pi 5 | Ubuntu 24.04 ARM64 | C++17 | NCNN | OpenCV 4.8

#include "protocol/protocol.h"
#include "camera/camera.h"
#include "yolo/yolo26.h"
#include "color/hsv.h"
#include "serial/serial.h"
#include "config/config.h"
#include "roi/roi.h"
#include "logger/logger.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// ═══════════════════════════════════════════════════════════════════
//  SafeQueue — 线程安全的有界队列
// ═══════════════════════════════════════════════════════════════════

template<typename T>
class SafeQueue {
public:
    explicit SafeQueue(size_t maxSize = 4) : maxSize_(maxSize) {}

    void push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        fullCV_.wait(lock, [this] { return queue_.size() < maxSize_ || !running_; });
        if (!running_) return;
        queue_.push(std::move(item));
        emptyCV_.notify_one();
    }

    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        emptyCV_.wait(lock, [this] { return !queue_.empty() || !running_; });
        if (!running_ && queue_.empty()) return T{};
        T item = std::move(queue_.front());
        queue_.pop();
        fullCV_.notify_one();
        return item;
    }

    bool tryPop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop();
        fullCV_.notify_one();
        return item;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        fullCV_.notify_all();
        emptyCV_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool isRunning() const { return running_; }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable emptyCV_;
    std::condition_variable fullCV_;
    size_t maxSize_;
    std::atomic<bool> running_{true};
};

// ═══════════════════════════════════════════════════════════════════
//  ColoredObject — 扁平化多目标检测结果
// ═══════════════════════════════════════════════════════════════════

struct ColoredObject {
    // ── YOLO 检测 ──────────────────────────────────────────────
    int    classId      = -1;          ///< 类别: 0=A, 1=B
    float  confidence   = 0.0f;        ///< YOLO 检测置信度 [0, 1]
    float  x1 = 0, y1 = 0;             ///< 边界框左上角
    float  x2 = 0, y2 = 0;             ///< 边界框右下角
    float  cx = 0, cy = 0;             ///< 中心点坐标

    // ── HSV 颜色（仅 A 类有效） ────────────────────────────────
    Color  colorId          = Color::Unknown;  ///< 颜色类别
    float  colorConfidence  = 0.0f;            ///< 颜色置信度 [0, 1]

    // ── 扩展接口（预留） ──────────────────────────────────────
    // float depth;         // 未来: 双目深度
    // int   trackId;       // 未来: 目标跟踪ID
    // float velocityX;     // 未来: 运动速度
};

/// 从 YOLO Object 创建 ColoredObject
inline ColoredObject fromYoloObject(const Object& obj) {
    ColoredObject co;
    co.classId    = obj.label;
    co.confidence = obj.confidence;
    co.x1 = obj.x1(); co.y1 = obj.y1();
    co.x2 = obj.x2(); co.y2 = obj.y2();
    co.cx = obj.cx(); co.cy = obj.cy();
    return co;
}

// ═══════════════════════════════════════════════════════════════════
//  扩展接口（预留）
// ═══════════════════════════════════════════════════════════════════

/// 后处理钩子 — 在颜色分类之后、串口发送之前调用
using PostProcessHook = std::function<void(std::vector<ColoredObject>&)>;

// ═══════════════════════════════════════════════════════════════════
//  全局状态
// ═══════════════════════════════════════════════════════════════════

static std::atomic<bool> g_running{true};

static void signalHandler(int sig) {
    (void)sig;
    g_running = false;
    LOG_INFO << "收到退出信号, 正在关闭...";
}

// ═══════════════════════════════════════════════════════════════════
//  集成检测线程: YOLO + ROI + HSV 颜色
// ═══════════════════════════════════════════════════════════════════

void detectionThread(YOLO26Detector& yolo,
                     HSVClassifier& hsv,
                     RoiCrop& roi,
                     SafeQueue<cv::Mat>& frameQueue,
                     SafeQueue<std::vector<ColoredObject>>& resultQueue,
                     int skipFrames,
                     const AppConfig::ColorCfg& colorCfg)
{
    LOG_INFO << "[检测] 线程已启动 (YOLO + ROI + 颜色)";

    int frameCount = 0;
    auto t0 = std::chrono::steady_clock::now();

    while (g_running && frameQueue.isRunning()) {
        cv::Mat frame = frameQueue.pop();
        if (frame.empty()) continue;

        // ── 跳帧控制 ──────────────────────────────────────────
        if (skipFrames > 0 && frameCount % (skipFrames + 1) != 0) {
            ++frameCount;
            continue;
        }

        // ── 步骤1: ROI 裁剪 ───────────────────────────────────
        cv::Mat processedFrame = roi.isEnabled() ? roi.apply(frame) : frame;

        // ── 步骤2: YOLO 检测 ──────────────────────────────────
        std::vector<Object> objects;
        if (!yolo.detect(processedFrame, objects)) {
            resultQueue.push({});
            continue;
        }

        // ── 步骤3: A 类目标 HSV 颜色 + 坐标映射 ───────────────
        std::vector<ColoredObject> colored;
        colored.reserve(objects.size());

        for (auto& obj : objects) {
            ColoredObject co = fromYoloObject(obj);

            // ROI 坐标映射回全帧坐标
            if (roi.isEnabled()) {
                cv::Rect_<float> mappedBox = roi.mapBack(obj.rect);
                co.x1 = mappedBox.x;
                co.y1 = mappedBox.y;
                co.x2 = mappedBox.x + mappedBox.width;
                co.y2 = mappedBox.y + mappedBox.height;
                co.cx = mappedBox.x + mappedBox.width  * 0.5f;
                co.cy = mappedBox.y + mappedBox.height * 0.5f;
            }

            if (obj.label == 0) {
                // A 类: 裁剪 ROI 并获取完整颜色分类结果
                cv::Rect roiRect(
                    std::max(0, static_cast<int>(obj.x1())),
                    std::max(0, static_cast<int>(obj.y1())),
                    std::min(static_cast<int>(obj.w()), processedFrame.cols - static_cast<int>(obj.x1())),
                    std::min(static_cast<int>(obj.h()), processedFrame.rows - static_cast<int>(obj.y1()))
                );

                if (roiRect.width > 0 && roiRect.height > 0) {
                    cv::Mat roiMat = processedFrame(roiRect);
                    ColorResult cr = hsv.classify(roiMat);

                    co.colorId         = cr.color;
                    co.colorConfidence = cr.confidence;

                    // 低置信度时发出警告
                    if (cr.color != Color::Unknown && cr.confidence < colorCfg.confThreshold * 1.5f) {
                        LOG_WARN << "[颜色] " << colorName(cr.color)
                                 << " 置信度偏低: " << cr.confidence;
                    }
                }
            } else {
                co.colorId         = Color::Unknown;
                co.colorConfidence = 0.0f;
            }

            colored.push_back(std::move(co));
        }

        resultQueue.push(std::move(colored));

        // ── FPS 统计 ──────────────────────────────────────────
        if (++frameCount % 50 == 0) {
            auto t1 = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(t1 - t0).count();
            double fps = 50.0 / elapsed;
            LOG_INFO << "[检测] FPS: " << fps
                     << " (推理: " << yolo.getAvgInferenceMs() << " ms)"
                     << " 目标: " << objects.size();
            t0 = t1;
        }
    }

    resultQueue.shutdown();
    LOG_INFO << "[检测] 线程已停止";
}

// ═══════════════════════════════════════════════════════════════════
//  串口发送线程
// ═══════════════════════════════════════════════════════════════════

void serialThread(SerialPort& serial,
                  SafeQueue<std::vector<ColoredObject>>& resultQueue,
                  float refWidth, float refHeight)
{
    LOG_INFO << "[串口] 线程已启动";

    while (g_running && resultQueue.isRunning()) {
        auto colored = resultQueue.pop();
        if (colored.empty() || !serial.isOpen()) continue;

        uint8_t totalCount = static_cast<uint8_t>(
            std::min(colored.size(), static_cast<size_t>(protocol::MAX_DETECTIONS)));

        std::vector<protocol::DetectionPacket> packets;

        for (size_t i = 0; i < totalCount; ++i) {
            const auto& co = colored[i];

            // 归一化到 [0, 1]
            float cxNorm = std::max(0.0f, std::min(1.0f, co.cx / refWidth));
            float cyNorm = std::max(0.0f, std::min(1.0f, co.cy / refHeight));

            packets.push_back(protocol::makePacket(
                static_cast<protocol::ObjectClass>(co.classId),
                static_cast<protocol::ObjectColor>(co.colorId),
                cxNorm, cyNorm,
                co.confidence,
                co.colorConfidence,  // 颜色置信度
                totalCount
            ));
        }

        auto frame = protocol::buildFrame(packets);
        ssize_t written = serial.write(frame);
        if (written < 0) {
            LOG_ERROR << "[串口] 写入失败";
        }
    }

    LOG_INFO << "[串口] 线程已停止";
}

// ═══════════════════════════════════════════════════════════════════
//  主函数
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    // ── 信号处理 ──────────────────────────────────────────────
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ── 解析 --config 和 --help（需要在 YAML 加载前处理） ─────
    std::string configPath = "config/vision.yaml";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        }
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            std::cout << "用法: " << argv[0] << " [选项]\n"
                      << "  --config  PATH   YAML配置文件路径 (默认: config/vision.yaml)\n"
                      << "  --model   TYPE   模型类型: fp16 | int8 (默认: fp16)\n"
                      << "  --serial  DEV    串口设备 (默认: /dev/ttyAMA0)\n"
                      << "  --baud    RATE   波特率 (默认: 115200)\n"
                      << "  --camera  ID     摄像头设备ID (默认: 0)\n"
                      << "  --size    PIX    模型输入尺寸 (默认: 320)\n"
                      << "  --threads N      NCNN线程数 (默认: 4)\n"
                      << "  --conf    F      置信度阈值 (默认: 0.25)\n"
                      << "  --log-level LVL  日志级别: debug|info|warn|error (默认: info)\n"
                      << "  --help, -h       显示本帮助信息\n"
                      << "\n所有参数也可通过 YAML 配置文件设置。\n";
            return 0;
        }
    }

    // ── 加载 YAML 配置 ────────────────────────────────────────
    AppConfig cfg = AppConfig::load(configPath);
    cfg.applyCliOverrides(argc, argv);

    // ── 配置日志 ──────────────────────────────────────────────
    Logger::setLevel(
        cfg.log.level == "debug" ? LogLevel::DEBUG :
        cfg.log.level == "warn"  ? LogLevel::WARN  :
        cfg.log.level == "error" ? LogLevel::ERROR : LogLevel::INFO);
    Logger::setUseColor(cfg.log.useColor);

    std::cout << "\n╔══════════════════════════════════════════════════╗\n";
    std::cout << "║  视觉识别系统 v1.1 — 树莓派5 + NCNN              ║\n";
    std::cout << "║  YOLO26n + OpenCV 4.8 + UART 串口通信            ║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n";

    cfg.print();

    // ── 初始化 YOLO ───────────────────────────────────────────
    YOLO26Detector::Config ycfg;
    ycfg.modelDir      = cfg.yolo.modelDir;
    ycfg.modelType     = cfg.yolo.modelType == "int8" ? ModelType::INT8 : ModelType::FP16;
    ycfg.inputWidth    = cfg.yolo.inputWidth;
    ycfg.inputHeight   = cfg.yolo.inputHeight;
    ycfg.numClasses    = cfg.yolo.numClasses;
    ycfg.numThreads    = cfg.yolo.numThreads;
    ycfg.confThreshold = cfg.yolo.confThreshold;
    ycfg.nmsThreshold  = cfg.yolo.nmsThreshold;

    YOLO26Detector yolo;
    if (!yolo.load(ycfg)) {
        LOG_ERROR << "无法加载 YOLO 模型";
        return 1;
    }

    // ── 初始化 HSV 分类器 ─────────────────────────────────────
    HSVClassifier::Config hsvCfg;
    hsvCfg.colorThreshold    = cfg.color.threshold;
    hsvCfg.colorConfThreshold = cfg.color.confThreshold;
    hsvCfg.morphKernelSize   = cfg.color.morphKernel;
    hsvCfg.blurKernelSize    = cfg.color.blurKernel;

    auto toScalar = [](const AppConfig::HSVRangeCfg& r) -> cv::Scalar {
        return cv::Scalar(r.hLow, r.sLow, r.vLow);
    };
    auto toScalarHigh = [](const AppConfig::HSVRangeCfg& r) -> cv::Scalar {
        return cv::Scalar(r.hHigh, r.sHigh, r.vHigh);
    };

    hsvCfg.red1   = { toScalar(cfg.color.red1),   toScalarHigh(cfg.color.red1) };
    hsvCfg.red2   = { toScalar(cfg.color.red2),   toScalarHigh(cfg.color.red2) };
    hsvCfg.blue   = { toScalar(cfg.color.blue),   toScalarHigh(cfg.color.blue) };
    hsvCfg.yellow = { toScalar(cfg.color.yellow), toScalarHigh(cfg.color.yellow) };

    HSVClassifier hsv(hsvCfg);

    // ── 初始化 ROI ────────────────────────────────────────────
    RoiConfig roiCfg;
    roiCfg.enabled = cfg.roi.enabled;
    roiCfg.x       = cfg.roi.x;
    roiCfg.y       = cfg.roi.y;
    roiCfg.width   = cfg.roi.width;
    roiCfg.height  = cfg.roi.height;

    RoiCrop roi;
    roi.configure(roiCfg);

    // ── 初始化摄像头 ──────────────────────────────────────────
    Camera camera;
    if (!camera.open(cfg.camera)) {
        LOG_ERROR << "无法打开摄像头";
        return 1;
    }

    // ── 初始化串口 ────────────────────────────────────────────
    SerialPort::Config serCfg;
    serCfg.device   = cfg.serial.device;
    serCfg.baudrate = cfg.serial.baudrate;
    serCfg.dataBits = cfg.serial.dataBits;
    serCfg.stopBits = cfg.serial.stopBits;
    serCfg.parity   = cfg.serial.parity[0];

    SerialPort serial;
    if (!serial.open(serCfg)) {
        LOG_WARN << "串口不可用, 将在无串口输出的调试模式下运行";
    }

    // ── 初始化队列 ────────────────────────────────────────────
    SafeQueue<cv::Mat>                     frameQueue(2);
    SafeQueue<std::vector<ColoredObject>>  resultQueue(4);

    // ── 启动线程 ──────────────────────────────────────────────
    std::thread camThread([&]() {
        LOG_INFO << "[采集] 线程已启动";

        cv::Mat frame;
        int frameCount = 0;
        auto t0 = std::chrono::steady_clock::now();

        while (g_running && camera.isOpened()) {
            if (!camera.read(frame) || frame.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            frameQueue.push(frame.clone());

            if (++frameCount % 100 == 0) {
                auto t1 = std::chrono::steady_clock::now();
                double fps = 100.0 / std::chrono::duration<double>(t1 - t0).count();
                LOG_INFO << "[采集] FPS: " << fps
                         << " (队列: " << frameQueue.size() << ")";
                t0 = t1;
            }
        }
        frameQueue.shutdown();
        LOG_INFO << "[采集] 线程已停止";
    });

    std::thread detThread(detectionThread,
                          std::ref(yolo), std::ref(hsv), std::ref(roi),
                          std::ref(frameQueue), std::ref(resultQueue),
                          cfg.detect.skipFrames, std::cref(cfg.color));

    std::thread serThread(serialThread,
                          std::ref(serial), std::ref(resultQueue),
                          static_cast<float>(cfg.camera.width),
                          static_cast<float>(cfg.camera.height));

    // ── 主循环: 状态监控 ──────────────────────────────────────
    LOG_INFO << "流水线运行中。按 Ctrl+C 停止。";

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        LOG_DEBUG << "状态 — 摄像头: " << camera.getActualFPS() << "fps"
                  << " | 推理: " << yolo.getAvgInferenceMs() << "ms"
                  << " | 队列: 帧=" << frameQueue.size()
                  << " 结果=" << resultQueue.size();
    }

    // ── 关闭流程 ──────────────────────────────────────────────
    LOG_INFO << "正在关闭...";

    frameQueue.shutdown();
    resultQueue.shutdown();

    if (camThread.joinable()) camThread.join();
    if (detThread.joinable()) detThread.join();
    if (serThread.joinable()) serThread.join();

    camera.release();
    serial.close();

    LOG_INFO << "关闭完成。";
    return 0;
}
