#pragma once
// ============================================================================
// VisionPipeline.h  —  视觉算法调度 + Kalman 滤波器管理
// ============================================================================
// 不持有原始帧数据；内部维护 Kalman 跟踪器、模式1 数字模板，
// 以及供 UI 读取的最近一帧调试图与圆环列表。
//
// process() 按当前模式分发：
//   模式0  —  6 色物块逐槽位快照（开关开启时每帧固定 6 包）
//   模式1  —  多圆环数字识别（归一化位图多角度 IoU，按环号优先级只推 1 包）
//   模式2  —  黑色圆柱检测
//   模式3  —  空闲（不检测；开关开启时每帧推 1 个无目标包）
//
// Kalman 滤波器：2-D [x, y]，恒等转移矩阵，**无速度模型**；
// 标量过程/测量噪声从 YAML 读取 → eye(2,2) * scalar。
// 连续 init_frames 帧检出后初始化；连续 lost_frame_limit 帧未检出后复位。
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
    /// 模式变化时，排队一次完整的 Kalman 重置；模式未变化不做任何事。
    /// 非法模式（>3）仅记录告警并忽略。
    /// 注：串口命令里的 TARGET 位不参与任何判断（帧格式保留但忽略）。
    void setMode(uint8_t mode);

    /// 读取当前模式。由 ProcessThread 调用。
    uint8_t currentMode() const { return current_mode_.load(); }

    // -------------------------------------------------------------------
    // 帧处理
    // -------------------------------------------------------------------

    /// 处理一帧并返回本帧要发送的检测结果。
    /// 由 ProcessThread 单线程调用；调试图与圆环列表的跨线程读取由互斥量保护。
    /// 输出包数：模式0 开关开启时 6 包；模式1、2、3 至多 1 包。
    std::vector<DetectionResult> process(const cv::Mat& frame);

    // -------------------------------------------------------------------
    // Kalman 状态重置（公开；process() 在模式切换后自动调用）
    // -------------------------------------------------------------------
    void resetKalman();

    /// 返回最近一次处理的二值化调试图像（供 main 中显示）。
    /// 模式0：6 色掩码按各自 BGR 色彩合成到黑底
    /// 模式1：各判号圆环的数字 ROI 二值图水平拼接；无有效数字时回退为环级二值图
    /// 模式2：黑色掩码
    /// 模式3：不更新，保留进入空闲前的最后一幅
    cv::Mat getLastDebugImage() const;

    // -------------------------------------------------------------------
    // 模式1 检测到的圆环列表（供 UI 绘制所有圆环）
    // -------------------------------------------------------------------
    struct RingDetection {
        uint8_t     digit_id;   // 识别出的数字编号 1/2/3
        cv::Point2f center;     // 圆心（全图坐标）
        float       radius;     // 外接圆半径（像素）
    };

    /// 返回最近一帧完成判号的圆环列表（拷贝，含同编号冲突的环）。
    /// 由 UI 绘制代码读取；非模式1 不更新（保留最后一次模式1 的结果）。
    std::vector<RingDetection> getLastDetectedRings() const;

private:
    // ---- 每个目标的 Kalman 跟踪器 ------------------------------------
    struct KalmanTracker {
        cv::KalmanFilter kf;
        bool initialized    = false;
        int  stable_count   = 0;       // 初始化前的连续检出帧数
        int  lost_count     = 0;       // 连续丢失帧数
        cv::Point2f last_measurement;
    };

    // ---- Kalman 辅助函数 ----------------------------------------------
    void initKalmanTracker(int key, const cv::Point2f& pos);
    cv::Point2f predictAndUpdate(int key, const cv::Point2f& measurement,
                                 bool detected);

    /// 模式0/1/2 未检出目标时的统一处理（受 serial.enable_no_target_frame 控制）：
    ///   · 开关关闭       → 不推任何结果（完全回退到"无目标不发送"）
    ///   · 跟踪器已初始化 → 推送 Kalman 预测坐标（target_type 保持 detected_type，
    ///                      坐标钳制到帧边界内），保证主控侧坐标连续；
    ///                      恒等转移矩阵下预测值等于最后一次滤波状态，不会随时间外推
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
    bool loadDigitTemplates();   // 加载 3 个数字模板图并生成多角度归一化位图（首次成功后不再加载）

    /// 把一幅二值图里"最大轮廓"填充成实心字形，按外接框归一化成
    /// size×size 的二值位图（用于位图 IoU 匹配）。
    static bool normalizeGlyph(const cv::Mat& bin, int size, cv::Mat& out);

    /// 两幅同尺寸二值位图的 IoU（交并比）。
    static double bitmapIoU(const cv::Mat& a, const cv::Mat& b);

    /// 对单个圆环轮廓做数字识别（剥同心细环 + 归一化位图多角度 IoU argmax）。
    /// 成功返回 true，并填充 digit_id(1-3) / center(全图坐标) /
    /// radius / digit_binary(剥环后的数字 ROI 二值图)；
    /// 最高与次高 IoU 之差小于 digit_min_margin 时按拒答处理，返回 false；
    /// 任一阶段失败也返回 false，调用方跳过该圆环。
    bool recognizeRingDigit(const cv::Mat& blurred,
                            const cv::Rect& roi_rect,
                            const std::vector<cv::Point>& ring_contour,
                            uint8_t& digit_id,
                            cv::Point2f& center,
                            float& radius,
                            cv::Mat& digit_binary) const;

    /// 模式1 统一出口：先把环号 1/2/3 三个 Kalman 各推进一帧（保持状态新鲜），
    /// 再按优先级 **2 > 1 > 3** 只推一包：
    ///   ① 本帧真实检出 → Kalman 滤波坐标（type=2；跟踪器未初始化时为原始测量值）
    ///   ② 未检出但跟踪器未超时 → 预测坐标（type=2）
    ///   ③ 三个都不可用 → {type=0, color_id=2, 0, 0}（0 值包）
    /// 选择严格按环号（环2 的预测坐标可以压过环1 的真实检出）；
    /// 同编号冲突（两个环判成同一编号）→ 该环号本帧按未检出处理，其余环号照常；
    /// ②③ 受 serial.enable_no_target_frame 控制（关闭时只推真实检出）。
    void reportMode1Rings(std::vector<DetectionResult>& results,
                          const std::vector<RingDetection>& rings,
                          int frame_cols, int frame_rows);

    void setLastDetectedRings(const std::vector<RingDetection>& rings);

    // ---- 模式2 ---------------------------------------------------------
    std::vector<DetectionResult> processMode2(const cv::Mat& frame);

    // ---- 状态 ----------------------------------------------------------
    std::atomic<uint8_t>  current_mode_{3};          // 默认 = 空闲
    std::atomic<bool>     pending_kalman_reset_{false};

    std::unordered_map<int, KalmanTracker> kalman_trackers_;

    // ---- 模式1 数字模板（首次成功加载后置位）---------------------------
    bool digit_templates_loaded_ = false;
    // 数字模板：digit_templates_[digit-1] = 该数字在 ±range、步长 step 下
    // 预生成的多个归一化位图实例
    std::vector<std::vector<cv::Mat>> digit_templates_;

    // ---- 模式1 最近一帧完成判号的圆环（供 UI 绘制，含同编号冲突的环）------
    mutable std::mutex rings_mutex_;
    std::vector<RingDetection> last_rings_;

    mutable std::mutex debug_image_mutex_;
    cv::Mat last_debug_image_;
    void setDebugImage(const cv::Mat& img);
};
