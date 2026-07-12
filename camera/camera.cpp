#include "camera/camera.h"
#include <iostream>
#include <thread>

Camera::~Camera() {
    release();
}

Camera::Camera(Camera&& other) noexcept
    : cap_(std::move(other.cap_))
    , config_(other.config_)
    , lastFrameTime_(other.lastFrameTime_)
{
    actualFPS_.store(other.actualFPS_.load());
}

Camera& Camera::operator=(Camera&& other) noexcept {
    if (this != &other) {
        release();
        cap_ = std::move(other.cap_);
        config_ = other.config_;
        lastFrameTime_ = other.lastFrameTime_;
        actualFPS_.store(other.actualFPS_.load());
    }
    return *this;
}

bool Camera::open(const Config& config) {
    config_ = config;

    // 尝试打开摄像头
    bool ok = cap_.open(config_.deviceId, config_.apiPreference);
    if (!ok) {
        // 回退: 尝试不使用显式 API 偏好
        std::cerr << "[摄像头] 警告: V4L2 API 打开失败, 尝试默认方式..." << std::endl;
        ok = cap_.open(config_.deviceId);
    }
    if (!ok) {
        std::cerr << "[摄像头] 错误: 无法打开设备 /dev/video"
                  << config_.deviceId << std::endl;
        return false;
    }

    // 设置像素格式 (FourCC)
    if (!config_.fourcc.empty()) {
        int fourcc = cv::VideoWriter::fourcc(
            config_.fourcc[0], config_.fourcc[1],
            config_.fourcc[2], config_.fourcc[3]);
        cap_.set(cv::CAP_PROP_FOURCC, fourcc);
    }

    // 设置分辨率
    cap_.set(cv::CAP_PROP_FRAME_WIDTH,  config_.width);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, config_.height);
    cap_.set(cv::CAP_PROP_FPS,          config_.fps);

    // V4L2 缓冲区大小 (越小延迟越低)
    cap_.set(cv::CAP_PROP_BUFFERSIZE,   config_.bufferSize);

    // 应用后端特定设置
    applyV4L2Settings();

    // 读取实际值（摄像头可能调整参数）
    int actualWidth  = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
    int actualHeight = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
    int actualFPS    = static_cast<int>(cap_.get(cv::CAP_PROP_FPS));

    std::cout << "[摄像头] 已打开 /dev/video" << config_.deviceId
              << " " << actualWidth << "x" << actualHeight
              << " @ " << actualFPS << " FPS"
              << " (" << config_.fourcc << ")" << std::endl;

    lastFrameTime_ = std::chrono::steady_clock::now();
    return true;
}

bool Camera::read(cv::Mat& frame) {
    if (!cap_.isOpened()) return false;

    bool ok = cap_.read(frame);
    if (!ok || frame.empty()) return false;

    // 计算实际 FPS
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - lastFrameTime_).count();
    lastFrameTime_ = now;

    if (elapsed > 0.0) {
        double instantFPS = 1.0 / elapsed;
        // 指数移动平均 (平滑因子 0.1)
        actualFPS_.store(actualFPS_.load() * 0.9 + instantFPS * 0.1);
    }

    return true;
}

void Camera::release() {
    if (cap_.isOpened()) {
        cap_.release();
        std::cout << "[摄像头] 已释放。" << std::endl;
    }
}

double Camera::getActualFPS() const {
    return actualFPS_.load();
}

double Camera::getProperty(int propId) const {
    return cap_.get(propId);
}

bool Camera::setProperty(int propId, double value) {
    return cap_.set(propId, value);
}

bool Camera::applyV4L2Settings() {
    if (!cap_.isOpened()) return false;

    // 如果请求了手动曝光
    if (config_.exposure >= 0) {
        // 先关闭自动曝光 (V4L2_CID_EXPOSURE_AUTO = 1 表示手动模式)
        cap_.set(cv::CAP_PROP_AUTO_EXPOSURE, 1.0);  // 手动模式
        cap_.set(cv::CAP_PROP_EXPOSURE, static_cast<double>(config_.exposure));
    }

    // 更多 V4L2 优化可通过 cv::CAP_PROP_* 或
    // 直接对文件描述符进行 ioctl 调用来实现高级调优。

    return true;
}
