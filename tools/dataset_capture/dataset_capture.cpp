/// @file dataset_capture.cpp
/// @brief DatasetCapture 完整实现 — 使用 Camera 类 + Logger 日志。

#include "dataset_capture.h"
#include "logger/logger.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <cstdlib>

// ═══════════════════════════════════════════════════════════════════

DatasetCapture::~DatasetCapture() {
    running_ = false;
    camera_.release();
    cv::destroyAllWindows();
}

// ── 初始化 ────────────────────────────────────────────────────────

bool DatasetCapture::Init(const std::string& yamlPath) {
    // 1. 加载 YAML
    YAML::Node root;
    try {
        root = YAML::LoadFile(yamlPath);
    } catch (const YAML::Exception& e) {
        LOG_ERROR << "YAML 解析失败: " << e.what();
        return false;
    }

    // 2. 解析摄像头配置 (严格, 缺失字段直接 exit(1))
    if (!root["camera"].IsDefined()) {
        LOG_ERROR << "配置缺少 'camera' 段";
        return false;
    }
    CameraConfig camCfg = CameraConfig::fromYaml(root["camera"]);

    // 3. 加载采集/显示配置
    cfg_ = CaptureConfig::load(yamlPath);

    LOG_INFO << "保存目录: " << cfg_.saveDir
             << "  间隔: " << cfg_.captureIntervalMs << "ms";

    // 4. 打开摄像头 (Camera 类会自动打印参数表格)
    if (!camera_.open(camCfg)) {
        LOG_ERROR << "无法打开摄像头 /dev/video" << camCfg.device;
        return false;
    }

    // 5. 创建保存目录
    std::error_code ec;
    if (!std::filesystem::exists(cfg_.saveDir)) {
        std::filesystem::create_directories(cfg_.saveDir, ec);
        if (ec) {
            LOG_ERROR << "无法创建目录: " << cfg_.saveDir
                      << " (" << ec.message() << ")";
            return false;
        }
        LOG_INFO << "目录已创建: " << cfg_.saveDir;
    }

    // 6. 扫描已有图片编号
    ScanImageIndex();
    LOG_INFO << "下一张编号: " << nextIndex_;

    // 7. 初始化计时器
    lastSaveTime_ = std::chrono::steady_clock::now();

    if (cfg_.captureEnable) {
        LOG_INFO << "自动采集已启用, 间隔 " << cfg_.captureIntervalMs << "ms";
    }

    return true;
}

// ── 主循环 ──────────────────────────────────────────────────────

void DatasetCapture::Run() {
    cv::Mat frame;

    if (cfg_.showWindow) {
        cv::namedWindow("Dataset Capture", cv::WINDOW_AUTOSIZE);
    }

    LOG_INFO << "运行中... ESC=退出 SPACE=保存 S=暂停/恢复 R=重新扫描";

    while (running_) {
        // ── 读取帧 ──────────────────────────────────────────
        if (!camera_.read(frame) || frame.empty()) {
            LOG_ERROR << "摄像头读取失败, 退出。";
            break;
        }

        // ── 自动保存 ─────────────────────────────────────────
        auto now = std::chrono::steady_clock::now();
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
                    LOG_WARN << "自动采集已暂停。";
                } else {
                    LOG_INFO << "自动采集已恢复。";
                    lastSaveTime_ = std::chrono::steady_clock::now();
                }
                break;
            case 'r':
            case 'R':
                ScanImageIndex();
                LOG_INFO << "已重新扫描, 下一张编号: " << nextIndex_;
                break;
        }
    }

    LOG_INFO << "采集结束。共保存 " << savedCount_ << " 张图片。";

    camera_.release();
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
        LOG_INFO << "已保存: " << fullPath;
    } else {
        LOG_ERROR << "保存失败: " << fullPath;
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
        ss << "FPS: " << static_cast<int>(camera_.getActualFPS());
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
