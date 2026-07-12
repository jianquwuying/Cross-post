/// @file roi.h
/// @brief YOLO 检测前 ROI 区域裁剪模块。
///
/// 当摄像头视野大于实际关注区域时（如传送带仅占画面中央50%），
/// 裁剪后再进行 YOLO 推理可显著降低计算量、减少背景误检。
///
/// 坐标映射: 检测结果自动从裁剪坐标系映射回全帧坐标系。

#pragma once

#include <opencv2/core.hpp>

// ── ROI 配置 ──────────────────────────────────────────────────────

struct RoiConfig {
    bool enabled = false;     ///< 是否启用 ROI 裁剪
    int  x       = 0;         ///< ROI 左上角 X
    int  y       = 0;         ///< ROI 左上角 Y
    int  width   = 640;       ///< ROI 宽度
    int  height  = 480;       ///< ROI 高度
};

// ── ROI 裁剪器 ────────────────────────────────────────────────────

class RoiCrop {
public:
    RoiCrop() = default;

    /// 配置 ROI 参数
    void configure(const RoiConfig& cfg);

    /// 是否启用裁剪
    bool isEnabled() const { return config_.enabled; }

    /// 获取当前配置
    const RoiConfig& getConfig() const { return config_; }

    /// 应用 ROI 裁剪
    /// @param frame  输入全帧图像
    /// @return 裁剪后的图像（未启用时返回原图引用, 注意生命周期）
    cv::Mat apply(const cv::Mat& frame) const;

    /// 将检测坐标从裁剪坐标系映射回全帧坐标系
    /// @param box  裁剪坐标系中的边界框
    /// @return 全帧坐标系中的边界框（偏移了 ROI 左上角坐标）
    cv::Rect_<float> mapBack(const cv::Rect_<float>& box) const;

    /// 获取 ROI 区域（OpenCV 格式）
    cv::Rect getRect() const;

private:
    RoiConfig config_;
};
