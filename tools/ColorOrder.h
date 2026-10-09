#pragma once
// ============================================================================
// ColorOrder.h  —  模式0 颜色定义与顺序
// ============================================================================

#include <cstdint>

enum class ColorID : uint8_t {
    RED    = 0,
    YELLOW = 1,
    BLUE   = 2,
    GREEN  = 3,
    BLACK  = 4,
    CYAN   = 5,
};

constexpr const char* kColorNames[] = {
    "red",    // 0  红
    "yellow", // 1  黄
    "blue",   // 2  蓝
    "green",  // 3  绿
    "black",  // 4  黑
    "cyan",   // 5  青
};

constexpr int kNumColors = 6;

// ---------------------------------------------------------------------------
// 外部颜色编号（串口协议用，1-6）→ 颜色名
//   内部数组索引 / Kalman key 一律 0-5；DetectionResult.color_id、串口 Color
//   字段一律 0-6（0 表示无目标帧的空闲）。越界返回 "unknown"，避免数组越界。
// ---------------------------------------------------------------------------
constexpr const char* colorNameByExternalId(uint8_t external_id) {
    return (external_id >= 1 &&
            external_id <= static_cast<uint8_t>(kNumColors))
               ? kColorNames[external_id - 1]
               : "unknown";
}
