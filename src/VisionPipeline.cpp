// ============================================================================
// VisionPipeline.cpp  —  视觉算法 + Kalman 滤波器实现
// ============================================================================

#include "VisionPipeline.h"
#include "ColorOrder.h"
#include "ConfigManager.h"
#include "Logger.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

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
void VisionPipeline::setMode(uint8_t mode, uint8_t target) {
    if (mode > 3) {
        LOG_WARN("无效模式 %d，已忽略", mode);
        return;
    }
    if (target > 3) {
        LOG_WARN("无效目标 %d，已忽略", target);
        return;
    }

    uint8_t old_mode   = current_mode_.load();
    uint8_t old_target = current_target_.load();
    if (old_mode == mode && old_target == target) {
        return;   // 无变化 —— 不重置 Kalman
    }

    current_mode_.store(mode);
    current_target_.store(target);

    LOG_INFO("模式/目标切换: MODE%d TARGET%d → MODE%d TARGET%d",
             old_mode, old_target, mode, target);
    pending_kalman_reset_.store(true);
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
// 模式1 圆环列表存取（供 UI 绘制所有检测到的圆环）
// ========================================================================
void VisionPipeline::setLastDetectedRings(
    const std::vector<RingDetection>& rings) {
    std::lock_guard<std::mutex> lock(rings_mutex_);
    last_rings_ = rings;
}

std::vector<VisionPipeline::RingDetection>
VisionPipeline::getLastDetectedRings() const {
    std::lock_guard<std::mutex> lock(rings_mutex_);
    return last_rings_;
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

    // ---- 颜色启用过滤：只处理 mode0.enabled_colors 列出的颜色 ------------
    // 被屏蔽的颜色整色跳过：不检测、不更新 Kalman、不发送。
    // （color_id 仍为全局 0-5 索引，屏蔽不重编号，避免串口协议对不齐）
    const auto enabled_colors = cfg.mode0EnabledColors();

    // ---- 逐色处理 ----------------------------------------------------
    for (int c = 0; c < kNumColors; ++c) {
        const char* color_name = kColorNames[c];

        if (std::find(enabled_colors.begin(), enabled_colors.end(),
                      color_name) == enabled_colors.end()) {
            continue;   // 该颜色被屏蔽
        }

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
// 模式1 —— 多圆环数字识别（同心环剥除 + Hu矩模板 argmin）
// ========================================================================
// 流水线：ROI → Gray → GaussianBlur → adaptiveThreshold →
//          MORPH_OPEN → findContours(RETR_EXTERNAL) → 面积过滤 →
//          形状过滤(圆度 + minAreaRect 长宽比 + 填充率，剔除线状/方形干扰) → 按面积降序 →
//          嵌套去重(包围盒包含 + 圆心距) → 取前 max_rings_per_frame 个 →
//          逐环：moments 圆心 + minEnclosingCircle 半径 → 中心方形数字 ROI →
//          数字自适应二值化 → 开运算剥同心细环(核∝半径) →
//          取靠近圆心的最大墨块 → cv::matchShapes 与 3 模板对比(argmin) →
//          同编号冲突整帧丢弃 → 按 TARGET 取指定编号 → Kalman → 输出单包
//
// **关键特性**：
//   - 视野内可同时出现多个圆环，各自识别数字编号
//   - 形状过滤先剔掉线状/条状/方形干扰，避免它们占用 max_rings_per_frame
//     名额把真靶标挤出前 N（如贯穿画面的桌边线、背景板/方形纸片）
//   - 同一物理靶标的同心环是断开的独立轮廓 → 嵌套去重后只留最大外环，
//     保证「一个物理靶标 = 一个候选」，不会误触发丢帧
//   - 仅输出 TARGET 指定编号的圆环；TARGET=0 时圆环空闲（不检测、不输出）
//   - 靶标为"中心数字 + 多层同心细环"；开核按半径缩放，距离无关地剥环
//   - Hu 矩天然旋转不变，机械臂旋转下无需任何旋转模板
//   - 该字体绝对匹配分偏高且不稳，改用三模板 argmin 判数字编号（不做绝对阈值）
// ========================================================================
std::vector<DetectionResult> VisionPipeline::processMode1(
    const cv::Mat& frame) {
    std::vector<DetectionResult> results;
    auto& cfg = ConfigManager::instance();

    const uint8_t target = current_target_.load();

    // ---- TARGET=0：圆环空闲，不检测、不输出 ---------------------------
    if (target == 0) {
        setLastDetectedRings({});
        return results;
    }

    // ---- 数字模板懒加载（仅一次）--------------------------------------
    if (!digit_templates_loaded_) {
        if (!loadDigitTemplates()) {
            LOG_ERROR("数字模板加载失败，模式1 无法运行");
            setLastDetectedRings({});
            return results;
        }
        digit_templates_loaded_ = true;
    }

    // ---- ROI ----------------------------------------------------------
    cv::Rect roi_rect(cfg.roiX(), cfg.roiY(),
                      cfg.roiWidth(), cfg.roiHeight());
    roi_rect &= cv::Rect(0, 0, frame.cols, frame.rows);
    if (roi_rect.width <= 0 || roi_rect.height <= 0) {
        setLastDetectedRings({});
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

    // ---- 自适应阈值二值化（圆环区域转白，背景转黑）-------------------
    int block_size = cfg.mode1AdaptiveBlockSize();
    if (block_size % 2 == 0) block_size++;
    int adaptive_c = cfg.mode1AdaptiveC();

    cv::Mat binary;
    cv::adaptiveThreshold(blurred, binary, 255,
                          cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY_INV,
                          block_size, adaptive_c);

    // ---- 形态学开运算：抹掉细微白点和破碎线条 ------------------------
    int open_ksize = cfg.mode1MorphOpenKernelSize();
    if (open_ksize % 2 == 0) open_ksize++;
    cv::Mat open_kernel = cv::getStructuringElement(
        cv::MORPH_ELLIPSE, cv::Size(open_ksize, open_ksize));
    cv::morphologyEx(binary, binary, cv::MORPH_OPEN, open_kernel);

    // 环级二值图先设为调试图：若后续阶段失败返回，画面停留在此图，
    // 真实反映"静止时圆环是否断裂/消失"，便于现场定位（数字路径随后覆盖）
    setDebugImage(binary);

    // ---- 提取所有圆环轮廓（面积 ≥ min_contour_area）--------------------
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours,
                     cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);

    const double min_area = static_cast<double>(cfg.mode1MinContourArea());

    // ---- 形状过滤参数 --------------------------------------------------
    // 开关关闭时也照常读取三个键 —— 保证缺键仍立即报错退出（与其他配置一致）
    const bool   shape_filter_on = cfg.mode1EnableShapeFilter();
    const double min_circularity = cfg.mode1MinCircularity();
    const double max_aspect      = cfg.mode1MaxAspectRatio();
    const double min_fill_ratio   = cfg.mode1MinFillRatio();

    std::vector<const std::vector<cv::Point>*> area_filtered;
    for (const auto& c : contours) {
        const double area = cv::contourArea(c);
        if (area < min_area) {
            continue;
        }

        // ---- 形状过滤：圆度 + 长宽比双校验，剔除线状/条状/畸形干扰 --------
        // 圆度 4π·面积/周长² 对线状干扰极敏感（实测桌边线 ≈0.02，圆环 ≈0.88）；
        // 长宽比取 minAreaRect（而非 boundingRect，避免旋转放大长宽比）。
        // 两者互补：圆度剔线状，长宽比剔长条/畸形块。
        if (shape_filter_on) {
            const double perimeter = cv::arcLength(c, true);
            if (perimeter < 1e-6) {
                continue;   // 退化轮廓，避免除零
            }
            const double circularity =
                4.0 * CV_PI * area / (perimeter * perimeter);
            if (circularity < min_circularity) {
                LOG_DEBUG("模式1: 轮廓被圆度过滤 (圆度=%.3f, 面积=%.0f)",
                          circularity, area);
                continue;
            }

            const cv::RotatedRect rr = cv::minAreaRect(c);
            const double w = rr.size.width;
            const double h = rr.size.height;
            if (w < 1.0 || h < 1.0) {
                continue;
            }
            const double aspect = std::max(w, h) / std::min(w, h);
            if (aspect > max_aspect) {
                LOG_DEBUG("模式1: 轮廓被长宽比过滤 (长宽比=%.2f, 面积=%.0f)",
                          aspect, area);
                continue;
            }

            // 填充率 = 面积 / 外接圆面积：理想圆 1.0，正方形仅 ≈0.637。
            // 正方形的圆度 ≈0.785、长宽比 =1.0，能骗过上面两道校验，必须靠
            // 这一道把它剔除（实测真实靶标 0.98 vs 正方形 0.64）。
            cv::Point2f circle_center;
            float circle_radius = 0.0f;
            cv::minEnclosingCircle(c, circle_center, circle_radius);
            if (circle_radius > 1e-6f) {
                const double fill_ratio =
                    area / (CV_PI * circle_radius * circle_radius);
                if (fill_ratio < min_fill_ratio) {
                    LOG_DEBUG("模式1: 轮廓被填充率过滤 (填充率=%.3f, 面积=%.0f)",
                              fill_ratio, area);
                    continue;
                }
            }
        }

        area_filtered.push_back(&c);
    }
    if (area_filtered.empty()) {
        setLastDetectedRings({});
        return results;   // 无满足面积阈值的圆环
    }

    // ---- 按面积从大到小排序（保证外环先进入保留列表）--------------------
    std::sort(area_filtered.begin(), area_filtered.end(),
              [](const std::vector<cv::Point>* a,
                 const std::vector<cv::Point>* b) {
                  return cv::contourArea(*a) > cv::contourArea(*b);
              });

    // ---- 嵌套去重：同一物理靶标的同心环只保留最大外环 -------------------
    // 真机上同心圆环二值化后是断开的独立轮廓，一个物理靶标会产生多个候选
    // （外环/中环/内环），必须去重，否则 TARGET 筛选会拿错环。
    // 判定"c 是 v 的嵌套碎片"需**同时**满足：
    //   ① c 的包围盒被 v 的包围盒包含（坐标比较带 2px 容差；
    //      不用 Rect::contains(rect.br()) —— OpenCV 的 br() 是排他坐标，
    //      边界正好重合时会漏判）
    //   ② 两轮廓圆心距 < 0.5 × v 的半径（同心环圆心几乎重合；两个独立
    //      靶标即使包围盒嵌套，圆心距也远大于此，不会被误合并）
    constexpr double kNestedRectTolerancePx = 2.0;
    constexpr double kNestedCenterRatio     = 0.5;

    std::vector<const std::vector<cv::Point>*> valid_contours;
    for (const auto* c : area_filtered) {
        const cv::Rect rect_inner = cv::boundingRect(*c);

        auto m_inner = cv::moments(*c);
        if (std::abs(m_inner.m00) < 1e-6) {
            continue;
        }
        const cv::Point2f center_inner(
            static_cast<float>(m_inner.m10 / m_inner.m00),
            static_cast<float>(m_inner.m01 / m_inner.m00));

        bool nested = false;
        for (const auto* v : valid_contours) {
            const cv::Rect rect_outer = cv::boundingRect(*v);
            const bool box_contained =
                rect_inner.x >= rect_outer.x - kNestedRectTolerancePx &&
                rect_inner.y >= rect_outer.y - kNestedRectTolerancePx &&
                rect_inner.x + rect_inner.width <=
                    rect_outer.x + rect_outer.width + kNestedRectTolerancePx &&
                rect_inner.y + rect_inner.height <=
                    rect_outer.y + rect_outer.height + kNestedRectTolerancePx;
            if (!box_contained) {
                continue;
            }

            auto m_outer = cv::moments(*v);
            if (std::abs(m_outer.m00) < 1e-6) {
                continue;
            }
            const cv::Point2f center_outer(
                static_cast<float>(m_outer.m10 / m_outer.m00),
                static_cast<float>(m_outer.m01 / m_outer.m00));

            cv::Point2f circle_center;
            float radius_outer = 0.0f;
            cv::minEnclosingCircle(*v, circle_center, radius_outer);

            const float dist = cv::norm(center_inner - center_outer);
            if (dist < kNestedCenterRatio * radius_outer) {
                LOG_DEBUG("模式1: 轮廓嵌套去重丢弃 (面积: %.0f, 圆心距: %.1f)",
                          cv::contourArea(*c), dist);
                nested = true;
                break;
            }
        }
        if (!nested) {
            valid_contours.push_back(c);
        }
    }

    if (valid_contours.empty()) {
        setLastDetectedRings({});
        return results;
    }

    // ---- 性能上限：每帧最多处理 max_rings_per_frame 个（取面积最大者）----
    const int max_per_frame = cfg.mode1MaxRingsPerFrame();
    if (static_cast<int>(valid_contours.size()) > max_per_frame) {
        valid_contours.resize(static_cast<size_t>(max_per_frame));
    }

    // ---- 逐候选圆环识别数字（单个失败只跳过该环，不影响其他环）----------
    std::vector<RingDetection> rings;
    std::vector<cv::Mat> digit_tiles;
    for (const auto* rc : valid_contours) {
        uint8_t     digit_id = 0;
        cv::Point2f center;
        float       radius = 0.0f;
        cv::Mat     digit_binary;

        if (!recognizeRingDigit(blurred, roi_rect, *rc,
                                digit_id, center, radius, digit_binary)) {
            LOG_DEBUG("模式1：候选圆环数字识别失败，已跳过");
            continue;
        }

        RingDetection rd;
        rd.digit_id = digit_id;
        rd.center   = center;
        rd.radius   = radius;
        rings.push_back(rd);
        digit_tiles.push_back(digit_binary);
    }

    // ---- 调试图：所有候选数字 ROI 二值图（统一高度水平拼接）------------
    if (!digit_tiles.empty()) {
        int tile_h = 0;
        for (const auto& t : digit_tiles) {
            tile_h = std::max(tile_h, t.rows);
        }
        std::vector<cv::Mat> tiles;
        for (const auto& t : digit_tiles) {
            double scale = static_cast<double>(tile_h) / t.rows;
            cv::Mat scaled;
            cv::resize(t, scaled,
                       cv::Size(std::max(1, cvRound(t.cols * scale)), tile_h),
                       0, 0, cv::INTER_NEAREST);
            tiles.push_back(scaled);
            // 8px 白色分隔条，便于区分各圆环
            tiles.push_back(cv::Mat(tile_h, 8, CV_8UC1, cv::Scalar(255)));
        }
        tiles.pop_back();   // 去掉末尾分隔条
        cv::Mat debug_concat;
        cv::hconcat(tiles, debug_concat);
        setDebugImage(debug_concat);
    } else {
        setDebugImage(binary);   // 无有效数字 → 回退显示环级二值图
    }

    // 供 UI 绘制所有圆环（含冲突帧 —— 便于现场定位丢帧原因）
    setLastDetectedRings(rings);

    if (rings.empty()) {
        // 所有候选数字识别均失败 → 等效未检出
        predictAndUpdate(target, cv::Point2f(0, 0), false);
        return results;
    }

    // ---- 同编号冲突检测：同一编号出现两次 → 抛弃整帧结果 ----------------
    for (size_t i = 0; i < rings.size(); ++i) {
        for (size_t j = i + 1; j < rings.size(); ++j) {
            if (rings[i].digit_id == rings[j].digit_id) {
                LOG_WARN("模式1：检测到重复数字编号 %d，丢弃本帧",
                         rings[i].digit_id);
                predictAndUpdate(target, cv::Point2f(0, 0), false);
                return results;
            }
        }
    }

    // ---- 按 TARGET 筛选 ------------------------------------------------
    const RingDetection* hit = nullptr;
    for (const auto& r : rings) {
        if (r.digit_id == target) {
            hit = &r;
            break;
        }
    }
    if (!hit) {
        // TARGET 指定圆环未检出 → 不输出、不发送，推进 Kalman 丢失计数
        predictAndUpdate(target, cv::Point2f(0, 0), false);
        return results;
    }

    // ---- Kalman（key = TARGET 编号 1/2/3）------------------------------
    cv::Point2f filtered = predictAndUpdate(target, hit->center, true);

    filtered.x = std::max(0.0f, std::min(filtered.x,
                          static_cast<float>(frame.cols - 1)));
    filtered.y = std::max(0.0f, std::min(filtered.y,
                          static_cast<float>(frame.rows - 1)));

    DetectionResult dr;
    dr.target_type = 2;         // 2 = 圆环
    dr.color_id    = target;    // 数字编号 1/2/3
    dr.x           = static_cast<uint16_t>(filtered.x);
    dr.y           = static_cast<uint16_t>(filtered.y);
    results.push_back(dr);

    return results;
}

// ========================================================================
// 模式1 —— 数字模板加载（仅一次）
// ========================================================================
bool VisionPipeline::loadDigitTemplates() {
    auto& cfg = ConfigManager::instance();
    template_contours_.clear();
    template_contours_.reserve(3);

    for (int id = 1; id <= 3; ++id) {
        std::string path = cfg.mode1TemplatePath(id);
        cv::Mat tmpl = cv::imread(path, cv::IMREAD_GRAYSCALE);
        if (tmpl.empty()) {
            LOG_ERROR("加载数字模板失败: %s", path.c_str());
            return false;
        }

        // 模板为白底黑字 → 反色使数字为白（前景），与当前数字二值化约定一致
        cv::Mat bin;
        cv::threshold(tmpl, bin, 0, 255,
                      cv::THRESH_BINARY_INV | cv::THRESH_OTSU);

        std::vector<std::vector<cv::Point>> cnts;
        cv::findContours(bin, cnts, cv::RETR_EXTERNAL,
                         cv::CHAIN_APPROX_SIMPLE);
        if (cnts.empty()) {
            LOG_ERROR("数字模板无有效轮廓: %s", path.c_str());
            return false;
        }

        auto largest = std::max_element(
            cnts.begin(), cnts.end(),
            [](const std::vector<cv::Point>& a,
               const std::vector<cv::Point>& b) {
                return cv::contourArea(a) < cv::contourArea(b);
            });
        template_contours_.push_back(*largest);
    }

    LOG_INFO("数字模板加载完成 (3 个)");
    return true;
}

// ========================================================================
// 模式1 —— 单个圆环的数字识别（供多圆环循环复用）
// ========================================================================
// 步骤：moments 圆心 + minEnclosingCircle 半径 → 中心方形数字 ROI →
//       自适应二值化 → 开运算剥同心细环(核 ∝ 半径) →
//       取"靠近圆心的最大墨块" → matchShapes 与 3 模板对比(argmin)
// 任一阶段失败返回 false，调用方跳过该圆环（不影响其他圆环与 Kalman）。
// ========================================================================
bool VisionPipeline::recognizeRingDigit(
    const cv::Mat& blurred,
    const cv::Rect& roi_rect,
    const std::vector<cv::Point>& ring_contour,
    uint8_t& digit_id,
    cv::Point2f& center,
    float& radius,
    cv::Mat& digit_binary) const {
    auto& cfg = ConfigManager::instance();

    // ---- 圆心（moments）与半径（minEnclosingCircle）-------------------
    auto m = cv::moments(ring_contour);
    if (std::abs(m.m00) < 1e-6) {
        return false;
    }
    const float cx = static_cast<float>(m.m10 / m.m00);
    const float cy = static_cast<float>(m.m01 / m.m00);

    cv::Point2f dummy_center;
    float r = 0.0f;
    cv::minEnclosingCircle(ring_contour, dummy_center, r);
    if (r < 1.0f) {
        return false;
    }

    // ---- 数字 ROI：中心方形裁剪（半宽 = digit_roi_scale × 半径）---------
    // 靶标是"中心数字 + 多层同心细环"。取 0.5~0.75 的通用裁剪：
    // 太小会裁到数字（数字几乎与最内圈同大），太大会把外圈细环框进来。
    const int half =
        cvRound(static_cast<double>(cfg.mode1DigitRoiScale()) * r);
    if (half < 8) {
        return false;
    }
    cv::Rect digit_rect(cvRound(cx) - half, cvRound(cy) - half,
                        half * 2, half * 2);
    digit_rect &= cv::Rect(0, 0, blurred.cols, blurred.rows);
    if (digit_rect.width < 8 || digit_rect.height < 8) {
        return false;
    }
    // 从"高斯模糊后"的图像取数字 ROI：抑制传感器噪声/反光细碎点，
    // 与环级自适应阈值同源，结果更稳定
    cv::Mat digit_gray = blurred(digit_rect);

    // ---- 数字二值化（自适应阈值；暗结构为白前景，抗光照渐变）------------
    const int thresh_type = cfg.mode1InvertDigit()
                            ? cv::THRESH_BINARY
                            : cv::THRESH_BINARY_INV;
    int dblock = std::min(digit_gray.cols, digit_gray.rows) / 3;
    if (dblock % 2 == 0) dblock++;
    dblock = std::max(dblock, 15);
    dblock = std::min(dblock, 63);
    cv::Mat digit_bin;
    cv::adaptiveThreshold(digit_gray, digit_bin, 255,
                          cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          thresh_type, dblock,
                          cfg.mode1AdaptiveC());

    // ---- 开运算剥掉同心细环（核随半径缩放）------------------------------
    // 数字笔画粗、圆环线细；环线宽度与靶标半径成比例(随距离变化)，
    // 故开核取 digit_open_ratio × 半径，才能在远近一致地把细环剥掉、
    // 保留粗数字笔画。实测 ratio=0.05 通解。
    int do_ksize =
        cvRound(static_cast<double>(cfg.mode1DigitOpenRatio()) * r);
    if (do_ksize < 3) do_ksize = 3;
    if (do_ksize % 2 == 0) do_ksize++;
    cv::Mat do_kernel = cv::getStructuringElement(
        cv::MORPH_ELLIPSE, cv::Size(do_ksize, do_ksize));
    cv::morphologyEx(digit_bin, digit_bin, cv::MORPH_OPEN, do_kernel);

    // ---- 数字轮廓：取"靠近圆心的最大墨块" ------------------------------
    // 剥环后 ROI 内数字是唯一的粗结构。按面积最大者取整（面积优先、次中心近），
    // 用面积/中心偏移/宽高比门槛排除残余弧块与噪点。**不做贴边剔除**——
    // 数字常常撑满整幅 ROI，贴边是正常现象而非外框。
    std::vector<std::vector<cv::Point>> digit_contours;
    cv::findContours(digit_bin, digit_contours,
                     cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);
    if (digit_contours.empty()) {
        return false;
    }
    const double roi_area =
        static_cast<double>(digit_bin.rows) * digit_bin.cols;
    const std::vector<cv::Point>* digit_blob = nullptr;
    double digit_area  = 0.0;
    double best_offset = 1e9;
    for (const auto& c : digit_contours) {
        double a = cv::contourArea(c);
        if (a < roi_area * 0.005 || a > roi_area * 0.6) continue;

        auto mm = cv::moments(c);
        if (std::abs(mm.m00) < 1e-6) continue;
        double cxx = mm.m10 / mm.m00;
        double cyy = mm.m01 / mm.m00;
        double off = std::hypot(cxx - half, cyy - half) / half;
        if (off > 0.6) continue;                // 必须靠近 ROI 中心

        cv::Rect bb = cv::boundingRect(c);
        double aspect = static_cast<double>(bb.height) /
                        std::max(bb.width, 1);
        if (aspect < 0.7 || aspect > 3.5) continue;   // 排除横向环弧/长残块

        if (a > digit_area ||
            (std::abs(a - digit_area) < 1e-6 && off < best_offset)) {
            digit_area  = a;
            best_offset = off;
            digit_blob  = &c;
        }
    }
    if (!digit_blob) {
        return false;
    }

    // ---- Hu 矩模板匹配（matchShapes 旋转不变，argmin 三选一）----------
    // 该衬线字体下绝对匹配分与模板笔画粗细相关、偏高且同类/异类分差不稳，
    // 不做绝对阈值，直接取与三个模板中最小者对应的编号（实测 argmin 全对）。
    double best_score = std::numeric_limits<double>::max();
    int best_id = -1;
    for (size_t t = 0; t < template_contours_.size(); ++t) {
        double d = cv::matchShapes(*digit_blob, template_contours_[t],
                                   cv::CONTOURS_MATCH_I1, 0.0);
        if (d < best_score) {
            best_score = d;
            best_id = static_cast<int>(t) + 1;
        }
    }
    if (best_id < 1) {
        return false;
    }

    // ---- 输出（坐标转换回全图）----------------------------------------
    digit_id = static_cast<uint8_t>(best_id);
    center   = cv::Point2f(cx + roi_rect.x, cy + roi_rect.y);
    radius   = r;
    digit_binary = digit_bin;
    return true;
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
