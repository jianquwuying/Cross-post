/// @file camera.h
/// @brief 基于 OpenCV VideoCapture 的 V4L2 摄像头采集封装。
///
/// 针对 Raspberry Pi 5 设计，支持 USB 或 CSI 摄像头。
/// 支持 MJPEG 和 YUYV 像素格式，可配置分辨率和帧率。

#pragma once

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <string>
#include <chrono>
#include <atomic>

class Camera {
public:
    /// 摄像头配置
    struct Config {
        int deviceId     = 0;              ///< /dev/videoX 设备索引
        int width        = 640;            ///< 采集宽度
        int height       = 480;            ///< 采集高度
        int fps          = 30;             ///< 目标帧率
        int apiPreference = cv::CAP_V4L2;  ///< V4L2 后端
        /// FourCC 编码: "MJPG" 为 MJPEG（CPU占用低）, "YUYV" 为原始格式
        std::string fourcc = "MJPG";
        /// 摄像头曝光 (V4L2 手动控制, -1 = 自动)
        int exposure     = -1;
        /// V4L2 缓冲区大小 (越大越流畅但延迟越高)
        int bufferSize   = 1;
    };

    Camera() = default;
    ~Camera();

    // ── 不可拷贝，可移动 ────────────────────────────────────────
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;
    Camera(Camera&&) noexcept;
    Camera& operator=(Camera&&) noexcept;

    /// 使用默认配置打开摄像头
    bool open() { return open(Config{}); }
    /// 使用指定配置打开摄像头
    bool open(const Config& config);

    /// 检查摄像头是否成功打开
    bool isOpened() const { return cap_.isOpened(); }

    /// 采集一帧图像（阻塞）
    /// @return 是否成功采集到帧
    bool read(cv::Mat& frame);

    /// 释放摄像头资源
    void release();

    /// 获取当前实测 FPS
    double getActualFPS() const;

    /// 获取摄像头属性
    double getProperty(int propId) const;
    /// 设置摄像头属性
    bool   setProperty(int propId, double value);

    /// 获取当前配置
    const Config& getConfig() const { return config_; }

private:
    /// 应用 V4L2 特定设置（曝光、缓冲区、FourCC）
    bool applyV4L2Settings();

    cv::VideoCapture cap_;
    Config config_;
    std::chrono::steady_clock::time_point lastFrameTime_;
    mutable std::atomic<double> actualFPS_{0.0};
};
