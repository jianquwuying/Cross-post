#pragma once
// ============================================================================
// ColorOrder.h  —  模式0 颜色定义与顺序
// ============================================================================
// 定义模式0 的颜色集合与固定顺序：RED(0) … CYAN(5) 对应协议外部编号 1 … 6，
// 与 config/vision.yaml 中 mode0.colors 下 red/yellow/blue/green/black/cyan
// 的书写顺序一致。
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
    "red",    // 内部 0  红 → 外部编号 1
    "yellow", // 内部 1  黄 → 外部编号 2
    "blue",   // 内部 2  蓝 → 外部编号 3
    "green",  // 内部 3  绿 → 外部编号 4
    "black",  // 内部 4  黑 → 外部编号 5
    "cyan",   // 内部 5  青 → 外部编号 6
};

constexpr int kNumColors = 6;

// ---------------------------------------------------------------------------
// 外部颜色编号（串口协议 Color 字段，1-6）→ 颜色名
//   内部枚举、kColorNames 下标、Kalman key 一律 0-5；
//   模式0 对外使用 1-6（= 内部索引 + 1）；
//   模式1 的 Color 字段是环号 1-3；模式2/3 的 Color 字段固定为 0。
//   越界返回 "unknown"，避免数组越界。
// ---------------------------------------------------------------------------
constexpr const char* colorNameByExternalId(uint8_t external_id) {
    return (external_id >= 1 &&
            external_id <= static_cast<uint8_t>(kNumColors))
               ? kColorNames[external_id - 1]
               : "unknown";
}
