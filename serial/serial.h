/// @file serial.h
/// @brief 基于 POSIX termios 的 UART 串口通信。
///
/// 使用 Linux 原生 termios API —— 无需外部库依赖。
/// 专为 Raspberry Pi 5 UART 设计 (/dev/ttyAMA0, /dev/ttyS0, /dev/ttyUSB*)。
///
/// 发送 (TX): Raspberry Pi → MCU (STM32/Arduino/机器人控制器)
/// 接收 (RX): 预留双向通信接口

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <atomic>

class SerialPort {
public:
    /// 串口配置
    struct Config {
        std::string device   = "/dev/ttyAMA0";  ///< 设备节点
        int         baudrate = 115200;           ///< 波特率
        int         dataBits = 8;                ///< 数据位: 5, 6, 7, 8
        int         stopBits = 1;                ///< 停止位: 1 或 2
        char        parity   = 'N';              ///< 校验位: 'N', 'E', 'O'
        bool        hardwareFlowControl = false; ///< 硬件流控 RTS/CTS
        /// 发送超时 (分秒, 0 = 无超时)
        int         txTimeout = 5;
        /// 接收超时 (分秒, 0 = 非阻塞)
        int         rxTimeout = 5;
    };

    SerialPort() = default;
    ~SerialPort();

    // ── 不可拷贝，可移动 ────────────────────────────────────────
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    SerialPort(SerialPort&&) noexcept;
    SerialPort& operator=(SerialPort&&) noexcept;

    /// 使用默认配置打开串口
    bool open() { return open(Config{}); }
    /// 使用指定配置打开串口
    bool open(const Config& config);

    /// 检查串口是否已打开
    bool isOpen() const { return fd_ >= 0; }

    /// 发送原始字节
    /// @return 写入的字节数, -1 表示错误
    ssize_t write(const uint8_t* data, size_t len);

    /// 发送字节向量（便捷接口）
    ssize_t write(const std::vector<uint8_t>& data);

    /// 接收原始字节（带超时的阻塞模式）
    /// @return 读取的字节数, 0 表示超时, -1 表示错误
    ssize_t read(uint8_t* buf, size_t maxLen);

    /// 接收字节到向量
    ssize_t read(std::vector<uint8_t>& buf, size_t maxLen);

    /// 清空发送/接收缓冲区
    void flush();

    /// 关闭串口
    void close();

    /// 获取当前配置
    const Config& getConfig() const { return config_; }

    /// 获取文件描述符（高级用途）
    int getFD() const { return fd_; }

private:
    /// 根据 Config 配置 termios 参数
    bool configureTermios();

    int    fd_ = -1;
    Config config_;
};
