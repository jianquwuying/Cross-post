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
