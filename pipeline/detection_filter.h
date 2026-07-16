/// @file detection_filter.h
/// @brief 检测结果过滤器 — Confidence → Class → Top-K → ROI。
///
/// 流程:
///   YOLO Raw Detections
///     → Confidence Filter (> threshold)
///     → Class Filter (白名单)
///     → Top-K Filter (最多 max_targets 个)
///     → ROI Filter (可选, 边界检查)
///     → 输出过滤后的结果
///
/// 使用:
///   DetectionFilter filter;
///   filter.configure(cfg);
///   auto results = filter.apply(rawObjects);

#pragma once

#include "yolo/yolo26.h"
#include <vector>
#include <set>
#include <algorithm>

struct DetectFilterConfig {
    float confidenceThreshold = 0.45f;   ///< 最低置信度
    int   maxTargets          = 10;      ///< Top-K 保留数量
    bool  roiEnabled          = false;   ///< 是否启用 ROI 边界过滤
    int   roiX = 0, roiY = 0;
    int   roiW = 640, roiH = 480;
    std::set<int> allowedClasses = {0, 1, 2};  ///< 类别白名单
};

class DetectionFilter {
public:
    void configure(const DetectFilterConfig& cfg) { cfg_ = cfg; }

    /// 应用全部过滤, 返回通过过滤的检测结果
    std::vector<Object> apply(const std::vector<Object>& raw) const {
        auto v = raw;
        v = confidenceFilter(v);
        v = classFilter(v);
        v = topKFilter(v);
        if (cfg_.roiEnabled) v = roiFilter(v);
        return v;
    }

    /// 仅置信度过滤
    std::vector<Object> confidenceFilter(const std::vector<Object>& raw) const {
        std::vector<Object> out;
        for (const auto& o : raw) {
            if (o.confidence >= cfg_.confidenceThreshold) out.push_back(o);
        }
        return out;
    }

    /// 仅类别过滤
    std::vector<Object> classFilter(const std::vector<Object>& raw) const {
        if (cfg_.allowedClasses.empty()) return raw;
        std::vector<Object> out;
        for (const auto& o : raw) {
            if (cfg_.allowedClasses.count(o.label)) out.push_back(o);
        }
        return out;
    }

    /// Top-K 过滤 (按置信度降序取前 K 个)
    std::vector<Object> topKFilter(const std::vector<Object>& raw) const {
        if (static_cast<int>(raw.size()) <= cfg_.maxTargets) return raw;
        auto sorted = raw;
        std::sort(sorted.begin(), sorted.end(),
            [](const Object& a, const Object& b) { return a.confidence > b.confidence; });
        sorted.resize(static_cast<size_t>(cfg_.maxTargets));
        return sorted;
    }

    /// ROI 边界过滤 (中心点在 ROI 内才保留)
    std::vector<Object> roiFilter(const std::vector<Object>& raw) const {
        std::vector<Object> out;
        for (const auto& o : raw) {
            float cx = o.cx(), cy = o.cy();
            if (cx >= cfg_.roiX && cx <= cfg_.roiX + cfg_.roiW &&
                cy >= cfg_.roiY && cy <= cfg_.roiY + cfg_.roiH) {
                out.push_back(o);
            }
        }
        return out;
    }

    const DetectFilterConfig& getConfig() const { return cfg_; }

private:
    DetectFilterConfig cfg_;
};
