#include "color/hsv.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <iostream>

// ── 公开接口 ──────────────────────────────────────────────────────

ColorResult HSVClassifier::classify(const cv::Mat& bgrROI) const {
    ColorResult result;

    // ── 合法性检查 ────────────────────────────────────────────
    if (bgrROI.empty() ||
        bgrROI.cols * bgrROI.rows < config_.minROIArea) {
        return result;
    }

    // ── 步骤1: 高斯模糊降噪 ───────────────────────────────────
    cv::Mat blurred;
    int ksize = config_.blurKernelSize;
    if (ksize % 2 == 0) ksize += 1;  // 核大小必须为奇数
    cv::GaussianBlur(bgrROI, blurred, cv::Size(ksize, ksize), 0);

    // ── 步骤2: BGR → HSV 转换 ─────────────────────────────────
    cv::Mat hsv;
    cv::cvtColor(blurred, hsv, cv::COLOR_BGR2HSV);

    // ── 步骤3: 逐颜色计算像素占比 ─────────────────────────────
    const int mksz = config_.morphKernelSize;

    result.ratioRed    = colorRatio(hsv, config_.red1.low, config_.red1.high, mksz)
                       + colorRatio(hsv, config_.red2.low, config_.red2.high, mksz);
    // 红色占比上限为 1.0（两个范围理论上可能重叠）
    result.ratioRed    = std::min(result.ratioRed, 1.0f);

    result.ratioBlue   = colorRatio(hsv, config_.blue.low,   config_.blue.high,   mksz);
    result.ratioYellow = colorRatio(hsv, config_.yellow.low, config_.yellow.high, mksz);

    // ── 步骤4: 判定主导颜色 ───────────────────────────────────
    // 找出超过颜色置信度阈值且占比最高的颜色
    struct Candidate { Color c; float r; };
    std::vector<Candidate> candidates = {
        { Color::Red,    result.ratioRed    },
        { Color::Blue,   result.ratioBlue   },
        { Color::Yellow, result.ratioYellow },
    };

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.r > b.r;
              });

    if (candidates[0].r >= config_.colorConfThreshold) {
        result.color      = candidates[0].c;
        result.confidence = candidates[0].r;
    }

    return result;
}

// ── 颜色像素占比计算 ──────────────────────────────────────────────

float HSVClassifier::colorRatio(const cv::Mat& hsvROI,
                                 const cv::Scalar& low,
                                 const cv::Scalar& high,
                                 int morphKernelSize) {
    // ── 创建 HSV 掩膜 ─────────────────────────────────────────
    cv::Mat mask;
    cv::inRange(hsvROI, low, high, mask);

    // ── 形态学操作 ────────────────────────────────────────────
    // 开运算: 去除小噪点 (腐蚀 → 膨胀)
    // 闭运算: 填充小孔洞 (膨胀 → 腐蚀)
    if (morphKernelSize > 0) {
        cv::Mat kernel = cv::getStructuringElement(
            cv::MORPH_ELLIPSE,
            cv::Size(morphKernelSize, morphKernelSize));

        cv::morphologyEx(mask, mask, cv::MORPH_OPEN,  kernel);
        cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);
    }

    // ── 计算像素占比 ─────────────────────────────────────────
    int colorPixels = cv::countNonZero(mask);
    int totalPixels = hsvROI.rows * hsvROI.cols;

    if (totalPixels == 0) return 0.0f;

    return static_cast<float>(colorPixels) / static_cast<float>(totalPixels);
}
