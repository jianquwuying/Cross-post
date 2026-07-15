/// @file camera.h
/// @brief 基于 OpenCV VideoCapture 的 V4L2 摄像头采集封装。
///
/// 针对 Raspberry Pi 5 设计，支持 USB 或 CSI 摄像头。
/// 支持 MJPEG 和 YUYV 像素格式，可配置分辨率和帧率。
/// 所有摄像头参数完全由 YAML 配置文件控制，无硬编码默认值。

#pragma once

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <yaml-cpp/yaml.h>

#include <string>
#include <chrono>
#include <atomic>

// ── 摄像头统一配置（无默认值，全部来自 YAML）───────────────────────

struct CameraConfig {
    int         device;                ///< /dev/videoX 设备索引
    std::string backend;               ///< "V4L2" | "ANY" | "DSHOW"
    int         width;                 ///< 采集宽度
    int         height;                ///< 采集高度
    int         fps;                   ///< 目标帧率
    std::string fourcc;                ///< 像素格式: "MJPG" | "YUYV"
    int         buffer_size;           ///< V4L2 缓冲区数量 (1 = 最低延迟)
    bool        auto_exposure;         ///< true = 自动曝光, false = 手动
    int         exposure;              ///< 手动曝光值 (仅 auto_exposure=false 时生效)
    bool        auto_white_balance;    ///< true = 自动白平衡, false = 手动
    int         white_balance;         ///< 白平衡色温 / Kelvin (仅 auto_wb=false 时生效)

    /// 从 YAML 节点严格解析（缺失字段直接 exit(1)）
    static CameraConfig fromYaml(const YAML::Node& node);
};

// ── 摄像头采集类 ────────────────────────────────────────────────────

class Camera {
public:
    Camera() = default;
    ~Camera();

    // ── 不可拷贝，可移动 ────────────────────────────────────────
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;
    Camera(Camera&&) noexcept;
    Camera& operator=(Camera&&) noexcept;

    /// 使用指定配置打开摄像头（必须提供完整配置，无默认值）
    bool open(const CameraConfig& config);

    /// 检查摄像头是否成功打开
    bool isOpened() const { return cap_.isOpened(); }

    /// 采集一帧图像（阻塞）
    /// @return 是否成功采集到帧
    bool read(cv::Mat& frame);

    /// 释放摄像头资源
    void release();

    /// 获取当前实测 FPS（EMA 平滑）
    double getActualFPS() const;

    /// 获取摄像头属性
    double getProperty(int propId) const;
    /// 设置摄像头属性
    bool   setProperty(int propId, double value);

    /// 获取当前配置
    const CameraConfig& getConfig() const { return config_; }

private:
    /// 应用曝光控制 (OpenCV CAP_PROP_AUTO_EXPOSURE + CAP_PROP_EXPOSURE)
    void applyExposure(const CameraConfig& cfg);

    /// 应用白平衡控制 (直接 V4L2 ioctl，OpenCV 不支持 CAP_PROP_AUTO_WB)
    void applyWhiteBalance(const CameraConfig& cfg);

    /// 读取实际参数并打印 ANSI 验证表格，不一致时输出 WARN
    void verifyAndLog(const CameraConfig& cfg);

    cv::VideoCapture cap_;
    CameraConfig config_;
    std::chrono::steady_clock::time_point lastFrameTime_;
    mutable std::atomic<double> actualFPS_{0.0};
};
