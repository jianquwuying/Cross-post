/// @file dataset_capture.cpp
/// @brief DatasetCapture 完整实现。

#include "dataset_capture.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

// ── ANSI 日志宏 (工具内联, 不依赖工程 logger/) ──────────────────

#define DC_INFO()  std::cout << "\033[32m[INFO]\033[0m  "
#define DC_WARN()  std::cout << "\033[33m[WARN]\033[0m  "
#define DC_ERROR() std::cerr << "\033[31m[ERROR]\033[0m "

// ═══════════════════════════════════════════════════════════════════

DatasetCapture::~DatasetCapture() {
    running_ = false;
    if (cap_.isOpened()) {
        cap_.release();
    }
    cv::destroyAllWindows();
}

// ── 初始化 ────────────────────────────────────────────────────────

bool DatasetCapture::Init(const std::string& yamlPath) {
    // 1. 加载配置
    cfg_ = CaptureConfig::load(yamlPath);

    DC_INFO() << "摄像头: /dev/video" << cfg_.cameraDevice
              << " " << cfg_.cameraWidth << "x" << cfg_.cameraHeight
              << " @ " << cfg_.cameraFps << "fps\n";
    DC_INFO() << "保存目录: " << cfg_.saveDir
              << "  间隔: " << cfg_.captureIntervalMs << "ms\n";

    // 2. 打开摄像头
    if (!OpenCamera()) {
        DC_ERROR() << "无法打开摄像头 /dev/video" << cfg_.cameraDevice << "\n";
        return false;
    }

    // 3. 创建保存目录
    std::error_code ec;
    if (!std::filesystem::exists(cfg_.saveDir)) {
        std::filesystem::create_directories(cfg_.saveDir, ec);
        if (ec) {
            DC_ERROR() << "无法创建目录: " << cfg_.saveDir
                        << " (" << ec.message() << ")\n";
            return false;
        }
        DC_INFO() << "目录已创建: " << cfg_.saveDir << "\n";
    }

    // 4. 扫描已有图片编号
    ScanImageIndex();
    DC_INFO() << "下一张编号: " << nextIndex_ << "\n";

    // 5. 初始化计时器
    lastSaveTime_  = std::chrono::steady_clock::now();
    lastFrameTime_ = std::chrono::steady_clock::now();

    if (cfg_.captureEnable) {
        DC_INFO() << "自动采集已启用, 间隔 " << cfg_.captureIntervalMs << "ms\n";
    }

    return true;
}

// ── 打开摄像头 ──────────────────────────────────────────────────

bool DatasetCapture::OpenCamera() {
    bool ok = cap_.open(cfg_.cameraDevice, cv::CAP_V4L2);
    if (!ok) {
        ok = cap_.open(cfg_.cameraDevice);  // 回退默认后端
    }
    if (!ok) return false;

    cap_.set(cv::CAP_PROP_FRAME_WIDTH,  cfg_.cameraWidth);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, cfg_.cameraHeight);
    cap_.set(cv::CAP_PROP_FPS,          cfg_.cameraFps);

    DC_INFO() << "摄像头已打开。\n";
    return true;
}

// ── 主循环 ──────────────────────────────────────────────────────

void DatasetCapture::Run() {
    cv::Mat frame;

    if (cfg_.showWindow) {
        cv::namedWindow("Dataset Capture", cv::WINDOW_AUTOSIZE);
    }

    DC_INFO() << "运行中... ESC=退出 SPACE=保存 S=暂停/恢复 R=重新扫描\n";

    while (running_) {
        // ── 读取帧 ──────────────────────────────────────────
        if (!cap_.read(frame) || frame.empty()) {
            DC_ERROR() << "摄像头读取失败, 退出。\n";
            break;
        }

        // ── 计算 FPS ────────────────────────────────────────
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - lastFrameTime_).count();
        if (elapsed > 0.0) {
            instantFps_ = instantFps_ * 0.9 + (1.0 / elapsed) * 0.1;
        }
        lastFrameTime_ = now;

        // ── 自动保存 ─────────────────────────────────────────
        if (cfg_.captureEnable && !paused_) {
            auto sinceSave = std::chrono::duration<double, std::milli>(
                now - lastSaveTime_).count();
            if (sinceSave >= static_cast<double>(cfg_.captureIntervalMs)) {
                SaveFrame(frame);
                lastSaveTime_ = now;
            }
        }

        // ── 绘制叠加信息 ────────────────────────────────────
        DrawOverlay(frame);

        // ── 显示 ─────────────────────────────────────────────
        if (cfg_.showWindow) {
            cv::imshow("Dataset Capture", frame);
        }

        // ── 快捷键 ──────────────────────────────────────────
        int key = cv::waitKey(1) & 0xFF;
        switch (key) {
            case 27:  // ESC
                running_ = false;
                break;
            case ' ':  // SPACE: 立即保存一张
                SaveFrame(frame);
                lastSaveTime_ = std::chrono::steady_clock::now();
                break;
            case 's':
            case 'S':
                paused_ = !paused_;
                if (paused_) {
                    DC_WARN() << "自动采集已暂停。\n";
                } else {
                    DC_INFO() << "自动采集已恢复。\n";
                    lastSaveTime_ = std::chrono::steady_clock::now();
                }
                break;
            case 'r':
            case 'R':
                ScanImageIndex();
                DC_INFO() << "已重新扫描, 下一张编号: " << nextIndex_ << "\n";
                break;
        }
    }

    DC_INFO() << "采集结束。共保存 " << savedCount_ << " 张图片。\n";

    if (cap_.isOpened()) cap_.release();
    cv::destroyAllWindows();
}

// ── 保存帧 ──────────────────────────────────────────────────────

void DatasetCapture::SaveFrame(const cv::Mat& frame) {
    // clone 确保数据独立 (为未来多线程预留)
    cv::Mat saveMat = frame.clone();

    std::string filename = GenerateFileName();
    std::string fullPath = cfg_.saveDir + "/" + filename;

    std::vector<int> params;
    if (cfg_.format == "jpg" || cfg_.format == "jpeg") {
        params = { cv::IMWRITE_JPEG_QUALITY, cfg_.jpegQuality };
    }

    bool ok = cv::imwrite(fullPath, saveMat, params);
    if (ok) {
        ++savedCount_;
        ++nextIndex_;
        DC_INFO() << "已保存: " << fullPath << "\n";
    } else {
        DC_ERROR() << "保存失败: " << fullPath << "\n";
    }
}

// ── 绘制叠加信息 ────────────────────────────────────────────────

void DatasetCapture::DrawOverlay(cv::Mat& frame) {
    const int baseX = 10;
    int baseY = 30;
    const int lineH = 22;
    const cv::Scalar green(0, 255, 0);
    const cv::Scalar yellow(0, 255, 255);
    const double fontScale = 0.6;
    const int thickness = 2;

    auto putLine = [&](const std::string& text, const cv::Scalar& color) {
        cv::putText(frame, text, cv::Point(baseX, baseY),
                    cv::FONT_HERSHEY_SIMPLEX, fontScale, color, thickness);
        baseY += lineH;
    };

    if (cfg_.showFps) {
        std::ostringstream ss;
        ss << "FPS: " << static_cast<int>(instantFps_);
        putLine(ss.str(), green);
    }

    if (cfg_.showSavedCount) {
        std::ostringstream ss;
        ss << "Saved: " << savedCount_;
        putLine(ss.str(), green);
    }

    {
        std::ostringstream ss;
        ss << "Interval: " << cfg_.captureIntervalMs << "ms";
        putLine(ss.str(), yellow);
    }

    {
        std::string status;
        if (!running_)       status = "STOPPED";
        else if (paused_)    status = "PAUSED";
        else if (cfg_.captureEnable) status = "RUNNING";
        else                 status = "MANUAL";
        putLine("Status: " + status, paused_ ? yellow : green);
    }
}

// ── 扫描已有图片编号 ────────────────────────────────────────────

void DatasetCapture::ScanImageIndex() {
    nextIndex_ = 1;

    if (!std::filesystem::exists(cfg_.saveDir)) return;

    int maxIdx = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cfg_.saveDir)) {
        if (!entry.is_regular_file()) continue;

        std::string name = entry.path().filename().string();
        // 匹配 prefix_NNNNNN.format
        if (name.find(cfg_.prefix + "_") != 0) continue;
        if (name.find("." + cfg_.format) == std::string::npos) continue;

        // 提取数字部分
        std::string numStr = name.substr(cfg_.prefix.size() + 1,
            name.size() - cfg_.prefix.size() - 1 - cfg_.format.size());
        try {
            int idx = std::stoi(numStr);
            if (idx > maxIdx) maxIdx = idx;
        } catch (...) {
            // 忽略无法解析的文件名
        }
    }

    nextIndex_ = maxIdx + 1;
}

// ── 生成文件名 ──────────────────────────────────────────────────

std::string DatasetCapture::GenerateFileName() const {
    std::ostringstream ss;
    ss << cfg_.prefix << "_"
       << std::setw(6) << std::setfill('0') << nextIndex_
       << "." << cfg_.format;
    return ss.str();
}
