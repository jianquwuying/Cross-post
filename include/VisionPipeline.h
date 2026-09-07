#pragma once
// ============================================================================
// VisionPipeline.h  —  视觉算法调度 + Kalman 滤波器管理
// ============================================================================
// 与帧数据无关的无状态设计；仅持有 Kalman 滤波器状态。
//
// process() 按当前模式分发：
//   模式0  —  6 颜色物块检测
//   模式1  —  单圆环数字识别（Hu矩 + matchShapes 模板匹配）
//   模式2  —  黑色圆柱检测
//   模式3  —  空闲（不检测、不发送）
//
// Kalman 滤波器：2-D [x, y]，恒等转移矩阵，**无速度模型**。
// 标量过程/测量噪声从 YAML 读取 → eye(2,2) * scalar。
// ============================================================================

#include "ConfigManager.h"
#include "ThreadSafeSendQueue.h"  // DetectionResult

#include <opencv2/core/mat.hpp>
#include <opencv2/video/tracking.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

class VisionPipeline {
public:
    VisionPipeline();
    ~VisionPipeline() = default;

    // -------------------------------------------------------------------
    // 模式控制
    // -------------------------------------------------------------------

    /// 设置当前模式（0–3）。由 SerialThread 调用。
    /// 模式变化时，排队一次完整的 Kalman 重置。
    void setMode(uint8_t mode);

    /// 读取当前模式。由 ProcessThread 调用。
    uint8_t currentMode() const { return current_mode_.load(); }

    // -------------------------------------------------------------------
    // 帧处理
    // -------------------------------------------------------------------

    /// 处理一帧。返回本帧所有有效检测结果。
    /// 由 ProcessThread 调用。线程安全（所有状态为内部持有）。
    std::vector<DetectionResult> process(const cv::Mat& frame);

    // -------------------------------------------------------------------
    // Kalman 状态重置（公开，供 main 中模式切换使用）
    // -------------------------------------------------------------------
    void resetKalman();

    /// 返回最近一次处理的二值化调试图像（供 main 中显示）。
    /// 模式0：各颜色掩码合成图  模式1：数字ROI二值化图  模式2：黑色掩码
    cv::Mat getLastDebugImage() const;

private:
    // ---- 每个目标的 Kalman 跟踪器 ------------------------------------
    struct KalmanTracker {
        cv::KalmanFilter kf;
        bool initialized    = false;
        int  stable_count   = 0;       // 当前连续检测帧数
        int  lost_count     = 0;       // 连续丢失帧数
        cv::Point2f last_measurement;
    };

    // ---- Kalman 辅助函数 ----------------------------------------------
    void initKalmanTracker(int key, const cv::Point2f& pos);
    cv::Point2f predictAndUpdate(int key, const cv::Point2f& measurement,
                                 bool detected);

    // ---- 模式0 ---------------------------------------------------------
    std::vector<DetectionResult> processMode0(const cv::Mat& frame);
    void applyHSVMask(const cv::Mat& hsv,
                      const ConfigManager::HSVRange& range,
                      cv::Mat& mask) const;

    // ---- 模式1 ---------------------------------------------------------
    std::vector<DetectionResult> processMode1(const cv::Mat& frame);
    bool loadDigitTemplates();   // 加载 3 个数字模板轮廓（一次）

    // ---- 模式2 ---------------------------------------------------------
    std::vector<DetectionResult> processMode2(const cv::Mat& frame);

    // ---- 状态 ----------------------------------------------------------
    std::atomic<uint8_t>  current_mode_{3};          // 默认 = 空闲
    std::atomic<bool>     pending_kalman_reset_{false};

    std::unordered_map<int, KalmanTracker> kalman_trackers_;

    // ---- 模式1 数字模板（懒加载一次）-----------------------------------
    bool digit_templates_loaded_ = false;
    std::vector<std::vector<cv::Point>> template_contours_;  // 数字 1/2/3 轮廓

    mutable std::mutex debug_image_mutex_;
    cv::Mat last_debug_image_;
    void setDebugImage(const cv::Mat& img);
};
