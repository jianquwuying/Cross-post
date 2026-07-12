#include "roi/roi.h"

#include <algorithm>
#include <iostream>

// ── 配置 ──────────────────────────────────────────────────────────

void RoiCrop::configure(const RoiConfig& cfg) {
    config_ = cfg;

    // 边界检查
    if (config_.x < 0) config_.x = 0;
    if (config_.y < 0) config_.y = 0;
    if (config_.width  < 1) config_.width  = 1;
    if (config_.height < 1) config_.height = 1;

    if (config_.enabled) {
        std::cout << "[ROI] 裁剪启用: x=" << config_.x
                  << " y=" << config_.y
                  << " w=" << config_.width
                  << " h=" << config_.height << std::endl;
    }
}

// ── 裁剪 ──────────────────────────────────────────────────────────

cv::Mat RoiCrop::apply(const cv::Mat& frame) const {
    if (!config_.enabled || frame.empty()) {
        return frame;
    }

    // 确保 ROI 不超出帧范围
    int x = std::max(0, config_.x);
    int y = std::max(0, config_.y);
    int w = std::min(config_.width,  frame.cols - x);
    int h = std::min(config_.height, frame.rows - y);

    if (w <= 0 || h <= 0) return frame;

    cv::Rect roi(x, y, w, h);
    return frame(roi).clone();  // clone 确保数据独立
}

// ── 坐标映射 ──────────────────────────────────────────────────────

cv::Rect_<float> RoiCrop::mapBack(const cv::Rect_<float>& box) const {
    if (!config_.enabled) return box;

    // 裁剪坐标 + ROI 偏移 = 全帧坐标
    return cv::Rect_<float>(
        box.x + static_cast<float>(config_.x),
        box.y + static_cast<float>(config_.y),
        box.width,
        box.height
    );
}

// ── 获取 ROI 矩形 ─────────────────────────────────────────────────

cv::Rect RoiCrop::getRect() const {
    return cv::Rect(config_.x, config_.y, config_.width, config_.height);
}
