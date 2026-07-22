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
// 调试图像存取
// ========================================================================
void VisionPipeline::setDebugImage(const cv::Mat& img) {
    std::lock_guard<std::mutex> lock(debug_image_mutex_);
    last_debug_image_ = img.clone();
}

cv::Mat VisionPipeline::getLastDebugImage() const {
    std::lock_guard<std::mutex> lock(debug_image_mutex_);
    if (last_debug_image_.empty()) return cv::Mat();
    return last_debug_image_.clone();
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

    // 二值化调试图像（各颜色掩码合成）
    cv::Mat combined_debug(roi.size(), CV_8UC3, cv::Scalar(0, 0, 0));
    // 各颜色对应的 BGR 色彩（用于渲染合成图）
    const cv::Scalar kDebugColors[] = {
        cv::Scalar(0, 0, 255),    // 红
        cv::Scalar(0, 255, 255),  // 黄
        cv::Scalar(255, 0, 0),    // 蓝
        cv::Scalar(0, 255, 0),    // 绿
        cv::Scalar(128, 128, 128),// 黑（灰色）
        cv::Scalar(255, 255, 0),  // 青
    };

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

        // 叠加到二值化调试图像
        combined_debug.setTo(kDebugColors[c], color_mask);

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
        dr.target_type = 1;   // 1 = 颜色物块
        dr.color_id    = static_cast<uint8_t>(c);
        dr.x           = static_cast<uint16_t>(filtered.x);
        dr.y           = static_cast<uint16_t>(filtered.y);
        results.push_back(dr);
    }

    setDebugImage(combined_debug);
    return results;
}

// ========================================================================
// 模式1 —— 圆环检测（轮廓法，替代 HoughCircles）
// ========================================================================
// 流水线：ROI → Gray → GaussianBlur → adaptiveThreshold →
//          findContours(RETR_EXTERNAL) → 按面积取前 3 大 →
//          moments 质心 → Kalman 预测/更新 → DetectionResult
//
// 优势：
//   - 自适应阈值抗光照不均，RETR_EXTERNAL 忽略内部数字和内圈
//   - 性能远超 HoughCircles（树莓派5 可达 50+ FPS）
//
// **关键约束**：有效轮廓数 < 3 → 丢弃整帧。
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

    // ---- 轻度高斯模糊降噪 --------------------------------------------
    int gks = cfg.mode1GaussianKernelSize();
    if (gks % 2 == 0) gks++;
    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(gks, gks), 0);

    // ---- 自适应阈值二值化 --------------------------------------------
    // 将圆环区域转为白色(255)，背景转为黑色(0)
    int block_size = cfg.mode1AdaptiveBlockSize();
    if (block_size % 2 == 0) block_size++;
    int adaptive_c = cfg.mode1AdaptiveC();

    cv::Mat binary;
    cv::adaptiveThreshold(blurred, binary, 255,
                          cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY_INV,
                          block_size, adaptive_c);

    // ---- 形态学开运算：抹掉顶部/边缘细微白点和破碎线条 ---------------
    int open_ksize = cfg.mode1MorphOpenKernelSize();
    if (open_ksize % 2 == 0) open_ksize++;
    cv::Mat open_kernel = cv::getStructuringElement(
        cv::MORPH_ELLIPSE, cv::Size(open_ksize, open_ksize));
    cv::morphologyEx(binary, binary, cv::MORPH_OPEN, open_kernel);

    setDebugImage(binary);

    // ---- 提取最外层轮廓（RETR_EXTERNAL 忽略内部数字和内圈）----------
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours,
                     cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);

    // ---- 按面积过滤并排序 --------------------------------------------
    double min_area = static_cast<double>(cfg.mode1MinContourArea());

    // 过滤小噪点
    std::vector<std::pair<size_t, double>> valid;  // (index, area)
    for (size_t i = 0; i < contours.size(); ++i) {
        double area = cv::contourArea(contours[i]);
        if (area >= min_area) {
            valid.emplace_back(i, area);
        }
    }

    // ================================================================
    // **严格约束**：至少 3 个有效轮廓，否则丢弃帧
    // ================================================================
    if (valid.size() < 3) {
        return results;
    }

    // 按面积降序，取前 3 大
    std::partial_sort(valid.begin(), valid.begin() + 3, valid.end(),
                      [](const auto& a, const auto& b) {
                          return a.second > b.second;
                      });

    // ---- 计算质心与半径（仅前 3 大轮廓）------------------------------
    struct CircleCandidate {
        float cx, cy, radius;
    };
    std::vector<CircleCandidate> candidates;
    candidates.reserve(3);

    for (int i = 0; i < 3; ++i) {
        const auto& contour = contours[valid[i].first];
        auto m = cv::moments(contour);
        if (std::abs(m.m00) < 1e-6) continue;

        // 用 minEnclosingCircle 计算半径（比 sqrt(area/π) 更准确）
        cv::Point2f center;
        float radius;
        cv::minEnclosingCircle(contour, center, radius);

        CircleCandidate cc;
        cc.cx     = static_cast<float>(m.m10 / m.m00);
        cc.cy     = static_cast<float>(m.m01 / m.m00);
        cc.radius = radius;
        candidates.push_back(cc);
    }

    // 再次检查（moments 可能失败）
    if (candidates.size() < 3) {
        return results;
    }

    // ================================================================
    // 三重几何校验
    // ================================================================

    // ---- 第一道锁：共线性校验（cv::fitLine 最小二乘拟合）----------
    if (cfg.mode1EnableCollinearityCheck()) {
        // 收集 3 个圆心坐标
        std::vector<cv::Point2f> centers;
        for (const auto& cc : candidates) {
            centers.emplace_back(cc.cx, cc.cy);
        }

        // 最小二乘直线拟合
        cv::Vec4f line;
        cv::fitLine(centers, line, cv::DIST_L2, 0, 0.01, 0.01);
        // line: (vx, vy, x0, y0) —— 方向向量已归一化

        // 计算每个点到拟合直线的垂直距离
        float max_deviation = 0.0f;
        for (const auto& p : centers) {
            // 点到直线的垂直距离 = |(p - p0) × v| / |v|
            // 因为 |v| = 1，简化为 |cross(p - p0, v)|
            float dx = p.x - line[2];
            float dy = p.y - line[3];
            float dist = std::abs(dx * line[1] - dy * line[0]);
            if (dist > max_deviation) max_deviation = dist;
        }

        if (max_deviation > cfg.mode1CollinearityTolerancePx()) {
            // 三个圆心偏离直线过大 → 丢弃整帧
            return results;
        }
    }

    // ---- 第二道锁：半径一致性校验 -----------------------------------
    if (cfg.mode1EnableRadiusCheck()) {
        float min_r = candidates[0].radius;
        float max_r = candidates[0].radius;
        for (const auto& cc : candidates) {
            if (cc.radius < min_r) min_r = cc.radius;
            if (cc.radius > max_r) max_r = cc.radius;
        }
        float diff_r = max_r - min_r;
        if (diff_r > cfg.mode1RadiusTolerancePx()) {
            // 圆环大小不一致 → 丢弃整帧
            return results;
        }
    }

    // ---- 按 X 坐标排序：左=1  中=2  右=3 -----------------------------
    std::sort(candidates.begin(), candidates.end(),
              [](const CircleCandidate& a, const CircleCandidate& b) {
                  return a.cx < b.cx;
              });

    // ---- 生成结果 ----------------------------------------------------
    for (size_t i = 0; i < candidates.size(); ++i) {
        float local_x = candidates[i].cx;
        float local_y = candidates[i].cy;

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
        dr.target_type = 2;   // 2 = 圆环
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

    setDebugImage(black_mask);

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
    dr.target_type = 3;   // 3 = 黑色圆柱
    dr.color_id    = 0;
    dr.x           = static_cast<uint16_t>(filtered.x);
    dr.y           = static_cast<uint16_t>(filtered.y);
    results.push_back(dr);

    return results;
}
