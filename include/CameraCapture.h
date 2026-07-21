#pragma once
// ============================================================================
// CameraCapture.h  —  USB UVC 摄像头封装
// ============================================================================
// 支持摄像头索引 (0, 1, …) 或视频文件路径。
// read() 返回 std::optional<cv::Mat> —— 失败时返回 nullopt。
// 重试逻辑：release → sleep 1s → reopen（最多连续失败 5 次）。
// 第 5 次连续失败后程序退出。
// ============================================================================

#include <opencv2/core/mat.hpp>
#include <opencv2/videoio.hpp>
#include <optional>
#include <string>

class CameraCapture {
public:
    CameraCapture()  = default;
    ~CameraCapture() = default;

    /// 打开设备或视频文件。
    /// 所有参数从 ConfigManager 读取。
    /// 成功返回 true。
    bool open();

    /// 显式释放捕获设备。
    void release();

    /// 查询打开状态。
    bool isOpened() const;

    /// 读取一帧。
    /// 失败时返回 std::nullopt（空帧、读取错误等）。
    /// 内部实现重试逻辑。
    std::optional<cv::Mat> read();

    /// 透传调用 cv::VideoCapture::get()。
    double getProperty(int prop_id) const;

private:
    cv::VideoCapture cap_;
    int retry_count_ = 0;

    static constexpr int kMaxRetries   = 5;
    static constexpr int kRetrySleepMs = 1000;
};
