/// @file dataset_capture.cpp
/// @brief DatasetCapture 完整实现 — 图片采集 + 视频录制 + Camera 类 + Logger 日志。
///
/// 图片采集与视频录制共用同一帧, 互不干扰。

#include "dataset_capture.h"
#include "logger/logger.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <ctime>

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════

DatasetCapture::~DatasetCapture() {
    running_ = false;
    ReleaseVideoWriter();
    camera_.release();
    cv::destroyAllWindows();
}

// ── 初始化 ────────────────────────────────────────────────────────

bool DatasetCapture::Init(const YAML::Node& root) {
    // 1. 解析摄像头配置 (严格, 缺失字段直接 exit(1))
    if (!root["camera"].IsDefined()) {
        LOG_ERROR << "配置缺少 'camera' 段";
        return false;
    }
    CameraConfig camCfg = CameraConfig::fromYaml(root["camera"]);

    // 2. 加载采集/显示/视频配置 (共用同一个 YAML::Node, 不重复读文件)
    cfg_ = CaptureConfig::load(root);

    LOG_INFO << "图片保存目录: " << cfg_.saveDir
             << "  间隔: " << cfg_.captureIntervalMs << "ms";

    // 3. 打开摄像头 (Camera 类会自动打印参数表格)
    if (!camera_.open(camCfg)) {
        LOG_ERROR << "无法打开摄像头 /dev/video" << camCfg.device;
        return false;
    }

    // 4. 创建图片保存目录
    std::error_code ec;
    if (!fs::exists(cfg_.saveDir)) {
        fs::create_directories(cfg_.saveDir, ec);
        if (ec) {
            LOG_ERROR << "无法创建图片目录: " << cfg_.saveDir
                      << " (" << ec.message() << ")";
            return false;
        }
        LOG_INFO << "图片目录已创建: " << cfg_.saveDir;
    }

    // 5. 扫描已有图片编号
    ScanImageIndex();
    LOG_INFO << "下一张图片编号: " << nextIndex_;

    // 6. 图片采集状态
    if (cfg_.captureEnable) {
        LOG_INFO << "[图片] 自动采集已启用, 间隔 " << cfg_.captureIntervalMs << "ms";
    } else {
        LOG_INFO << "[图片] 自动采集已禁用 (按 SPACE 手动保存)";
    }

    // 7. 初始化视频录制
    if (cfg_.videoEnable) {
        if (InitVideoWriter(camCfg.width, camCfg.height, camCfg.fps)) {
            LOG_INFO << "[视频] 录制已启用 → " << videoFilePath_;
        } else {
            LOG_ERROR << "[视频] 录制初始化失败, 将仅采集图片";
            cfg_.videoEnable = false;
        }
    } else {
        LOG_INFO << "[视频] 录制已禁用";
    }

    // 8. 初始化计时器
    lastSaveTime_ = std::chrono::steady_clock::now();

    return true;
}

// ── 初始化视频录制器 ──────────────────────────────────────────────

bool DatasetCapture::InitVideoWriter(int width, int height, int fps) {
    // 1. 创建目录
    std::error_code ec;
    if (!fs::exists(cfg_.videoSavePath)) {
        fs::create_directories(cfg_.videoSavePath, ec);
        if (ec) {
            LOG_ERROR << "无法创建视频目录: " << cfg_.videoSavePath
                      << " (" << ec.message() << ")";
            return false;
        }
        LOG_INFO << "视频目录已创建: " << cfg_.videoSavePath;
    }

    // 2. 生成文件名
    videoFilePath_ = cfg_.videoSavePath + "/" + GenerateVideoFileName();

    // 3. 解析 FourCC
    int fourcc = 0;
    if (cfg_.videoCodec.length() == 4) {
        fourcc = cv::VideoWriter::fourcc(
            cfg_.videoCodec[0], cfg_.videoCodec[1],
            cfg_.videoCodec[2], cfg_.videoCodec[3]);
    } else {
        LOG_ERROR << "视频编码格式无效 (需4字符): " << cfg_.videoCodec;
        return false;
    }

    // 4. 打开 VideoWriter
    bool ok = videoWriter_.open(videoFilePath_, fourcc, fps,
                                 cv::Size(width, height), true);
    if (!ok) {
        LOG_ERROR << "无法打开 VideoWriter: " << videoFilePath_;
        return false;
    }

    LOG_INFO << "[视频] "
             << width << "x" << height << " @ " << fps << "fps"
             << " " << cfg_.videoCodec;

    return true;
}

// ── 安全释放 VideoWriter ─────────────────────────────────────────

void DatasetCapture::ReleaseVideoWriter() {
    if (videoWriter_.isOpened()) {
        videoWriter_.release();
        if (!videoFilePath_.empty()) {
            LOG_INFO << "[视频] 已保存: " << videoFilePath_;
        }
    }
}

// ── 主循环 ──────────────────────────────────────────────────────

void DatasetCapture::Run() {
    cv::Mat frame;

    if (cfg_.showWindow) {
        cv::namedWindow("Dataset Capture", cv::WINDOW_AUTOSIZE);
    }

    LOG_INFO << "运行中... ESC=退出 SPACE=保存 S=暂停/恢复 R=重新扫描";

    while (running_) {
        // ── 读取帧 (仅一次, 图片和视频共用) ──────────────────
        if (!camera_.read(frame) || frame.empty()) {
            LOG_ERROR << "摄像头读取失败, 退出。";
            break;
        }

        auto now = std::chrono::steady_clock::now();

        // ── 图片自动保存 ─────────────────────────────────────
        if (cfg_.captureEnable && !paused_) {
            auto sinceSave = std::chrono::duration<double, std::milli>(
                now - lastSaveTime_).count();
            if (sinceSave >= static_cast<double>(cfg_.captureIntervalMs)) {
                SaveFrame(frame);
                lastSaveTime_ = now;
            }
        }

        // ── 视频录制 (与图片并行, 互不影响) ──────────────────
        if (cfg_.videoEnable && videoWriter_.isOpened() && !paused_) {
            videoWriter_.write(frame);
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
                    LOG_WARN << "采集已暂停。";
                } else {
                    LOG_INFO << "采集已恢复。";
                    lastSaveTime_ = std::chrono::steady_clock::now();
                }
                break;
            case 'r':
            case 'R':
                ScanImageIndex();
                LOG_INFO << "已重新扫描, 下一张图片编号: " << nextIndex_;
                break;
        }
    }

    // ── 清理 ──────────────────────────────────────────────────
    LOG_INFO << "采集结束。共保存 " << savedCount_ << " 张图片。";

    ReleaseVideoWriter();
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
        LOG_INFO << "[图片] 已保存: " << filename;
    } else {
        LOG_ERROR << "[图片] 保存失败: " << fullPath;
    }
}

// ── 绘制叠加信息 ────────────────────────────────────────────────

void DatasetCapture::DrawOverlay(cv::Mat& frame) {
    const int baseX = 10;
    int baseY = 30;
    const int lineH = 22;
    const cv::Scalar green(0, 255, 0);
    const cv::Scalar yellow(0, 255, 255);
    const cv::Scalar red(0, 0, 255);
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
        ss << "Images: " << savedCount_;
        putLine(ss.str(), green);
    }

    {
        std::ostringstream ss;
        ss << "Interval: " << cfg_.captureIntervalMs << "ms";
        putLine(ss.str(), yellow);
    }

    // 视频录制状态指示
    if (cfg_.videoEnable) {
        bool recording = videoWriter_.isOpened() && !paused_;
        std::string recStatus = recording ? "REC" : "STBY";
        const cv::Scalar& recColor = recording ? red : yellow;
        putLine("Video: " + recStatus, recColor);
    }

    {
        std::string status;
        if (!running_)       status = "STOPPED";
        else if (paused_)    status = "PAUSED";
        else if (cfg_.captureEnable || cfg_.videoEnable) status = "RUNNING";
        else                 status = "MANUAL";
        putLine("Status: " + status, paused_ ? yellow : green);
    }
}

// ── 扫描已有图片编号 ────────────────────────────────────────────

void DatasetCapture::ScanImageIndex() {
    nextIndex_ = 1;

    if (!fs::exists(cfg_.saveDir)) return;

    int maxIdx = 0;
    for (const auto& entry : fs::directory_iterator(cfg_.saveDir)) {
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

// ── 生成图片文件名 ──────────────────────────────────────────────

std::string DatasetCapture::GenerateFileName() const {
    std::ostringstream ss;
    ss << cfg_.prefix << "_"
       << std::setw(6) << std::setfill('0') << nextIndex_
       << "." << cfg_.format;
    return ss.str();
}

// ── 生成视频文件名 ──────────────────────────────────────────────

std::string DatasetCapture::GenerateVideoFileName() const {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* tm = std::localtime(&tt);

    std::ostringstream ss;
    ss << cfg_.videoFilenamePrefix << "_"
       << std::setw(4) << std::setfill('0') << (tm->tm_year + 1900)
       << std::setw(2) << std::setfill('0') << (tm->tm_mon + 1)
       << std::setw(2) << std::setfill('0') << tm->tm_mday << "_"
       << std::setw(2) << std::setfill('0') << tm->tm_hour
       << std::setw(2) << std::setfill('0') << tm->tm_min
       << std::setw(2) << std::setfill('0') << tm->tm_sec
       << "." << cfg_.videoExtension;
    return ss.str();
}
