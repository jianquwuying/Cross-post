#pragma once
// ============================================================================
// CameraCapture.h  —  USB UVC 摄像头 / 视频文件封装
// ============================================================================
// 输入源选择：camera.video_path 非空时优先打开视频文件；否则按 camera.index
// 以 V4L2 后端打开 UVC 摄像头，并设置 MJPG、分辨率与帧率。
//
// read() 返回 std::optional<cv::Mat>，三条失败路径：
//   ① 设备未打开 → release → sleep 1 s → open() 重连，open 失败则计数 +1
//   ② 读取抛 cv::Exception → 计数 +1 并 release()，下一轮走 ① 的重连分支
//   ③ 读出空帧 → 计数 +1，不 release、不重连，下一轮继续读取
// 失败计数达到 kMaxRetries（5 次）时进程退出；open() 成功或任一帧读取成功
// 都会清零计数。读取异常会立即 release，下一次调用先重连，重连成功即清零，
// 因此单纯连续读异常不会累计到 5 次。
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
    /// 成功返回图像；失败返回 std::nullopt，由调用方在下一轮继续调用。
    /// 内部按三条失败路径计数重试；open() 或成功读帧均清零计数，
    /// 只有重连失败/空帧的计数持续累积到 kMaxRetries 时退出进程。
    std::optional<cv::Mat> read();

    /// 透传调用 cv::VideoCapture::get()。
    double getProperty(int prop_id) const;

private:
    cv::VideoCapture cap_;
    int retry_count_ = 0;   // 连续失败计数（成功读取后清零）

    static constexpr int kMaxRetries   = 5;      // 连续失败上限（第 5 次触发退出）
    static constexpr int kRetrySleepMs = 1000;   // 重连前等待（毫秒）
};
