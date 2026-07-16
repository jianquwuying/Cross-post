/// @file protocol.h
/// @brief 最终版串口二进制协议 — 9字节固定长度, 0xCD/0xDC 帧边界。
///
/// 数据包格式（每目标 9 字节）:
/// ┌────────┬────────┬───────────┬───────┬─────────┬─────────┬────────┐
/// │ Header │ Length │ TargetType│ Color │ CenterX │ CenterY │ Tail   │
/// ├────────┼────────┼───────────┼───────┼─────────┼─────────┼────────┤
/// │ 0xCD   │ 0x06   │ 1 Byte    │1 Byte │ 2 Byte  │ 2 Byte  │ 0xDC   │
/// └────────┴────────┴───────────┴───────┴─────────┴─────────┴────────┘
///
/// Length 固定 6 (TargetType + Color + CenterX + CenterY)。
/// STM32 端使用状态机解析: 等待 0xCD → 读 Length → 读数据 → 读 0xDC。

#pragma once

#include <cstdint>
#include <vector>

namespace protocol {

// ── 常量 ──────────────────────────────────────────────────────────
constexpr uint8_t  PACKET_HEADER  = 0xCD;
constexpr uint8_t  PACKET_TAIL    = 0xDC;
constexpr uint8_t  PACKET_LENGTH  = 6;     // 数据区长度 (TargetType+Color+CX+CY)
constexpr uint8_t  PACKET_SIZE    = 9;     // 1+1+1+1+2+2+1

// ── 目标类型 ──────────────────────────────────────────────────────

enum class TargetType : uint8_t {
    Material  = 1,   ///< 物料 — 需要 HSV 全颜色识别
    ColorRing = 2,   ///< 色环 — 仅识别 RED/BLUE/GREEN
    Obstacle  = 3,   ///< 障碍物 — 不执行 HSV, 颜色固定 NONE
};

/// TargetType → 字符串
inline const char* targetTypeName(TargetType t) {
    switch (t) {
        case TargetType::Material:  return "Material";
        case TargetType::ColorRing: return "ColorRing";
        case TargetType::Obstacle:  return "Obstacle";
        default:                    return "Unknown";
    }
}

// ── 颜色类型 ──────────────────────────────────────────────────────

enum class ColorType : uint8_t {
    NONE   = 0,
    RED    = 1,
    YELLOW = 2,
    BLUE   = 3,
    GREEN  = 4,
    BLACK  = 5,
    CYAN   = 6,
};

/// ColorType → 字符串
inline const char* colorTypeName(ColorType c) {
    switch (c) {
        case ColorType::RED:    return "Red";
        case ColorType::YELLOW: return "Yellow";
        case ColorType::BLUE:   return "Blue";
        case ColorType::GREEN:  return "Green";
        case ColorType::BLACK:  return "Black";
        case ColorType::CYAN:   return "Cyan";
        default:                return "None";
    }
}

// ── 目标分类规则 ──────────────────────────────────────────────────

/// Material 有效颜色: 全色 1-6
inline bool isValidMaterialColor(ColorType c) {
    return c >= ColorType::RED && c <= ColorType::CYAN;
}

/// ColorRing 有效颜色: 仅 RED(1), BLUE(3), GREEN(4)
inline bool isValidColorRingColor(ColorType c) {
    return c == ColorType::RED || c == ColorType::BLUE || c == ColorType::GREEN;
}

// ── 编码器 ────────────────────────────────────────────────────────

/// 将单个检测目标编码为 9 字节数据包。
/// @param target  目标类型
/// @param color   颜色类型 (Obstacle 必须为 NONE)
/// @param cx      中心 X 坐标 (像素, 0-65535)
/// @param cy      中心 Y 坐标 (像素, 0-65535)
/// @return 9 字节数据包
inline std::vector<uint8_t> encodePacket(
    TargetType target, ColorType color,
    uint16_t cx, uint16_t cy)
{
    std::vector<uint8_t> pkt;
    pkt.reserve(PACKET_SIZE);
    pkt.push_back(PACKET_HEADER);                        // [0]
    pkt.push_back(PACKET_LENGTH);                        // [1]
    pkt.push_back(static_cast<uint8_t>(target));         // [2]
    pkt.push_back(static_cast<uint8_t>(color));          // [3]
    pkt.push_back(static_cast<uint8_t>((cx >> 8) & 0xFF)); // [4]
    pkt.push_back(static_cast<uint8_t>(cx & 0xFF));      // [5]
    pkt.push_back(static_cast<uint8_t>((cy >> 8) & 0xFF)); // [6]
    pkt.push_back(static_cast<uint8_t>(cy & 0xFF));      // [7]
    pkt.push_back(PACKET_TAIL);                          // [8]
    return pkt;
}

/// 将多个检测目标编码为连续字节流。
/// @param packets  每个元素为 {target, color, cx, cy}
/// @return 9*N 字节连续流
inline std::vector<uint8_t> encodePackets(
    const std::vector<std::tuple<TargetType, ColorType, uint16_t, uint16_t>>& packets)
{
    std::vector<uint8_t> frame;
    frame.reserve(packets.size() * PACKET_SIZE);
    for (const auto& [t, c, cx, cy] : packets) {
        auto pkt = encodePacket(t, c, cx, cy);
        frame.insert(frame.end(), pkt.begin(), pkt.end());
    }
    return frame;
}

} // namespace protocol
