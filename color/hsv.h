/// @file hsv.h
/// @brief 针对检测目标的 HSV 颜色分类。
///
/// 仅对 A 类目标进行颜色判别（YOLO 检测 A/B, OpenCV 判断 A 的颜色）。
///
/// 策略:
///   1. YOLO 检测到 A 类 → 裁剪 ROI
///   2. BGR → HSV 色彩空间转换
///   3. 对每种颜色应用掩膜 + 形态学滤波
///   4. 计算 ROI 内各颜色像素占比
///   5. 主导颜色占比 > 阈值 → 判定为该颜色
///
/// 可处理: 光照变化、阴影、反光、红色 hue 环绕（0°/180°）。

#pragma once

#include <opencv2/core.hpp>

#include <string>
#include <vector>

// ── 颜色枚举 ──────────────────────────────────────────────────────

/// 颜色分类结果
enum class Color : uint8_t {
    Unknown = 0,   ///< 未知
    Red     = 1,   ///< 红色
    Blue    = 2,   ///< 蓝色
    Yellow  = 3,   ///< 黄色
};

/// 颜色名称（便于调试输出）
inline const char* colorName(Color c) {
    switch (c) {
        case Color::Red:     return "红";
        case Color::Blue:    return "蓝";
        case Color::Yellow:  return "黄";
        default:             return "未知";
    }
}

// ── HSV 分类结果 ──────────────────────────────────────────────────

/// 单次颜色分类的详细结果
struct ColorResult {
    Color  color      = Color::Unknown;   ///< 主导颜色
    float  confidence = 0.0f;             ///< 主导颜色像素占比
    float  ratioRed   = 0.0f;             ///< 红色像素占比
    float  ratioBlue  = 0.0f;             ///< 蓝色像素占比
    float  ratioYellow= 0.0f;             ///< 黄色像素占比
};

// ── HSV 分类器 ────────────────────────────────────────────────────

class HSVClassifier {
public:
    /// 单种颜色的 HSV 范围
    struct HSVRange {
        cv::Scalar low;   ///< HSV 下界
        cv::Scalar high;  ///< HSV 上界
    };

    /// 分类配置
    struct Config {
        // ── 颜色判定阈值 (0.0 - 1.0) ─────────────────────────
        // 当某颜色像素占比超过此阈值时, 认为该颜色"存在"。
        float colorThreshold = 0.15f;

        // ── 颜色置信度最低阈值 ───────────────────────────────
        // 输出颜色结果的最低置信度。低于此值时报告 Color::Unknown,
        // 避免在光照不足/反光/阴影情况下输出不可靠的颜色判定。
        float colorConfThreshold = 0.15f;

        // ── ROI 最小面积 ─────────────────────────────────────
        // 如果裁剪出的 ROI 小于此值, 跳过分类。
        int minROIArea = 100;  // 像素

        // ── 形态学核大小 ─────────────────────────────────────
        int morphKernelSize = 3;

        // ── 高斯模糊核大小（预处理） ─────────────────────────
        int blurKernelSize = 3;

        // ── 各颜色 HSV 范围 ──────────────────────────────────
        // 红色: 两个范围, 因为 hue 在 0°/180° 处环绕
        HSVRange red1    = { cv::Scalar(  0, 100, 100), cv::Scalar( 10, 255, 255) };
        HSVRange red2    = { cv::Scalar(170, 100, 100), cv::Scalar(180, 255, 255) };
        HSVRange blue    = { cv::Scalar(100, 100,  50), cv::Scalar(130, 255, 255) };
        HSVRange yellow  = { cv::Scalar( 20, 100, 100), cv::Scalar( 35, 255, 255) };
    };

    HSVClassifier() = default;
    explicit HSVClassifier(const Config& config) : config_(config) {}

    /// 对 BGR 格式的 ROI 进行主导颜色分类
    /// @param bgrROI  从原始 BGR 帧中裁剪的区域
    /// @return 包含各颜色占比的完整分类结果
    ColorResult classify(const cv::Mat& bgrROI) const;

    /// 分类并只返回颜色枚举值（便捷接口）
    Color classifyColor(const cv::Mat& bgrROI) const {
        return classify(bgrROI).color;
    }

    /// 获取/设置配置
    const Config& getConfig() const { return config_; }
    void setConfig(const Config& c) { config_ = c; }

    /// 动态调整 HSV 范围（运行时标定）
    void setRedRange(  const HSVRange& r1, const HSVRange& r2) { config_.red1 = r1; config_.red2 = r2; }
    void setBlueRange( const HSVRange& r)  { config_.blue   = r; }
    void setYellowRange(const HSVRange& r) { config_.yellow = r; }

    /// 从 HSV 图像中计算指定颜色范围的像素占比（静态工具函数）
    static float colorRatio(const cv::Mat& hsvROI,
                            const cv::Scalar& low,
                            const cv::Scalar& high,
                            int morphKernelSize = 3);

private:
    Config config_;
};
