// ============================================================================
// VisionPipeline.cpp  —  视觉算法 + Kalman 滤波器实现
// ============================================================================

#include "VisionPipeline.h"
#include "ColorOrder.h"
#include "ConfigManager.h"
#include "Logger.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

// ========================================================================
// 构造
// ========================================================================
VisionPipeline::VisionPipeline() {
    current_mode_.store(3);
    pending_kalman_reset_.store(false);
}

// ========================================================================
// 模式控制
// ========================================================================
void VisionPipeline::setMode(uint8_t mode) {
    if (mode > 3) {
        LOG_WARN("无效模式 %d，已忽略", mode);
        return;
    }
    uint8_t old = current_mode_.exchange(mode);
    if (old != mode) {
        LOG_INFO("模式切换: %d → %d", old, mode);
        pending_kalman_reset_.store(true);
    }
}

// ========================================================================
// Kalman 重置
// ========================================================================
void VisionPipeline::resetKalman() {
    kalman_trackers_.clear();
    LOG_DEBUG("Kalman 跟踪器已重置");
}

// ========================================================================
// Kalman —— 初始化跟踪器
// ========================================================================
void VisionPipeline::initKalmanTracker(int key, const cv::Point2f& pos) {
    auto& cfg  = ConfigManager::instance();
    auto& trk  = kalman_trackers_[key];

    // 2 状态 [x, y]，2 测量 [x, y]，0 控制
    trk.kf.init(2, 2, 0);

    // ---- 转移矩阵 = I₂（无速度）----------------------------------
    cv::setIdentity(trk.kf.transitionMatrix);

    // ---- 测量矩阵 = I₂ -------------------------------------------
    cv::setIdentity(trk.kf.measurementMatrix);

    // ---- 噪声协方差（标量 → 矩阵）--------------------------------
    float pnc = cfg.kalmanProcessNoiseCov();
    float mnc = cfg.kalmanMeasurementNoiseCov();

    trk.kf.processNoiseCov =
        cv::Mat::eye(2, 2, CV_32F) * pnc;
    trk.kf.measurementNoiseCov =
        cv::Mat::eye(2, 2, CV_32F) * mnc;

    // ---- 初始状态 ------------------------------------------------
    trk.kf.statePost.at<float>(0) = pos.x;
    trk.kf.statePost.at<float>(1) = pos.y;

    trk.initialized      = true;
    trk.stable_count     = 0;
    trk.lost_count       = 0;
    trk.last_measurement = pos;

    LOG_DEBUG("Kalman 跟踪器 %d 已初始化，位置 (%.0f, %.0f)",
              key, pos.x, pos.y);
}

// ========================================================================
// Kalman —— 预测 + 可选校正
// ========================================================================
cv::Point2f VisionPipeline::predictAndUpdate(int key,
                                              const cv::Point2f& measurement,
                                              bool detected) {
    auto& cfg = ConfigManager::instance();
    auto& trk = kalman_trackers_[key];

    if (!trk.initialized) {
        // ---- 初始化阶段 -----------------------------------------------
        if (detected) {
            trk.stable_count++;
            trk.last_measurement = measurement;

            if (trk.stable_count >= cfg.kalmanInitFrames()) {
                initKalmanTracker(key, measurement);
                return measurement;   // 首次输出 = 原始测量值
            }
        } else {
            trk.stable_count = 0;
        }
        // 尚未初始化 —— 返回原始测量值
        return measurement;
    }

    // ---- 跟踪阶段 ----------------------------------------------------
    trk.kf.predict();

    if (detected) {
        // 用测量值校正
        cv::Mat meas = (cv::Mat_<float>(2, 1)
                        << measurement.x, measurement.y);
        cv::Mat corrected = trk.kf.correct(meas);

        trk.lost_count       = 0;
        trk.last_measurement = measurement;

        return cv::Point2f(corrected.at<float>(0),
                           corrected.at<float>(1));
    } else {
        // 仅预测 —— 无校正
        trk.lost_count++;

        if (trk.lost_count >= cfg.kalmanLostFrameLimit()) {
            // ---- 超时复位：statePost = statePre（防止漂移）-----------
            trk.kf.statePost = trk.kf.statePre.clone();
            trk.initialized  = false;
            trk.stable_count = 0;
            trk.lost_count   = 0;
            LOG_DEBUG("Kalman 跟踪器 %d 已超时", key);
        }

        // 返回预测值
        return cv::Point2f(trk.kf.statePre.at<float>(0),
                           trk.kf.statePre.at<float>(1));
    }
}

// ========================================================================
// process —— 主分发函数
// ========================================================================
std::vector<DetectionResult> VisionPipeline::process(const cv::Mat& frame) {
    // 处理模式切换导致的待定 Kalman 重置
    if (pending_kalman_reset_.exchange(false)) {
        resetKalman();
    }

    uint8_t mode = current_mode_.load();

    switch (mode) {
        case 0:  return processMode0(frame);
        case 1:  return processMode1(frame);
        case 2:  return processMode2(frame);
        case 3:  // 空闲 —— 返回空
        default: return {};
    }
}

// ========================================================================
// 模式0 —— 6 颜色物块检测
// ========================================================================
// 流水线：ROI → BGR→HSV → 逐色掩码 → 轮廓 → 质心 →
//          Kalman 预测/更新 → DetectionResult
//
// 红色使用**双** HSV 区间（H 0-10 + H 170-180），通过 bitwise_or 合并。
// 每种颜色独立跟踪。
// ========================================================================

void VisionPipeline::applyHSVMask(const cv::Mat& hsv,
                                   const ConfigManager::HSVRange& range,
                                   cv::Mat& mask) const {
    cv::inRange(hsv,
                cv::Scalar(range.h_min, range.s_min, range.v_min),
                cv::Scalar(range.h_max, range.s_max, range.v_max),
                mask);
}

std::vector<DetectionResult> VisionPipeline::processMode0(
    const cv::Mat& frame) {
    std::vector<DetectionResult> results;
    auto& cfg = ConfigManager::instance();

    // ---- ROI ----------------------------------------------------------
    cv::Rect roi_rect(cfg.roiX(), cfg.roiY(),
                      cfg.roiWidth(), cfg.roiHeight());
    // 将 ROI 裁剪到帧边界内
    roi_rect &= cv::Rect(0, 0, frame.cols, frame.rows);
    if (roi_rect.width <= 0 || roi_rect.height <= 0) {
        return results;
    }
    cv::Mat roi = frame(roi_rect);

    // ---- BGR → HSV ----------------------------------------------------
    cv::Mat hsv;
    cv::cvtColor(roi, hsv, cv::COLOR_BGR2HSV);

    int morph_ksize = cfg.mode0MorphKernelSize();
    cv::Mat morph_kernel = cv::getStructuringElement(
        cv::MORPH_RECT,
        cv::Size(morph_ksize, morph_ksize));

    double min_area = static_cast<double>(cfg.mode0MinContourArea());

    // ---- 逐色处理 ----------------------------------------------------
    for (int c = 0; c < kNumColors; ++c) {
        const char* color_name = kColorNames[c];

        // 构建颜色掩码（红色可能为双区间）
        auto ranges = cfg.mode0ColorHSV(color_name);
        cv::Mat color_mask(roi.size(), CV_8UC1, cv::Scalar(0));

        for (const auto& r : ranges) {
            cv::Mat m;
            applyHSVMask(hsv, r, m);
            cv::bitwise_or(color_mask, m, color_mask);
        }

        // 形态学开运算（去噪）
        cv::morphologyEx(color_mask, color_mask,
                         cv::MORPH_OPEN, morph_kernel);

        // 查找轮廓
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(color_mask, contours,
                         cv::RETR_EXTERNAL,
                         cv::CHAIN_APPROX_SIMPLE);

        if (contours.empty()) {
            // 该颜色无检测 → 仅 Kalman 预测
            predictAndUpdate(c, cv::Point2f(0, 0), false);
            continue;
        }

        // 按面积选取最大轮廓
        auto largest = std::max_element(
            contours.begin(), contours.end(),
            [](const std::vector<cv::Point>& a,
               const std::vector<cv::Point>& b) {
                return cv::contourArea(a) < cv::contourArea(b);
            });

        double area = cv::contourArea(*largest);
        if (area < min_area) {
            predictAndUpdate(c, cv::Point2f(0, 0), false);
            continue;
        }

        // 计算质心
        auto moments = cv::moments(*largest);
        if (std::abs(moments.m00) < 1e-6) {
            predictAndUpdate(c, cv::Point2f(0, 0), false);
            continue;
        }

        float local_x = static_cast<float>(moments.m10 / moments.m00);
        float local_y = static_cast<float>(moments.m01 / moments.m00);

        // 转换到全图坐标
        cv::Point2f measurement(local_x + roi_rect.x,
                                local_y + roi_rect.y);

        // Kalman 滤波
        cv::Point2f filtered = predictAndUpdate(c, measurement, true);

        // 裁剪到有效范围
        filtered.x = std::max(0.0f, std::min(filtered.x,
                              static_cast<float>(frame.cols - 1)));
        filtered.y = std::max(0.0f, std::min(filtered.y,
                              static_cast<float>(frame.rows - 1)));

        DetectionResult dr;
        dr.target_type = 0;
        dr.color_id    = static_cast<uint8_t>(c);
        dr.x           = static_cast<uint16_t>(filtered.x);
        dr.y           = static_cast<uint16_t>(filtered.y);
        results.push_back(dr);
    }

    return results;
}

// ========================================================================
// 模式1 —— 圆环检测（Hough）
// ========================================================================
// 流水线：ROI → Gray → CLAHE → MORPH_GRADIENT → Gaussian → HoughCircles
//
// **关键约束**：circles.size() != 3 → 丢弃整帧。
//              （不更新 Kalman、不发送数据。）
//
// 圆按 X 坐标排序：左=1  中=2  右=3
// ========================================================================
std::vector<DetectionResult> VisionPipeline::processMode1(
    const cv::Mat& frame) {
    std::vector<DetectionResult> results;
    auto& cfg = ConfigManager::instance();

    // ---- ROI ----------------------------------------------------------
    cv::Rect roi_rect(cfg.roiX(), cfg.roiY(),
                      cfg.roiWidth(), cfg.roiHeight());
    roi_rect &= cv::Rect(0, 0, frame.cols, frame.rows);
    if (roi_rect.width <= 0 || roi_rect.height <= 0) {
        return results;
    }
    cv::Mat roi = frame(roi_rect);

    // ---- 灰度化 -------------------------------------------------------
    cv::Mat gray;
    cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);

    // ---- CLAHE --------------------------------------------------------
    auto clahe = cv::createCLAHE(
        cfg.mode1ClaheClipLimit(),
        cv::Size(cfg.mode1ClaheTileSize(), cfg.mode1ClaheTileSize()));
    cv::Mat clahe_img;
    clahe->apply(gray, clahe_img);

    // ---- 形态学梯度 ---------------------------------------------------
    int mks = cfg.mode1MorphKernelSize();
    cv::Mat grad_kernel = cv::getStructuringElement(
        cv::MORPH_RECT, cv::Size(mks, mks));
    cv::Mat grad;
    cv::morphologyEx(clahe_img, grad, cv::MORPH_GRADIENT, grad_kernel);

    // ---- 高斯模糊 -----------------------------------------------------
    int gks = cfg.mode1GaussianKernelSize();
    if (gks % 2 == 0) gks++;   // 确保为奇数
    cv::Mat blurred;
    cv::GaussianBlur(grad, blurred, cv::Size(gks, gks), 0);

    // ---- Hough 圆检测 ------------------------------------------------
    std::vector<cv::Vec3f> circles;
    cv::HoughCircles(blurred, circles, cv::HOUGH_GRADIENT,
                     cfg.mode1HoughDp(),
                     cfg.mode1HoughMinDist(),
                     cfg.mode1HoughParam1(),
                     cfg.mode1HoughParam2(),
                     cfg.mode1HoughMinRadius(),
                     cfg.mode1HoughMaxRadius());

    // ================================================================
    // **严格约束**：恰好 3 个圆，否则丢弃帧
    // ================================================================
    if (circles.size() != 3) {
        // 丢弃 —— 不更新 Kalman，不输出
        return results;
    }

    // ---- 按 X 坐标排序 -----------------------------------------------
    std::sort(circles.begin(), circles.end(),
              [](const cv::Vec3f& a, const cv::Vec3f& b) {
                  return a[0] < b[0];
              });

    // ---- 生成结果（圆 1 = 左，2 = 中，3 = 右）----------------------
    for (size_t i = 0; i < circles.size(); ++i) {
        float local_x = circles[i][0];
        float local_y = circles[i][1];

        // 转换到全图坐标
        cv::Point2f measurement(local_x + roi_rect.x,
                                local_y + roi_rect.y);

        // 键 = 圆编号 (1, 2, 3)
        int key = static_cast<int>(i) + 1;
        cv::Point2f filtered = predictAndUpdate(key, measurement, true);

        filtered.x = std::max(0.0f, std::min(filtered.x,
                              static_cast<float>(frame.cols - 1)));
        filtered.y = std::max(0.0f, std::min(filtered.y,
                              static_cast<float>(frame.rows - 1)));

        DetectionResult dr;
        dr.target_type = 1;
        dr.color_id    = static_cast<uint8_t>(key);
        dr.x           = static_cast<uint16_t>(filtered.x);
        dr.y           = static_cast<uint16_t>(filtered.y);
        results.push_back(dr);
    }

    return results;
}

// ========================================================================
// 模式2 —— 黑色圆柱检测
// ========================================================================
// 流水线：ROI → BGR→HSV → 黑色掩码 → 形态学 → 轮廓 →
//          minAreaRect → 长宽比/面积过滤 → Kalman
// ========================================================================
std::vector<DetectionResult> VisionPipeline::processMode2(
    const cv::Mat& frame) {
    std::vector<DetectionResult> results;
    auto& cfg = ConfigManager::instance();

    // ---- ROI ----------------------------------------------------------
    cv::Rect roi_rect(cfg.roiX(), cfg.roiY(),
                      cfg.roiWidth(), cfg.roiHeight());
    roi_rect &= cv::Rect(0, 0, frame.cols, frame.rows);
    if (roi_rect.width <= 0 || roi_rect.height <= 0) {
        return results;
    }
    cv::Mat roi = frame(roi_rect);

    // ---- HSV → 黑色掩码 -----------------------------------------------
    cv::Mat hsv;
    cv::cvtColor(roi, hsv, cv::COLOR_BGR2HSV);

    auto bhsv = cfg.mode2BlackHSV();
    cv::Mat black_mask;
    cv::inRange(hsv,
                cv::Scalar(bhsv.h_min, bhsv.s_min, bhsv.v_min),
                cv::Scalar(bhsv.h_max, bhsv.s_max, bhsv.v_max),
                black_mask);

    // ---- 形态学闭运算（填充空洞）--------------------------------------
    int mks = cfg.mode2MorphKernelSize();
    cv::Mat morph_kernel = cv::getStructuringElement(
        cv::MORPH_RECT, cv::Size(mks, mks));
    cv::morphologyEx(black_mask, black_mask,
                     cv::MORPH_CLOSE, morph_kernel);

    // ---- 查找轮廓 ----------------------------------------------------
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(black_mask, contours,
                     cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);

    if (contours.empty()) {
        predictAndUpdate(0, cv::Point2f(0, 0), false);
        return results;
    }

    // ---- 寻找最佳候选（最大有效轮廓）----------------------------------
    double aspect_thresh = cfg.mode2AspectRatioThreshold();
    double min_area      = static_cast<double>(cfg.mode2MinArea());

    double best_area = 0.0;
    cv::Point2f best_center(-1, -1);

    for (const auto& contour : contours) {
        double area = cv::contourArea(contour);
        if (area < min_area) continue;

        cv::RotatedRect rect = cv::minAreaRect(contour);
        float w = rect.size.width;
        float h = rect.size.height;
        if (w < 1.0f || h < 1.0f) continue;

        float aspect_ratio = std::max(w, h) / std::min(w, h);
        if (aspect_ratio < aspect_thresh) continue;

        if (area > best_area) {
            best_area   = area;
            best_center = rect.center;
        }
    }

    if (best_center.x < 0) {
        // 未找到有效圆柱
        predictAndUpdate(0, cv::Point2f(0, 0), false);
        return results;
    }

    // ---- Kalman -------------------------------------------------------
    cv::Point2f measurement(best_center.x + roi_rect.x,
                            best_center.y + roi_rect.y);
    cv::Point2f filtered = predictAndUpdate(0, measurement, true);

    filtered.x = std::max(0.0f, std::min(filtered.x,
                          static_cast<float>(frame.cols - 1)));
    filtered.y = std::max(0.0f, std::min(filtered.y,
                          static_cast<float>(frame.rows - 1)));

    DetectionResult dr;
    dr.target_type = 2;
    dr.color_id    = 0;
    dr.x           = static_cast<uint16_t>(filtered.x);
    dr.y           = static_cast<uint16_t>(filtered.y);
    results.push_back(dr);

    return results;
}
