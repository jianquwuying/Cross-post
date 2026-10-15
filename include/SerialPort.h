#pragma once
// ============================================================================
// SerialPort.h  —  UART 通信（非阻塞）
// ============================================================================
// **仅** SerialThread 访问 UART 文件描述符。
// ProcessThread **必须**通过 ThreadSafeSendQueue 推送结果
// —— **严禁**直接调用 writeResult()。
//
// readCommand()   — 搜寻 0xCD … 0xDC 帧，返回 {MODE, TARGET}。
// writeResult()   — 格式化 9 字节数据包，非阻塞写入。
// ============================================================================

#include <cstdint>
#include <optional>
#include <string>

class SerialPort {
public:
    SerialPort()  = default;
    ~SerialPort();

    /// 打开并配置串口设备。
    /// 成功返回 true。失败则调用方应退出。
    bool open(const std::string& device, int baudrate);

    /// 关闭设备。
    void close();

    /// 查询打开状态。
    bool isOpen() const { return fd_ >= 0; }

    /// 获取文件描述符（供 SerialThread 中的 poll / select 使用）。
    int fd() const { return fd_; }

    /// 尝试从串口读取一个命令帧。
    /// 协议：0xCD  0x01  MODE  TARGET  0xDC  （5 字节）
    /// **TARGET 位保留但被忽略**（不校验、不使用），成功返回 MODE，
    /// 超时/无数据返回 std::nullopt。
    std::optional<uint8_t> readCommand();

    /// 向串口写入检测结果（非阻塞）。
    /// 协议：0xCD  0x06  TargetType  Color  X_H  X_L  Y_H  Y_L  0xDC
    /// TargetType：1=颜色物块 2=圆环 3=圆柱；0=无目标帧（X=Y=0，
    ///             Color 标识丢失的目标 ID），由 serial.enable_no_target_frame 控制。
    /// 全部 9 字节写入成功返回 true。
    bool writeResult(uint8_t target_type, uint8_t color,
                     uint16_t x, uint16_t y);

private:
    /// 配置 termios：8N1，无流控，原始模式。
    bool configurePort(int baudrate);

    int fd_ = -1;
};
