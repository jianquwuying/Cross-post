// ============================================================================
// CameraCapture.cpp  —  摄像头 / 视频文件捕获实现
// ============================================================================

#include "CameraCapture.h"
#include "ConfigManager.h"
#include "Logger.h"

#include <chrono>
#include <cstdlib>
#include <thread>

// ---------------------------------------------------------------------------
// 打开设备
// ---------------------------------------------------------------------------
bool CameraCapture::open() {
    auto& cfg = ConfigManager::instance();
    std::string video_path = cfg.cameraVideoPath();

    // video_path 非空 → 优先视频文件；否则按索引打开 V4L2 摄像头
    if (!video_path.empty()) {
        LOG_INFO("正在打开视频文件: %s", video_path.c_str());
        cap_.open(video_path);
    } else {
        int idx = cfg.cameraIndex();
        LOG_INFO("正在打开摄像头设备 %d", idx);
        cap_.open(idx, cv::CAP_V4L2);   // Linux 上 UVC 使用 V4L2 后端

        // 设置捕获格式
        cap_.set(cv::CAP_PROP_FOURCC,
                 cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        cap_.set(cv::CAP_PROP_FRAME_WIDTH,  cfg.cameraWidth());
        cap_.set(cv::CAP_PROP_FRAME_HEIGHT, cfg.cameraHeight());
        cap_.set(cv::CAP_PROP_FPS,          cfg.cameraFps());
    }

    if (!cap_.isOpened()) {
        LOG_ERROR("无法打开摄像头 / 视频源");
        return false;
    }

    retry_count_ = 0;
    LOG_INFO("摄像头源已成功打开  (%dx%d)",
             static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH)),
             static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT)));
    return true;
}

// ---------------------------------------------------------------------------
// 释放设备
// ---------------------------------------------------------------------------
void CameraCapture::release() {
    if (cap_.isOpened()) {
        cap_.release();
    }
}

// ---------------------------------------------------------------------------
// 查询打开状态
// ---------------------------------------------------------------------------
bool CameraCapture::isOpened() const {
    return cap_.isOpened();
}

// ---------------------------------------------------------------------------
// 读取帧（含重试逻辑）
// ---------------------------------------------------------------------------
std::optional<cv::Mat> CameraCapture::read() {
    cv::Mat frame;

    // ---- 失败路径①：设备未打开 → 释放、等待 1 s、重新打开 --------------
    if (!cap_.isOpened()) {
        LOG_WARN("摄像头已断开，尝试重连 (%d/%d)",
                 retry_count_ + 1, kMaxRetries);
        release();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kRetrySleepMs));

        if (!open()) {
            retry_count_++;
            if (retry_count_ >= kMaxRetries) {
                LOG_ERROR("摄像头重连失败，已尝试 %d 次，退出",
                          kMaxRetries);
                std::exit(EXIT_FAILURE);
            }
            return std::nullopt;
        }
        retry_count_ = 0;
    }

    // ---- 读取 ----------------------------------------------------------
    try {
        cap_ >> frame;
    } catch (const cv::Exception& e) {
        // 失败路径②：读取异常 → 计数 +1 并释放设备，
        // 下一次调用进入 ① 的重连分支
        LOG_ERROR("摄像头读取 OpenCV 异常: %s", e.what());
        retry_count_++;
        if (retry_count_ >= kMaxRetries) {
            LOG_ERROR("超过最大重试次数，退出");
            std::exit(EXIT_FAILURE);
        }
        release();
        return std::nullopt;
    }

    if (frame.empty()) {
        // 失败路径③：空帧 → 计数 +1，但不 release、不重连，
        // 下一次调用继续从同一设备读取
        LOG_WARN("从摄像头接收到空帧");
        retry_count_++;
        if (retry_count_ >= kMaxRetries) {
            LOG_ERROR("空帧过多 (%d)，退出", kMaxRetries);
            std::exit(EXIT_FAILURE);
        }
        return std::nullopt;
    }

    // 成功 —— 重置重试计数
    retry_count_ = 0;
    return frame;
}

// ---------------------------------------------------------------------------
// 属性透传
// ---------------------------------------------------------------------------
double CameraCapture::getProperty(int prop_id) const {
    return cap_.get(prop_id);
}
