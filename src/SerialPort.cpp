// ============================================================================
// SerialPort.cpp  —  UART 通信实现
// ============================================================================

#include "SerialPort.h"
#include "Logger.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// 析构函数
// ---------------------------------------------------------------------------
SerialPort::~SerialPort() {
    close();
}

// ---------------------------------------------------------------------------
// 打开串口
// ---------------------------------------------------------------------------
bool SerialPort::open(const std::string& device, int baudrate) {
    LOG_INFO("正在打开串口: %s @ %d 波特", device.c_str(), baudrate);

    fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        LOG_ERROR("无法打开 %s: %s", device.c_str(), std::strerror(errno));
        return false;
    }

    if (!configurePort(baudrate)) {
        LOG_ERROR("无法配置串口");
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // 清空残留数据
    tcflush(fd_, TCIOFLUSH);

    LOG_INFO("串口已成功打开");
    return true;
}

// ---------------------------------------------------------------------------
// 关闭串口
// ---------------------------------------------------------------------------
void SerialPort::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        LOG_INFO("串口已关闭");
    }
}

// ---------------------------------------------------------------------------
// 配置串口 —— 8N1，无流控，原始模式
// ---------------------------------------------------------------------------
bool SerialPort::configurePort(int baudrate) {
    struct termios tty;
    std::memset(&tty, 0, sizeof(tty));

    if (tcgetattr(fd_, &tty) != 0) {
        LOG_ERROR("tcgetattr 失败: %s", std::strerror(errno));
        return false;
    }

    // 波特率 → 速度常量映射
    speed_t speed = B115200;
    switch (baudrate) {
        case 9600:    speed = B9600;    break;
        case 19200:   speed = B19200;   break;
        case 38400:   speed = B38400;   break;
        case 57600:   speed = B57600;   break;
        case 115200:  speed = B115200;  break;
        case 230400:  speed = B230400;  break;
        case 460800:  speed = B460800;  break;
        case 921600:  speed = B921600;  break;
        default:
            LOG_WARN("不支持的波特率 %d，回退到 115200", baudrate);
            speed = B115200;
            break;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    // 8 数据位，无校验，1 停止位（8N1）
    tty.c_cflag &= ~PARENB;        // 无校验
    tty.c_cflag &= ~CSTOPB;        // 1 停止位
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;            // 8 数据位
    tty.c_cflag &= ~CRTSCTS;       // 无硬件流控
    tty.c_cflag |= CREAD | CLOCAL; // 启用接收，忽略调制解调器信号

    // 原始输入（无规范处理、无回显、无信号）
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    // 原始输出
    tty.c_oflag &= ~OPOST;

    // 无软件流控
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);

    // 设置非阻塞
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        LOG_ERROR("fcntl F_GETFL 失败: %s", std::strerror(errno));
        return false;
    }
    if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_ERROR("fcntl F_SETFL O_NONBLOCK 失败: %s", std::strerror(errno));
        return false;
    }

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        LOG_ERROR("tcsetattr 失败: %s", std::strerror(errno));
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// readCommand —— 非阻塞帧同步解析
// ---------------------------------------------------------------------------
// 协议：0xCD  0x01  MODE  TARGET  0xDC
// 搜寻 0xCD 起始字节；找到后读取后续 4 字节。
// 帧有效则返回 {MODE, TARGET}。
// 任何字节错误/缺失 → 重置并继续搜寻 0xCD。
// ---------------------------------------------------------------------------
std::optional<SerialCommand> SerialPort::readCommand() {
    uint8_t byte;

    // 搜寻 0xCD 起始字节
    while (true) {
        ssize_t n = ::read(fd_, &byte, 1);
        if (n <= 0) {
            // 无可用数据（EAGAIN）或错误
            return std::nullopt;
        }
        if (byte == 0xCD) {
            break;   // 找到帧头
        }
    }

    // 读取后续 4 字节（0x01, MODE, TARGET, 0xDC）
    uint8_t buf[4];
    size_t total = 0;
    while (total < 4) {
        ssize_t n = ::read(fd_, buf + total, 4 - total);
        if (n <= 0) {
            // 帧内超时/错误 → 丢弃
            return std::nullopt;
        }
        total += static_cast<size_t>(n);
    }

    // 验证帧结构
    if (buf[0] != 0x01) {
        LOG_DEBUG("串口 RX：期望 0x01，收到 0x%02X", buf[0]);
        return std::nullopt;
    }
    if (buf[3] != 0xDC) {
        LOG_DEBUG("串口 RX：期望 0xDC 结束符，收到 0x%02X", buf[3]);
        return std::nullopt;
    }

    uint8_t mode   = buf[1];
    uint8_t target = buf[2];
    if (mode > 3) {
        LOG_DEBUG("串口 RX：无效模式 %d", mode);
        return std::nullopt;
    }
    if (target > 6) {
        // 0-6 均合法（模式0：0=空闲 1-6=颜色；模式1：0=空闲 1-3=圆环编号）；
        // 具体模式的范围校验由 VisionPipeline::setMode 处理
        LOG_DEBUG("串口 RX：无效目标 %d", target);
        return std::nullopt;
    }

    LOG_DEBUG("串口 RX：MODE%d TARGET%d", mode, target);

    SerialCommand cmd;
    cmd.mode   = mode;
    cmd.target = target;
    return cmd;
}

// ---------------------------------------------------------------------------
// writeResult —— 非阻塞 9 字节帧
// ---------------------------------------------------------------------------
// 协议：0xCD  0x06  TargetType  Color  X_H  X_L  Y_H  Y_L  0xDC
// 坐标：uint16_t，大端序。
// 使用 select() 设置 5 ms 写入超时。
// ---------------------------------------------------------------------------
bool SerialPort::writeResult(uint8_t target_type, uint8_t color,
                             uint16_t x, uint16_t y) {
    uint8_t buf[9] = {
        0xCD,
        0x06,
        target_type,
        color,
        static_cast<uint8_t>((x >> 8) & 0xFF),    // X_H  高字节
        static_cast<uint8_t>(x & 0xFF),            // X_L  低字节
        static_cast<uint8_t>((y >> 8) & 0xFF),    // Y_H  高字节
        static_cast<uint8_t>(y & 0xFF),            // Y_L  低字节
        0xDC,
    };

    LOG_DEBUG("串口发送 HEX: %02X %02X %02X %02X %02X %02X %02X %02X %02X",
              buf[0], buf[1], buf[2], buf[3], buf[4],
              buf[5], buf[6], buf[7], buf[8]);

    if (target_type == 0) {
        LOG_DEBUG("串口发送: 无目标帧 (Color=%d)", color);
    }

    // 非阻塞检查：fd 是否可写？
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(fd_, &write_fds);

    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 5000;   // 5 ms 写入超时

    int ret = select(fd_ + 1, nullptr, &write_fds, nullptr, &tv);
    if (ret <= 0) {
        LOG_WARN("串口发送失败");
        return false;
    }

    // 写入全部 9 字节（循环处理部分写入）
    ssize_t total = 0;
    while (total < 9) {
        ssize_t n = ::write(fd_, buf + total,
                            static_cast<size_t>(9 - total));
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_WARN("串口发送失败");
                return false;
            }
            LOG_ERROR("串口写入错误: %s", std::strerror(errno));
            return false;
        }
        total += n;
    }

    return true;
}
