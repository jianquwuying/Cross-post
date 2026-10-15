#pragma once
// ============================================================================
// VisionPipeline.h  —  视觉算法调度 + Kalman 滤波器管理
// ============================================================================
// 与帧数据无关的无状态设计；仅持有 Kalman 滤波器状态。
//
// process() 按当前模式分发：
//   模式0  —  6 颜色物块检测
//   模式1  —  多圆环数字识别（Hu矩 + matchShapes 模板匹配，TARGET 指定输出）
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

    /// 设置当前模式（0–3）与圆环目标编号（0–3）。由 SerialThread 调用。
    /// MODE 或 TARGET 任一变化时，排队一次完整的 Kalman 重置。
    /// TARGET 仅 MODE=1 时生效（0=圆环空闲，1/2/3=指定编号）。
    void setMode(uint8_t mode, uint8_t target);

    /// 读取当前模式。由 ProcessThread 调用。
    uint8_t currentMode() const { return current_mode_.load(); }

    /// 读取当前圆环目标编号。由 ProcessThread 调用。
    uint8_t currentTarget() const { return current_target_.load(); }

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

    // -------------------------------------------------------------------
    // 模式1 检测到的圆环列表（供 UI 绘制所有圆环）
    // -------------------------------------------------------------------
    struct RingDetection {
        uint8_t     digit_id;   // 识别出的数字编号 1/2/3
        cv::Point2f center;     // 圆心（全图坐标）
        float       radius;     // 外接圆半径（像素）
    };

    /// 返回最近一帧完成数字识别的圆环列表（拷贝）。由 ProcessThread 调用。
    std::vector<RingDetection> getLastDetectedRings() const;

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

    /// 模式0/1/2 未检出目标时的统一处理（受 serial.enable_no_target_frame 控制）：
    ///   · 开关关闭       → 不推任何结果（完全回退到"无目标不发送"）
    ///   · 跟踪器已初始化 → 推送 Kalman 预测坐标（target_type 保持 detected_type），
    ///                      保证主控侧坐标连续；这是外推值、会随时间漂移，
    ///                      主控应结合时间窗口判断可信度
    ///   · 跟踪器未初始化 → 推送无目标帧 {target_type=0, color_id, x=0, y=0}
    ///                      （含"冷启动从未捕获"与"超时复位"两种情况）
    void pushLostOrNoTarget(std::vector<DetectionResult>& results,
                            int key, uint8_t detected_type, uint8_t color_id,
                            const cv::Point2f& predicted,
                            int frame_cols, int frame_rows);

    // ---- 模式0 ---------------------------------------------------------
    std::vector<DetectionResult> processMode0(const cv::Mat& frame);
    void applyHSVMask(const cv::Mat& hsv,
                      const ConfigManager::HSVRange& range,
                      cv::Mat& mask) const;

    // ---- 模式1 ---------------------------------------------------------
    std::vector<DetectionResult> processMode1(const cv::Mat& frame);
    bool loadDigitTemplates();   // 加载 3 个数字模板轮廓（一次）

    /// 把一幅二值图里"最大轮廓"填充成实心字形，按外接框归一化成
    /// size×size 的二值位图（用于位图 IoU 匹配）。
    static bool normalizeGlyph(const cv::Mat& bin, int size, cv::Mat& out);

    /// 两幅同尺寸二值位图的 IoU（交并比）。
    static double bitmapIoU(const cv::Mat& a, const cv::Mat& b);

    /// 对单个圆环轮廓做数字识别（剥同心细环 + Hu矩 argmin）。
    /// 成功返回 true，并填充 digit_id(1-3) / center(全图坐标) /
    /// radius / digit_binary(剥环后的数字 ROI 二值图)；
    /// 任一阶段失败返回 false，调用方跳过该圆环。
    bool recognizeRingDigit(const cv::Mat& blurred,
                            const cv::Rect& roi_rect,
                            const std::vector<cv::Point>& ring_contour,
                            uint8_t& digit_id,
                            cv::Point2f& center,
                            float& radius,
                            cv::Mat& digit_binary) const;

    void setLastDetectedRings(const std::vector<RingDetection>& rings);

    // ---- 模式2 ---------------------------------------------------------
    std::vector<DetectionResult> processMode2(const cv::Mat& frame);

    // ---- 状态 ----------------------------------------------------------
    std::atomic<uint8_t>  current_mode_{3};          // 默认 = 空闲
    std::atomic<uint8_t>  current_target_{0};        // 默认 = 圆环空闲
    std::atomic<bool>     pending_kalman_reset_{false};

    std::unordered_map<int, KalmanTracker> kalman_trackers_;

    // ---- 模式1 数字模板（懒加载一次）-----------------------------------
    bool digit_templates_loaded_ = false;
    // 数字模板：digit_templates_[digit-1] = 该数字在多个旋转角下的归一化位图
    std::vector<std::vector<cv::Mat>> digit_templates_;

    // ---- 模式1 最近一帧检测到的圆环（供 UI 绘制）------------------------
    mutable std::mutex rings_mutex_;
    std::vector<RingDetection> last_rings_;

    mutable std::mutex debug_image_mutex_;
    cv::Mat last_debug_image_;
    void setDebugImage(const cv::Mat& img);
};
