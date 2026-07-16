/// @file packet_encoder.h
/// @brief DetectionResult → 9字节二进制包的编码器。
///
/// 职责:
///   - Header (0xCD) + Length (0x06) + TargetType + Color + CenterX + CenterY + Tail (0xDC)
///   - 禁止业务代码直接拼接协议字节
///
/// 使用:
///   PacketEncoder encoder;
///   auto packet = encoder.encode(result);   // 单个目标 → 9字节
///   auto frame  = encoder.encode(results);  // 多个目标 → 9*N字节

#pragma once

#include "protocol/protocol.h"
#include <cstdint>
#include <vector>

// ── 统一检测结果 ──────────────────────────────────────────────────

struct DetectionResult {
    protocol::TargetType target = protocol::TargetType::Obstacle;
    protocol::ColorType  color  = protocol::ColorType::NONE;
    float  confidence       = 0.0f;   ///< YOLO 检测置信度
    float  colorConfidence  = 0.0f;   ///< HSV 颜色置信度 (Obstacle 恒为 0)
    int    cx = 0;                    ///< 中心 X (像素)
    int    cy = 0;                    ///< 中心 Y (像素)
};

// ── 编码器 ────────────────────────────────────────────────────────

class PacketEncoder {
public:
    /// 编码单个 DetectionResult → 9 字节
    static std::vector<uint8_t> encode(const DetectionResult& r) {
        return protocol::encodePacket(
            r.target,
            r.color,
            static_cast<uint16_t>(r.cx),
            static_cast<uint16_t>(r.cy)
        );
    }

    /// 编码多个 DetectionResult → 9*N 字节连续流
    static std::vector<uint8_t> encode(const std::vector<DetectionResult>& results) {
        std::vector<std::tuple<
            protocol::TargetType,
            protocol::ColorType,
            uint16_t, uint16_t>> packets;
        packets.reserve(results.size());
        for (const auto& r : results) {
            packets.emplace_back(r.target, r.color,
                static_cast<uint16_t>(r.cx),
                static_cast<uint16_t>(r.cy));
        }
        return protocol::encodePackets(packets);
    }
};
