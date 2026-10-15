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
void VisionPipeline::setMode(uint8_t mode) {
    if (mode > 3) {
        LOG_WARN("无效模式 %d，已忽略", mode);
        return;
    }

    // 串口命令里的 TARGET 位已被忽略（帧格式保留，值不参与任何判断）
    const uint8_t old_mode = current_mode_.load();
    if (old_mode == mode) {
        return;   // 无变化 —— 不重置 Kalman
    }

    current_mode_.store(mode);

    LOG_INFO("模式切换: MODE%d → MODE%d", old_mode, mode);
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
// 未检出目标时的统一处理（无目标帧 / 预测坐标）
// ========================================================================
// 供模式0/1/2 在"本帧未检出目标"时调用：
//   · 开关关闭       → 不推任何结果（完全回退到"无目标不发送"）
//   · 跟踪器已初始化 → 推送 Kalman 预测坐标（target_type 保持 detected_type），
//                      保证主控侧坐标连续；这是外推值、会随时间漂移，
//                      主控应结合时间窗口判断可信度
//   · 跟踪器未初始化 → 推送无目标帧 {target_type=0, color_id, x=0, y=0}
//                      （含"冷启动从未捕获"与"超时复位"两种情况）
// ========================================================================
void VisionPipeline::pushLostOrNoTarget(std::vector<DetectionResult>& results,
                                        int key, uint8_t detected_type,
                                        uint8_t color_id,
                                        const cv::Point2f& predicted,
                                        int frame_cols, int frame_rows) {
    auto& cfg = ConfigManager::instance();

    if (!cfg.serialEnableNoTargetFrame()) {
        return;   // 开关关闭：完全回退，什么都不推
    }

    DetectionResult dr;
    dr.color_id = color_id;

    if (kalman_trackers_[key].initialized) {
        // 短暂丢失（Kalman 未超时）→ 推预测坐标，保持坐标连续性
        cv::Point2f p = predicted;
        p.x = std::max(0.0f, std::min(p.x,
                          static_cast<float>(frame_cols - 1)));
        p.y = std::max(0.0f, std::min(p.y,
                          static_cast<float>(frame_rows - 1)));
        dr.target_type = detected_type;
        dr.x           = static_cast<uint16_t>(p.x);
        dr.y           = static_cast<uint16_t>(p.y);
    } else {
        // 冷启动从未捕获 / 超时复位 → 无目标帧
        dr.target_type = 0;
        dr.x           = 0;
        dr.y           = 0;
    }

    results.push_back(dr);
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
        case 3: {   // 空闲：不检测、无跟踪目标
            std::vector<DetectionResult> results;
            if (ConfigManager::instance().serialEnableNoTargetFrame()) {
                DetectionResult dr;      // 无目标帧（Color=0）
                dr.target_type = 0;
                dr.color_id    = 0;
                dr.x           = 0;
                dr.y           = 0;
                results.push_back(dr);
            }
            return results;
        }
        default: return {};
    }
}

// ========================================================================
// 模式0 —— 6 色物块「逐槽位快照」
// ========================================================================
// 流水线：ROI → BGR→HSV → 逐色掩码 → 轮廓 → 质心 → Kalman 预测/更新
//
// **每帧固定输出 6 包**（槽位 = 颜色外部值 1=红 … 6=青，内部索引/Kalman key = 槽位-1）：
//   检出            → {type=1, color_id=槽位, x, y}（Kalman 滤波后）
//   未检出且未超时  → {type=1, color_id=槽位, x_pred, y_pred}（短时丢失顶住坐标）
//   未超时复位/冷启动 → {type=0, color_id=槽位, 0, 0}（0 值包）
// 后两种由 serial.enable_no_target_frame 控制（关闭时只播报真实检出）。
// 红色使用**双** HSV 区间（H 0-10 + H 170-180），通过 bitwise_or 合并。
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

    const int morph_ksize = cfg.mode0MorphKernelSize();
    cv::Mat morph_kernel = cv::getStructuringElement(
        cv::MORPH_RECT, cv::Size(morph_ksize, morph_ksize));

    const double min_area = static_cast<double>(cfg.mode0MinContourArea());

    // 二值化调试图：6 色掩码按各自 BGR 色彩合成到黑底
    cv::Mat combined_debug(roi.size(), CV_8UC3, cv::Scalar(0, 0, 0));
    const cv::Scalar kDebugColors[] = {
        cv::Scalar(0, 0, 255),     // 1 红
        cv::Scalar(0, 255, 255),   // 2 黄
        cv::Scalar(255, 0, 0),     // 3 蓝
        cv::Scalar(0, 255, 0),     // 4 绿
        cv::Scalar(128, 128, 128), // 5 黑（灰）
        cv::Scalar(255, 255, 0),   // 6 青
    };

    // ---- 逐色 6 个槽位：每色各推一包 -----------------------------------
    for (int c = 0; c < kNumColors; ++c) {
        const uint8_t slot = static_cast<uint8_t>(c) + 1;   // 协议值 1-6
        const char* color_name = kColorNames[c];

        // 掩码（红色双区间）+ 形态学开运算
        auto ranges = cfg.mode0ColorHSV(color_name);
        cv::Mat color_mask(roi.size(), CV_8UC1, cv::Scalar(0));
        for (const auto& r : ranges) {
            cv::Mat m;
            applyHSVMask(hsv, r, m);
            cv::bitwise_or(color_mask, m, color_mask);
        }
        cv::morphologyEx(color_mask, color_mask,
                         cv::MORPH_OPEN, morph_kernel);
        combined_debug.setTo(kDebugColors[c], color_mask);

        // 未检出（无轮廓/面积不足/质心无效）→ Kalman 预测；未超时发预测坐标，
        // 已超时/冷启动发 0 值包（开关包裹在 pushLostOrNoTarget 内）
        auto on_lost = [&]() {
            const cv::Point2f pred =
                predictAndUpdate(c, cv::Point2f(0, 0), false);
            pushLostOrNoTarget(results, c, 1, slot, pred,
                               frame.cols, frame.rows);
        };

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(color_mask, contours,
                         cv::RETR_EXTERNAL,
                         cv::CHAIN_APPROX_SIMPLE);
        if (contours.empty()) {
            on_lost();
            continue;
        }

        auto largest = std::max_element(
            contours.begin(), contours.end(),
            [](const std::vector<cv::Point>& a,
               const std::vector<cv::Point>& b) {
                return cv::contourArea(a) < cv::contourArea(b);
            });
        if (cv::contourArea(*largest) < min_area) {
            on_lost();
            continue;
        }

        auto moments = cv::moments(*largest);
        if (std::abs(moments.m00) < 1e-6) {
            on_lost();
            continue;
        }

        const float local_x = static_cast<float>(moments.m10 / moments.m00);
        const float local_y = static_cast<float>(moments.m01 / moments.m00);

        // 全图坐标 + Kalman 滤波
        cv::Point2f measurement(local_x + roi_rect.x, local_y + roi_rect.y);
        cv::Point2f filtered = predictAndUpdate(c, measurement, true);
        filtered.x = std::max(0.0f, std::min(filtered.x,
                              static_cast<float>(frame.cols - 1)));
        filtered.y = std::max(0.0f, std::min(filtered.y,
                              static_cast<float>(frame.rows - 1)));

        DetectionResult dr;
        dr.target_type = 1;                 // 1 = 颜色物块
        dr.color_id    = slot;              // 协议值 1-6
        dr.x           = static_cast<uint16_t>(filtered.x);
        dr.y           = static_cast<uint16_t>(filtered.y);
        results.push_back(dr);
    }

    setDebugImage(combined_debug);
    return results;
}

// ========================================================================
// 模式1 —— 多圆环数字识别（同心环剥除 + 归一化位图多角度 IoU）「优先级单包」
// ========================================================================
// 流水线：ROI → Gray → GaussianBlur → adaptiveThreshold →
//          MORPH_OPEN → findContours(RETR_EXTERNAL) → 面积过滤 →
//          形状过滤(圆度 + minAreaRect 长宽比 + 填充率，剔除线状/方形干扰) → 按面积降序 →
//          嵌套去重(包围盒包含 + 圆心距) → 取前 max_rings_per_frame 个 →
//          逐环：moments 圆心 + minEnclosingCircle 半径 → 中心方形数字 ROI →
//          数字自适应二值化 → 开运算剥同心细环(核∝半径) →
//          取靠近圆心的最大墨块 → 归一化位图与多角度模板比 IoU(取最大) →
//          统一出口按优先级 2 > 1 > 3 只推一包：检出→真实坐标；
//          未检出但未超时→预测坐标；三者都不可用→ type=0 0 值包
//
// **关键特性**：
//   - 视野内可同时出现多个圆环，各自识别数字编号
//   - **每帧只推 1 包**：按优先级 2 > 1 > 3 取第一个可用环号（Kalman key = 环号）
//       本帧识别到        → {type=2, color_id=环号, 滤波坐标}
//       未识别但未超时    → {type=2, color_id=环号, 预测坐标}
//       三者都不可用      → {type=0, color_id=2, 0, 0}
//     选择严格按环号（环2 的预测坐标可以压过环1 的真实检出）；
//     后两种由 serial.enable_no_target_frame 控制（关闭时只播报真实检出）
//   - 形状过滤先剔掉线状/条状/方形干扰，避免它们占用 max_rings_per_frame
//     名额把真靶标挤出前 N（如贯穿画面的桌边线、背景板/方形纸片）
//   - 同一物理靶标的同心环是断开的独立轮廓 → 嵌套去重后只留最大外环，
//     保证「一个物理靶标 = 一个候选」，不会误触发丢帧
//   - 同编号冲突（两个环判成同一编号）→ 该环号按"未检出"处理，其余环号照常
//   - 靶标为"中心数字 + 多层同心细环"；开核按半径缩放，距离无关地剥环
//   - 数字匹配改用"按外接框归一化的二值位图 + 多角度模板 IoU"
//     （旧 cv::matchShapes/Hu矩 实机验证不可用：见 loadDigitTemplates 注释）
// ========================================================================
std::vector<DetectionResult> VisionPipeline::processMode1(
    const cv::Mat& frame) {
    std::vector<DetectionResult> results;
    auto& cfg = ConfigManager::instance();

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
        // 无任何候选圆环 → 走统一出口（三个环按"未检出"推进，只推优先级最高的一包）
        reportMode1Rings(results, {}, frame.cols, frame.rows);
        return results;
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
        // 全部候选都被形状过滤掉 → 同样走统一出口
        reportMode1Rings(results, {}, frame.cols, frame.rows);
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

    // ---- 统一出口：按优先级 2 > 1 > 3 只推一包 --------------------------
    reportMode1Rings(results, rings, frame.cols, frame.rows);
    return results;
}

// ========================================================================
// 模式1 —— 统一出口：按优先级 2 > 1 > 3 只推一包
// ========================================================================
// 三个环的 Kalman 每帧都推进（命中→校正并记下滤波坐标；未命中→只推进丢失
// 计数），保证优先级链切换时状态新鲜；但**只推优先级最高的那一个可用环号**：
//   ① 本帧真实检出           → {type=2, color_id=环号, 滤波坐标}
//   ② 未检出但跟踪器未超时     → {type=2, color_id=环号, 预测坐标}
//   ③ 三个都不可用（冷启动/全超时）→ {type=0, color_id=2, 0, 0}（0 值包）
// 选择**严格按环号**：环2 的预测坐标可以压过环1 的真实检出。
// ②③ 受 serial.enable_no_target_frame 控制（关闭时只推真实检出）。
// 同编号冲突（两个环判成同一编号）→ 该环号本帧视为"未检出"，其余环号照常。
// ========================================================================
void VisionPipeline::reportMode1Rings(
    std::vector<DetectionResult>& results,
    const std::vector<RingDetection>& rings,
    int frame_cols, int frame_rows) {
    auto& cfg = ConfigManager::instance();

    // ---- 同编号冲突：某环号被判成两次 → 该环号本帧按"未检出"处理 ---------
    bool dup[4] = {false, false, false, false};   // 索引 = 环号 1-3
    for (size_t i = 0; i < rings.size(); ++i) {
        for (size_t j = i + 1; j < rings.size(); ++j) {
            if (rings[i].digit_id == rings[j].digit_id) {
                LOG_WARN("模式1：检测到重复数字编号 %d，该环号本帧按未检出处理",
                         rings[i].digit_id);
                if (rings[i].digit_id >= 1 && rings[i].digit_id <= 3) {
                    dup[rings[i].digit_id] = true;
                }
            }
        }
    }

    // ---- 推进三个环的 Kalman（不管最后推哪个，状态都要新鲜）--------------
    bool        detected[4]  = {false, false, false, false};
    cv::Point2f filtered[4];      // 命中 → 滤波后坐标
    cv::Point2f predicted[4];     // 未命中 → 仅预测值
    for (uint8_t id = 1; id <= 3; ++id) {
        const RingDetection* hit = nullptr;
        if (!dup[id]) {
            for (const auto& r : rings) {
                if (r.digit_id == id) {
                    hit = &r;
                    break;
                }
            }
        }
        if (hit) {
            cv::Point2f f = predictAndUpdate(id, hit->center, true);
            f.x = std::max(0.0f, std::min(f.x,
                          static_cast<float>(frame_cols - 1)));
            f.y = std::max(0.0f, std::min(f.y,
                          static_cast<float>(frame_rows - 1)));
            filtered[id] = f;
            detected[id] = true;
        } else {
            predicted[id] = predictAndUpdate(id, cv::Point2f(0, 0), false);
        }
    }

    // ---- 按优先级 2 > 1 > 3 选出第一包 -----------------------------------
    static const uint8_t kPriority[3] = {2, 1, 3};
    const bool fill = cfg.serialEnableNoTargetFrame();

    for (uint8_t id : kPriority) {
        if (detected[id]) {
            DetectionResult dr;
            dr.target_type = 2;                 // 2 = 圆环
            dr.color_id    = id;                // 环号 1/2/3
            dr.x           = static_cast<uint16_t>(filtered[id].x);
            dr.y           = static_cast<uint16_t>(filtered[id].y);
            results.push_back(dr);
            return;
        }
        if (fill && kalman_trackers_[id].initialized) {
            // 未超时 → 预测坐标（内部含开关判断与坐标钳制）
            pushLostOrNoTarget(results, id, 2, id, predicted[id],
                               frame_cols, frame_rows);
            return;
        }
    }

    // ---- 三个环都不可用（冷启动 / 全超时）→ 0 值包（Color=2）------------
    // pushLostOrNoTarget 在开关关闭时不推任何东西；此处 tracker[2] 必为未初始化，
    // 因此开关开启时它推的正是 {type=0, color_id=2, 0, 0}
    if (fill) {
        pushLostOrNoTarget(results, 2, 2, 2, cv::Point2f(0, 0),
                           frame_cols, frame_rows);
    }
}

// ========================================================================
// 模式1 —— 数字模板加载（仅一次）
// ========================================================================
// 模板统一处理成"归一化实心字形位图"，并为每个数字预生成多个旋转角实例
// （见 digit_match_angle_range / step）。判号时用位图 IoU 逐数字取最大，
// 避免 cv::matchShapes(Hu矩) 对笔画粗细/渲染风格的极端敏感性。
// ========================================================================
bool VisionPipeline::normalizeGlyph(const cv::Mat& bin, int size, cv::Mat& out) {
    std::vector<std::vector<cv::Point>> cnts;
    cv::findContours(bin.clone(), cnts, cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);
    if (cnts.empty()) {
        return false;
    }
    auto largest = std::max_element(
        cnts.begin(), cnts.end(),
        [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
            return cv::contourArea(a) < cv::contourArea(b);
        });
    const cv::Rect bb = cv::boundingRect(*largest);
    if (bb.width < 2 || bb.height < 2) {
        return false;
    }
    // 把该轮廓填充成实心字形，再按外接框拉伸归一化（blob 与模板同样处理，
    // 保证同一姿态下两者位图可比）
    std::vector<std::vector<cv::Point>> one{*largest};
    cv::Mat mask = cv::Mat::zeros(bin.size(), CV_8UC1);
    cv::drawContours(mask, one, -1, cv::Scalar(255), cv::FILLED);
    cv::resize(mask(bb), out, cv::Size(size, size), 0, 0, cv::INTER_AREA);
    cv::threshold(out, out, 127, 255, cv::THRESH_BINARY);
    return true;
}

double VisionPipeline::bitmapIoU(const cv::Mat& a, const cv::Mat& b) {
    if (a.size() != b.size() || a.empty()) {
        return 0.0;
    }
    double inter = 0.0, uni = 0.0;
    for (int y = 0; y < a.rows; ++y) {
        const uint8_t* pa = a.ptr<uint8_t>(y);
        const uint8_t* pb = b.ptr<uint8_t>(y);
        for (int x = 0; x < a.cols; ++x) {
            const bool va = pa[x] > 127;
            const bool vb = pb[x] > 127;
            if (va || vb) {
                ++uni;
                if (va && vb) ++inter;
            }
        }
    }
    return uni > 0.0 ? inter / uni : 0.0;
}

bool VisionPipeline::loadDigitTemplates() {
    auto& cfg = ConfigManager::instance();
    digit_templates_.clear();
    digit_templates_.reserve(3);

    const int size  = std::max(8, cfg.mode1DigitMatchSize());
    const int range = std::max(0, cfg.mode1DigitMatchAngleRange());
    const int step  = std::max(1, cfg.mode1DigitMatchAngleStep());

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

        cv::Mat base;
        if (!normalizeGlyph(bin, size, base)) {
            LOG_ERROR("数字模板无法归一化: %s", path.c_str());
            return false;
        }

        // 多角度实例：绕中心旋转后重新按外接框归一化
        const cv::Point2f c(size / 2.0f, size / 2.0f);
        std::vector<cv::Mat> insts;
        for (int a = -range; a <= range; a += step) {
            const cv::Mat M = cv::getRotationMatrix2D(c, a, 1.0);
            cv::Mat rot;
            cv::warpAffine(base, rot, M, base.size(), cv::INTER_LINEAR,
                           cv::BORDER_CONSTANT, cv::Scalar(0));
            cv::threshold(rot, rot, 127, 255, cv::THRESH_BINARY);
            cv::Mat norm;
            if (normalizeGlyph(rot, size, norm)) {
                insts.push_back(norm);
            }
        }
        if (insts.empty()) {
            LOG_ERROR("数字模板多角度实例生成失败: %s", path.c_str());
            return false;
        }
        digit_templates_.push_back(insts);
    }

    LOG_INFO("数字模板加载完成 (3 个数字 × 多角度实例)");
    return true;
}

// ========================================================================
// 模式1 —— 单个圆环的数字识别（供多圆环循环复用）
// ========================================================================
// 步骤：moments 圆心 + minEnclosingCircle 半径 → 中心方形数字 ROI →
//       自适应二值化 → 开运算剥同心细环(核 ∝ 半径) →
//       取"靠近圆心的最大墨块" → 归一化位图 vs 多角度模板 IoU（取最大）
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
    // 邻域 = ROI 短边 × digit_adaptive_block_ratio，且**必须随半径一起放大**：
    // 邻域小于数字笔画宽度时，自适应阈值会把笔画内部判成背景（大块实心
    // 前景被"掏空"），有效笔画宽度塌缩到 ≈ 邻域/2，随后被按半径缩放的
    // 剥环开运算整个抹掉，表现为大半径（目标近 / 分辨率高）时数字识别
    // 失败或判号错误。旧实现对邻域硬钳上界 63，与半径无关，大半径下必然
    // 掏空；此处只保留"邻域不得超出 ROI 自身"这一物理约束。
    const int dm = std::min(digit_gray.cols, digit_gray.rows);
    int dblock = cvRound(cfg.mode1DigitAdaptiveBlockRatio() * dm);
    if (dblock % 2 == 0) dblock++;
    dblock = std::max(dblock, 15);
    const int dmax = (dm % 2 == 0) ? dm - 1 : dm;   // 不超过 ROI 短边的最大奇数
    dblock = std::min(dblock, dmax);
    if (dblock < 3) {
        return false;   // ROI 过小，无法可靠二值化
    }
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

        // 长宽比用 minAreaRect（最小面积外接矩形）而非 boundingRect：
        // boundingRect 是**轴对齐**的，同一个数字旋转后长宽比会随之改变 ——
        // 数字"1"横过来（旋转约 90°）时被判成"过扁"而剔除，形成约
        // 60°~110° 的旋转盲区。minAreaRect 跟随形状自身主方向，长宽比与
        // 旋转无关。判据只取"长边/短边 ≤ 上限"，即可在任意姿态下剔除
        // 线状环弧与长条残块（旧式的"下限 0.7"正是轴对齐假设的产物：
        // 它只拦横向长条，对斜向长条完全无效）。
        const cv::RotatedRect rr = cv::minAreaRect(c);
        const double side_a = rr.size.width;
        const double side_b = rr.size.height;
        if (side_a < 1.0 || side_b < 1.0) continue;          // 退化形状
        const double aspect =
            std::max(side_a, side_b) / std::min(side_a, side_b);
        if (aspect > 3.5) continue;                          // 剔除线状/长条

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

    // ---- 归一化位图 + 多角度 IoU 匹配（argmax 三选一）------------------
    // 不用 cv::matchShapes：实机验证其 I1 距离对"笔画粗细/渲染风格"极端敏感
    // —— 真实相机数字与任一模板的距离都 ≈6，而 2/3 两个模板之间仅 1.2，
    // 判号实际被噪声主导（±30° 旋转就会把 2 判成 3）。改为把数字墨块与模板
    // 都按外接框归一化成 size² 二值位图，模板预生成多角度实例，取 IoU 最大者；
    // 同一姿态下正确数字约 0.70、次优约 0.44（≈2 倍裕量），旋转由角度实例覆盖。
    const int msize = std::max(8, cfg.mode1DigitMatchSize());
    cv::Mat norm_blob;
    {
        const cv::Rect bb = cv::boundingRect(*digit_blob);
        if (bb.width < 2 || bb.height < 2) {
            return false;
        }
        cv::Mat blob_mask = cv::Mat::zeros(digit_bin.size(), CV_8UC1);
        std::vector<std::vector<cv::Point>> one{*digit_blob};
        cv::drawContours(blob_mask, one, -1, cv::Scalar(255), cv::FILLED);
        cv::resize(blob_mask(bb), norm_blob, cv::Size(msize, msize), 0, 0,
                   cv::INTER_AREA);
        cv::threshold(norm_blob, norm_blob, 127, 255, cv::THRESH_BINARY);
    }

    double best_score = -1.0;   // IoU 越大越像
    int best_id = -1;
    double score_by_digit[3] = {0.0, 0.0, 0.0};
    for (size_t t = 0; t < digit_templates_.size(); ++t) {
        for (const auto& inst : digit_templates_[t]) {
            const double s = bitmapIoU(norm_blob, inst);
            if (t < 3 && s > score_by_digit[t]) {
                score_by_digit[t] = s;
            }
            if (s > best_score) {
                best_score = s;
                best_id = static_cast<int>(t) + 1;
            }
        }
    }
    if (best_id < 1) {
        return false;
    }

    // ---- 置信度门槛：最高分与次高分（不同数字）之差过小 → 拒答 --------------
    // 严重离焦/过曝时数字墨块会退化，正确数字的 IoU 可能被别的数字反超。
    // 实测正确样本的裕量最小 +0.083（中位 +0.164），而唯一一例误判裕量为
    // −0.056：设一个很小的门槛就能把"判错"变成"没判出来"，宁可少报不可报错。
    std::vector<double> ranked(score_by_digit, score_by_digit + 3);
    std::sort(ranked.begin(), ranked.end(),
              [](double a, double b) { return a > b; });
    const double margin = ranked[0] - ranked[1];
    LOG_DEBUG("模式1: 数字匹配 → %d (IoU=%.3f 裕量=%.3f; 1=%.3f 2=%.3f 3=%.3f)",
              best_id, best_score, margin, score_by_digit[0], score_by_digit[1],
              score_by_digit[2]);
    const double min_margin = cfg.mode1DigitMinMargin();
    if (min_margin > 0.0 && margin < min_margin) {
        LOG_DEBUG("模式1: 判号裕量 %.3f < %.3f，拒答（该环不判号）",
                  margin, min_margin);
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
        const cv::Point2f pred =
            predictAndUpdate(0, cv::Point2f(0, 0), false);
        pushLostOrNoTarget(results, 0, 3, 0, pred, frame.cols, frame.rows);
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
        const cv::Point2f pred =
            predictAndUpdate(0, cv::Point2f(0, 0), false);
        pushLostOrNoTarget(results, 0, 3, 0, pred, frame.cols, frame.rows);
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
