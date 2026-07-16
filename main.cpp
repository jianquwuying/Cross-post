/// @file main.cpp
/// @brief 最终视觉流水线 — Camera → YOLO → DetectionFilter → HSV → PacketEncoder → UART。
///
/// 数据流:
///   Camera → frameQueue → detectionThread:
///     ROI → YOLO → DetectionFilter → {Material→HSV, ColorRing→HSV, Obstacle→skip}
///     → DetectionResult → resultQueue → serialThread:
///       PacketEncoder → 9字节二进制包 → UART TX
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
#include "pipeline/detection_filter.h"
#include "pipeline/packet_encoder.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// ═══════════════════════════════════════════════════════════════════
//  SafeQueue — 线程安全有界队列
// ═══════════════════════════════════════════════════════════════════

template<typename T>
class SafeQueue {
public:
    explicit SafeQueue(size_t maxSize = 4) : maxSize_(maxSize) {}
    void push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        fullCV_.wait(lock, [this]{ return queue_.size() < maxSize_ || !running_; });
        if (!running_) return;
        queue_.push(std::move(item));
        emptyCV_.notify_one();
    }
    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        emptyCV_.wait(lock, [this]{ return !queue_.empty() || !running_; });
        if (!running_ && queue_.empty()) return T{};
        T item = std::move(queue_.front()); queue_.pop();
        fullCV_.notify_one();
        return item;
    }
    void shutdown() {
        { std::lock_guard<std::mutex> lock(mutex_); running_ = false; }
        fullCV_.notify_all(); emptyCV_.notify_all();
    }
    size_t size() const { std::lock_guard<std::mutex> lock(mutex_); return queue_.size(); }
    bool isRunning() const { return running_; }
private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable emptyCV_, fullCV_;
    size_t maxSize_;
    std::atomic<bool> running_{true};
};

// ═══════════════════════════════════════════════════════════════════
//  全局状态
// ═══════════════════════════════════════════════════════════════════

static std::atomic<bool> g_running{true};
static void signalHandler(int) { g_running = false; LOG_INFO << "收到退出信号, 正在关闭..."; }

// ═══════════════════════════════════════════════════════════════════
//  检测线程
// ═══════════════════════════════════════════════════════════════════

void detectionThread(
    YOLO26Detector& yolo, HSVClassifier& hsv, RoiCrop& roi,
    DetectionFilter& filter,
    SafeQueue<cv::Mat>& frameQueue,
    SafeQueue<std::vector<DetectionResult>>& resultQueue,
    int skipFrames)
{
    LOG_INFO << "[检测] 线程已启动 (YOLO + Filter + HSV)";

    int frameCount = 0;
    auto t0 = std::chrono::steady_clock::now();

    while (g_running && frameQueue.isRunning()) {
        cv::Mat frame = frameQueue.pop();
        if (frame.empty()) continue;

        if (skipFrames > 0 && frameCount % (skipFrames + 1) != 0) { ++frameCount; continue; }

        // ── ROI ──────────────────────────────────────────────
        cv::Mat processedFrame = roi.isEnabled() ? roi.apply(frame) : frame;

        // ── YOLO ─────────────────────────────────────────────
        std::vector<Object> rawObjects;
        if (!yolo.detect(processedFrame, rawObjects)) { resultQueue.push({}); continue; }

        // ── Detection Filter ─────────────────────────────────
        auto filtered = filter.apply(rawObjects);

        // ── HSV + DetectionResult ────────────────────────────
        std::vector<DetectionResult> results;
        results.reserve(filtered.size());

        for (auto& obj : filtered) {
            DetectionResult dr;

            // 坐标映射
            float x1 = obj.x1(), y1 = obj.y1(), w = obj.w(), h = obj.h();
            if (roi.isEnabled()) {
                auto mapped = roi.mapBack(obj.rect);
                x1 = mapped.x; y1 = mapped.y; w = mapped.width; h = mapped.height;
            }
            dr.cx = static_cast<int>(x1 + w * 0.5f);
            dr.cy = static_cast<int>(y1 + h * 0.5f);
            dr.confidence = obj.confidence;

            // 三目标分支
            switch (obj.label) {
                case 0: {  // Material — 全色 HSV
                    dr.target = protocol::TargetType::Material;
                    cv::Rect roiR(
                        std::max(0, static_cast<int>(x1)),
                        std::max(0, static_cast<int>(y1)),
                        std::min(static_cast<int>(w), processedFrame.cols),
                        std::min(static_cast<int>(h), processedFrame.rows));
                    if (roiR.width > 0 && roiR.height > 0) {
                        ColorResult cr = hsv.classify(processedFrame(roiR));
                        dr.color = static_cast<protocol::ColorType>(cr.color);
                        dr.colorConfidence = cr.confidence;
                    }
                    break;
                }
                case 1: {  // ColorRing — 仅 RED/BLUE/GREEN
                    dr.target = protocol::TargetType::ColorRing;
                    cv::Rect roiR(
                        std::max(0, static_cast<int>(x1)),
                        std::max(0, static_cast<int>(y1)),
                        std::min(static_cast<int>(w), processedFrame.cols),
                        std::min(static_cast<int>(h), processedFrame.rows));
                    if (roiR.width > 0 && roiR.height > 0) {
                        ColorResult cr = hsv.classify(processedFrame(roiR));
                        auto pc = static_cast<protocol::ColorType>(cr.color);
                        if (protocol::isValidColorRingColor(pc)) {
                            dr.color = pc;
                            dr.colorConfidence = cr.confidence;
                        }
                    }
                    break;
                }
                case 2: {  // Obstacle — 跳过 HSV
                    dr.target = protocol::TargetType::Obstacle;
                    dr.color  = protocol::ColorType::NONE;
                    dr.colorConfidence = 0.0f;
                    break;
                }
                default: continue;  // 未知类别跳过
            }
            results.push_back(dr);
        }

        resultQueue.push(std::move(results));

        if (++frameCount % 50 == 0) {
            auto t1 = std::chrono::steady_clock::now();
            double fps = 50.0 / std::chrono::duration<double>(t1 - t0).count();
            LOG_INFO << "[检测] FPS: " << fps << " | 推理: " << yolo.getAvgInferenceMs()
                     << "ms | 原始: " << rawObjects.size()
                     << " → 过滤: " << filtered.size();
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
                  SafeQueue<std::vector<DetectionResult>>& resultQueue)
{
    LOG_INFO << "[串口] 线程已启动 (9字节协议)";

    int frameCount = 0;
    int totalSent  = 0;

    while (g_running && resultQueue.isRunning()) {
        auto results = resultQueue.pop();
        if (results.empty() || !serial.isOpen()) continue;

        auto frame = PacketEncoder::encode(results);
        ssize_t written = serial.write(frame);
        if (written < 0) {
            LOG_ERROR << "[串口] 写入失败";
        } else {
            totalSent += static_cast<int>(results.size());
        }

        // 每100帧打印一次统计
        if (++frameCount % 100 == 0) {
            LOG_INFO << "[串口] 帧=" << frameCount
                     << " 累计发送=" << totalSent
                     << " 本次=" << results.size()
                     << " 字节=" << frame.size();
        }
    }
    LOG_INFO << "[串口] 线程已停止, 累计发送 " << totalSent << " 个目标";
}

// ═══════════════════════════════════════════════════════════════════
//  主函数
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::string configPath = "config/vision.yaml";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) configPath = argv[++i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "用法: " << argv[0] << " [选项]\n"
                      << "  --config  PATH  配置文件 (默认: config/vision.yaml)\n"
                      << "  --model   TYPE  模型: fp16 | int8\n"
                      << "  --serial  DEV   串口设备\n"
                      << "  --baud    RATE  波特率\n"
                      << "  --camera  ID    摄像头ID\n"
                      << "  --size    PIX   输入尺寸\n"
                      << "  --threads N     线程数\n"
                      << "  --conf    F     置信度阈值\n"
                      << "  --log-level LVL 日志: debug|info|warn|error\n"
                      << "  --help, -h      帮助\n";
            return 0;
        }
    }

    // ── 加载配置 ──────────────────────────────────────────────
    AppConfig cfg = AppConfig::load(configPath);
    cfg.applyCliOverrides(argc, argv);

    Logger::setLevel(
        cfg.log.level == "debug" ? LogLevel::DEBUG :
        cfg.log.level == "warn"  ? LogLevel::WARN  :
        cfg.log.level == "error" ? LogLevel::ERROR : LogLevel::INFO);
    Logger::setUseColor(cfg.log.useColor);

    std::cout << "\n╔══════════════════════════════════════════════════╗\n";
    std::cout << "║  视觉识别系统 — YOLO26n + Detection Filter      ║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n";
    cfg.print();

    // ── YOLO ──────────────────────────────────────────────────
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
    if (!yolo.load(ycfg)) { LOG_ERROR << "YOLO 模型加载失败"; return 1; }

    // ── Detection Filter ──────────────────────────────────────
    DetectFilterConfig dfCfg;
    dfCfg.confidenceThreshold = cfg.detect.confThreshold;
    dfCfg.maxTargets          = cfg.detect.maxTargets;
    dfCfg.allowedClasses      = cfg.detect.allowedClasses;
    if (cfg.roi.enabled) {
        dfCfg.roiEnabled = true;
        dfCfg.roiX = cfg.roi.x; dfCfg.roiY = cfg.roi.y;
        dfCfg.roiW = cfg.roi.width; dfCfg.roiH = cfg.roi.height;
    }
    DetectionFilter filter;
    filter.configure(dfCfg);
    LOG_INFO << "[Filter] 置信度≥" << dfCfg.confidenceThreshold
             << " TopK=" << dfCfg.maxTargets;

    // ── HSV ───────────────────────────────────────────────────
    HSVClassifier::Config hsvCfg;
    hsvCfg.colorThreshold     = cfg.color.threshold;
    hsvCfg.colorConfThreshold = cfg.color.confThreshold;
    hsvCfg.morphKernelSize    = cfg.color.morphKernel;
    hsvCfg.blurKernelSize     = cfg.color.blurKernel;
    auto toScalar = [](const AppConfig::HSVRangeCfg& r) { return cv::Scalar(r.hLow, r.sLow, r.vLow); };
    auto toScalarH = [](const AppConfig::HSVRangeCfg& r) { return cv::Scalar(r.hHigh, r.sHigh, r.vHigh); };
    hsvCfg.red1   = { toScalar(cfg.color.red1),   toScalarH(cfg.color.red1) };
    hsvCfg.red2   = { toScalar(cfg.color.red2),   toScalarH(cfg.color.red2) };
    hsvCfg.yellow = { toScalar(cfg.color.yellow), toScalarH(cfg.color.yellow) };
    hsvCfg.blue   = { toScalar(cfg.color.blue),   toScalarH(cfg.color.blue) };
    // green/black/cyan use defaults from hsv.h (not yet in AppConfig)
    HSVClassifier hsv(hsvCfg);

    // ── ROI ───────────────────────────────────────────────────
    RoiCrop roi;
    if (cfg.roi.enabled) {
        RoiConfig rc; rc.enabled = true;
        rc.x = cfg.roi.x; rc.y = cfg.roi.y;
        rc.width = cfg.roi.width; rc.height = cfg.roi.height;
        roi.configure(rc);
    }

    // ── Camera (直接使用 AppConfig 中的 CameraConfig) ──────────
    Camera camera;
    if (!camera.open(cfg.camera)) { LOG_ERROR << "摄像头打开失败"; return 1; }

    // ── Serial ────────────────────────────────────────────────
    SerialPort::Config serCfg;
    serCfg.device   = cfg.serial.device;
    serCfg.baudrate = cfg.serial.baudrate;
    serCfg.dataBits = cfg.serial.dataBits;
    serCfg.stopBits = cfg.serial.stopBits;
    serCfg.parity   = cfg.serial.parity[0];
    SerialPort serial;
    if (!serial.open(serCfg)) {
        LOG_WARN << "串口不可用, 调试模式运行 (无输出)";
    }

    // ── Queues ────────────────────────────────────────────────
    SafeQueue<cv::Mat>                    frameQueue(2);
    SafeQueue<std::vector<DetectionResult>> resultQueue(4);

    // ── Threads ───────────────────────────────────────────────
    std::thread camThread([&]() {
        cv::Mat frame; int n = 0; auto t0 = std::chrono::steady_clock::now();
        while (g_running && camera.isOpened()) {
            if (!camera.read(frame) || frame.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            frameQueue.push(frame.clone());
            if (++n % 100 == 0) {
                auto t1 = std::chrono::steady_clock::now();
                LOG_INFO << "[采集] FPS: " << (100.0 / std::chrono::duration<double>(t1 - t0).count()) << " (队列: " << frameQueue.size() << ")";
                t0 = t1;
            }
        }
        frameQueue.shutdown();
    });

    std::thread detThread(detectionThread, std::ref(yolo), std::ref(hsv),
                          std::ref(roi), std::ref(filter),
                          std::ref(frameQueue), std::ref(resultQueue), cfg.detect.skipFrames);

    std::thread serThread(serialThread, std::ref(serial), std::ref(resultQueue));

    // ── Main loop ─────────────────────────────────────────────
    LOG_INFO << "流水线运行中。Ctrl+C 停止。";
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        LOG_DEBUG << "状态 | 摄像头: " << camera.getActualFPS() << "fps"
                  << " | 推理: " << yolo.getAvgInferenceMs() << "ms"
                  << " | 队列: 帧=" << frameQueue.size() << " 结果=" << resultQueue.size();
    }

    LOG_INFO << "正在关闭...";
    frameQueue.shutdown(); resultQueue.shutdown();
    if (camThread.joinable()) camThread.join();
    if (detThread.joinable()) detThread.join();
    if (serThread.joinable()) serThread.join();
    camera.release(); serial.close();
    LOG_INFO << "关闭完成。";
    return 0;
}
