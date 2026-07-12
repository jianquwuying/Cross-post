/// @file protocol.h
/// @brief 视觉系统 → MCU 串口链路的二进制通信协议。
///
/// 数据包格式（每个检测目标 12 字节，自包含）:
/// ┌──────┬──────┬──────┬───────┬───────┬──────────┬───────┬───────┬──────┬──────┬──────┐
/// │ 0xAA │ 0x55 │ 数量 │ 类别  │ 颜色  │ 颜色置信 │ cx_hi │ cx_lo │ cy_hi│ cy_lo│ 置信 │
/// ├──────┴──────┴──────┴───────┴───────┴──────────┴───────┴───────┴──────┴──────┴──────┤
/// │ CRC8 │
/// └──────┘
/// 总计 12 字节。每个检测包自包含同步头和 CRC8 校验，
/// 支持在数据流损坏时进行鲁棒的重新同步。
///
/// 多目标帧格式: [帧头] [包1] [包2] ... [包N]

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace protocol {

// ── 常量 ──────────────────────────────────────────────────────────
constexpr uint8_t  FRAME_HEADER0   = 0xAA;       ///< 帧同步头字节0
constexpr uint8_t  FRAME_HEADER1   = 0x55;       ///< 帧同步头字节1
constexpr uint8_t  DETECTION_SIZE  = 12;         ///< 每个检测包的字节数
constexpr uint8_t  MAX_DETECTIONS  = 32;         ///< 每帧最大目标数
constexpr uint16_t COORD_MAX       = 10000;      ///< 坐标归一化范围 (0-10000)

// ── 枚举 ──────────────────────────────────────────────────────────

/// 目标类别标签（YOLO 输出）
enum class ObjectClass : uint8_t {
    A = 0,   ///< A类 — 需要颜色分类
    B = 1,   ///< B类 — 不需要颜色
};

/// 颜色分类结果（仅用于 A 类）
enum class ObjectColor : uint8_t {
    Unknown = 0,   ///< 未知
    Red     = 1,   ///< 红色
    Blue    = 2,   ///< 蓝色
    Yellow  = 3,   ///< 黄色
};

// ── 检测数据包 ────────────────────────────────────────────────────
#pragma pack(push, 1)
struct DetectionPacket {
    uint8_t  header0;     ///< 同步头 0xAA
    uint8_t  header1;     ///< 同步头 0x55
    uint8_t  count;       ///< 本帧检测目标数量
    uint8_t  obj_class;   ///< 类别 (ObjectClass 枚举值)
    uint8_t  color;       ///< 颜色 (ObjectColor 枚举值)
    uint8_t  color_conf;  ///< 颜色置信度 × 100 (0-100), 仅对 A 类有效
    uint16_t cx;          ///< 中心点 X 坐标 (0-10000, 大端序)
    uint16_t cy;          ///< 中心点 Y 坐标 (0-10000, 大端序)
    uint8_t  confidence;  ///< YOLO 检测置信度 × 100 (0-100)
    uint8_t  crc8;        ///< CRC-8 校验值, 覆盖字节 [2..10]

    /// 序列化为线序字节（大端序, 12 字节）。
    /// 调用前需先计算 CRC8。
    void toBytes(uint8_t* buf, size_t bufSize) const {
        (void)bufSize; // 期望 DETECTION_SIZE
        buf[0]  = header0;
        buf[1]  = header1;
        buf[2]  = count;
        buf[3]  = obj_class;
        buf[4]  = color;
        buf[5]  = color_conf;
        buf[6]  = static_cast<uint8_t>((cx >> 8) & 0xFF);
        buf[7]  = static_cast<uint8_t>(cx & 0xFF);
        buf[8]  = static_cast<uint8_t>((cy >> 8) & 0xFF);
        buf[9]  = static_cast<uint8_t>(cy & 0xFF);
        buf[10] = confidence;
        buf[11] = crc8;
    }

    /// 对字节 [2..10]（9 个数据字节）计算 CRC8 并填入 crc8 字段
    void computeCRC() {
        uint8_t data[9] = {
            count, obj_class, color, color_conf,
            static_cast<uint8_t>((cx >> 8) & 0xFF),
            static_cast<uint8_t>(cx & 0xFF),
            static_cast<uint8_t>((cy >> 8) & 0xFF),
            static_cast<uint8_t>(cy & 0xFF),
            confidence
        };
        crc8 = calculateCRC8(data, 9);
    }

    /// 验证 CRC8 完整性
    bool verifyCRC() const {
        uint8_t data[9] = {
            count, obj_class, color, color_conf,
            static_cast<uint8_t>((cx >> 8) & 0xFF),
            static_cast<uint8_t>(cx & 0xFF),
            static_cast<uint8_t>((cy >> 8) & 0xFF),
            static_cast<uint8_t>(cy & 0xFF),
            confidence
        };
        uint8_t computed = calculateCRC8(data, 9);
        return computed == crc8;
    }

private:
    /// CRC-8-ATM 多项式: x^8 + x^2 + x + 1 (0x07)
    static uint8_t calculateCRC8(const uint8_t* data, size_t len) {
        uint8_t crc = 0x00;
        for (size_t i = 0; i < len; ++i) {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit) {
                if (crc & 0x80)
                    crc = static_cast<uint8_t>((crc << 1) ^ 0x07);
                else
                    crc = static_cast<uint8_t>(crc << 1);
            }
        }
        return crc;
    }
};
#pragma pack(pop)

static_assert(sizeof(DetectionPacket) == 12, "DetectionPacket must be 12 bytes");

// ── 帧构建器 ──────────────────────────────────────────────────────

/// 从检测包列表构建完整帧缓冲区。
/// 返回可直接发送的字节向量。
inline std::vector<uint8_t> buildFrame(const std::vector<DetectionPacket>& packets) {
    std::vector<uint8_t> frame;
    frame.reserve(1 + packets.size() * DETECTION_SIZE);

    // 帧级别同步头
    frame.push_back(FRAME_HEADER0);
    frame.push_back(FRAME_HEADER1);

    for (auto& pkt : packets) {
        // const_cast: 序列化前填充 CRC
        const_cast<DetectionPacket&>(pkt).computeCRC();
        uint8_t buf[DETECTION_SIZE];
        pkt.toBytes(buf, DETECTION_SIZE);
        frame.insert(frame.end(), buf, buf + DETECTION_SIZE);
    }

    return frame;
}

/// 便捷函数：创建单个检测包（自动计算 CRC）
inline DetectionPacket makePacket(
    ObjectClass cls, ObjectColor color,
    float cxNorm, float cyNorm,
    float confidence,           // YOLO 检测置信度 [0.0, 1.0]
    float colorConfidence = 0.0f,  // 颜色置信度 [0.0, 1.0]（仅 A 类有效）
    uint8_t totalCount = 1)
{
    DetectionPacket pkt{};
    pkt.header0     = FRAME_HEADER0;
    pkt.header1     = FRAME_HEADER1;
    pkt.count       = totalCount;
    pkt.obj_class   = static_cast<uint8_t>(cls);
    pkt.color       = static_cast<uint8_t>(color);
    pkt.color_conf  = static_cast<uint8_t>(colorConfidence * 100.0f);
    pkt.cx          = static_cast<uint16_t>(cxNorm * COORD_MAX);
    pkt.cy          = static_cast<uint16_t>(cyNorm * COORD_MAX);
    pkt.confidence  = static_cast<uint8_t>(confidence * 100.0f);
    pkt.computeCRC();
    return pkt;
}

} // namespace protocol
