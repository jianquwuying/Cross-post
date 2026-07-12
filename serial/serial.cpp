#include "serial/serial.h"

#include <iostream>
#include <cstring>
#include <cerrno>

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>

SerialPort::~SerialPort() {
    close();
}

SerialPort::SerialPort(SerialPort&& other) noexcept
    : fd_(other.fd_)
    , config_(other.config_)
{
    other.fd_ = -1;
}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        config_ = other.config_;
        other.fd_ = -1;
    }
    return *this;
}

bool SerialPort::open(const Config& config) {
    config_ = config;

    // ── 打开设备 ──────────────────────────────────────────────
    // O_RDWR   = 读写模式
    // O_NOCTTY = 不将此设备设为控制终端
    // O_NDELAY = 非阻塞打开（后续配置为阻塞模式）
    fd_ = ::open(config_.device.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd_ < 0) {
        std::cerr << "[串口] 错误: 无法打开 " << config_.device
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }

    // 确保不被设为控制终端
    if (::fcntl(fd_, F_SETFL, 0) < 0) {
        std::cerr << "[串口] 错误: fcntl 失败: "
                  << std::strerror(errno) << std::endl;
        close();
        return false;
    }

    // ── 配置 termios ──────────────────────────────────────────
    if (!configureTermios()) {
        close();
        return false;
    }

    std::cout << "[串口] 已打开 " << config_.device
              << " @ " << config_.baudrate << " 波特 "
              << config_.dataBits << config_.parity << config_.stopBits
              << std::endl;

    // 等待硬件稳定
    ::usleep(100000); // 100ms

    return true;
}

bool SerialPort::configureTermios() {
    struct termios tty;
    std::memset(&tty, 0, sizeof(tty));

    if (::tcgetattr(fd_, &tty) != 0) {
        std::cerr << "[串口] 错误: tcgetattr: "
                  << std::strerror(errno) << std::endl;
        return false;
    }

    // ── 波特率 ────────────────────────────────────────────────
    speed_t speed = B115200;
    switch (config_.baudrate) {
        case 9600:    speed = B9600;    break;
        case 19200:   speed = B19200;   break;
        case 38400:   speed = B38400;   break;
        case 57600:   speed = B57600;   break;
        case 115200:  speed = B115200;  break;
        case 230400:  speed = B230400;  break;
        case 460800:  speed = B460800;  break;
        case 921600:  speed = B921600;  break;
        case 1000000: speed = B1000000; break;
        case 1500000: speed = B1500000; break;
        case 2000000: speed = B2000000; break;
        default:
            std::cerr << "[串口] 警告: 不支持的波特率 "
                      << config_.baudrate << ", 回退到 115200"
                      << std::endl;
            speed = B115200;
            break;
    }

    ::cfsetispeed(&tty, speed);
    ::cfsetospeed(&tty, speed);

    // ── 控制标志 ──────────────────────────────────────────────
    // CSIZE: 字符大小掩码
    // CLOCAL: 忽略调制解调器控制线
    // CREAD: 启用接收器
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CSIZE;

    // 数据位
    switch (config_.dataBits) {
        case 5: tty.c_cflag |= CS5; break;
        case 6: tty.c_cflag |= CS6; break;
        case 7: tty.c_cflag |= CS7; break;
        case 8: tty.c_cflag |= CS8; break;
        default: tty.c_cflag |= CS8; break;
    }

    // 校验位
    switch (config_.parity) {
        case 'E': case 'e':
            tty.c_cflag |= PARENB;
            tty.c_cflag &= ~PARODD;
            break;
        case 'O': case 'o':
            tty.c_cflag |= (PARENB | PARODD);
            break;
        case 'N': case 'n':
        default:
            tty.c_cflag &= ~PARENB;
            break;
    }

    // 停止位
    if (config_.stopBits == 2) {
        tty.c_cflag |= CSTOPB;
    } else {
        tty.c_cflag &= ~CSTOPB;
    }

    // 硬件流控
    if (config_.hardwareFlowControl) {
        tty.c_cflag |= CRTSCTS;
    } else {
        tty.c_cflag &= ~CRTSCTS;
    }

    // ── 本地标志 ──────────────────────────────────────────────
    tty.c_lflag = 0;  // 原始模式: 无规范模式, 无回显, 无信号

    // ── 输入标志 ──────────────────────────────────────────────
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);  // 无软件流控
    tty.c_iflag &= ~(INLCR | ICRNL | IGNCR); // 无换行转换

    // ── 输出标志 ──────────────────────────────────────────────
    tty.c_oflag = 0;  // 原始输出

    // ── 超时设置 ──────────────────────────────────────────────
    tty.c_cc[VMIN]  = 0;                             // 非阻塞读取
    tty.c_cc[VTIME] = static_cast<cc_t>(config_.rxTimeout); // 超时 (0.1s 单位)

    // ── 应用配置 ──────────────────────────────────────────────
    if (::tcsetattr(fd_, TCSANOW, &tty) != 0) {
        std::cerr << "[串口] 错误: tcsetattr: "
                  << std::strerror(errno) << std::endl;
        return false;
    }

    return true;
}

ssize_t SerialPort::write(const uint8_t* data, size_t len) {
    if (fd_ < 0) return -1;

    ssize_t written = ::write(fd_, data, len);
    if (written < 0) {
        std::cerr << "[串口] 写入错误: "
                  << std::strerror(errno) << std::endl;
    }

    // 等待输出排空
    ::tcdrain(fd_);

    return written;
}

ssize_t SerialPort::write(const std::vector<uint8_t>& data) {
    return write(data.data(), data.size());
}

ssize_t SerialPort::read(uint8_t* buf, size_t maxLen) {
    if (fd_ < 0) return -1;

    // 使用 select() 实现超时
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd_, &readSet);

    struct timeval timeout;
    timeout.tv_sec  = config_.rxTimeout / 10;
    timeout.tv_usec = (config_.rxTimeout % 10) * 100000;

    int ret = ::select(fd_ + 1, &readSet, nullptr, nullptr, &timeout);
    if (ret < 0) {
        std::cerr << "[串口] select 错误: "
                  << std::strerror(errno) << std::endl;
        return -1;
    }
    if (ret == 0) {
        return 0;  // 超时
    }

    return ::read(fd_, buf, maxLen);
}

ssize_t SerialPort::read(std::vector<uint8_t>& buf, size_t maxLen) {
    buf.resize(maxLen);
    ssize_t n = read(buf.data(), maxLen);
    if (n > 0) {
        buf.resize(static_cast<size_t>(n));
    } else {
        buf.clear();
    }
    return n;
}

void SerialPort::flush() {
    if (fd_ >= 0) {
        ::tcflush(fd_, TCIOFLUSH);
    }
}

void SerialPort::close() {
    if (fd_ >= 0) {
        ::tcdrain(fd_);
        ::close(fd_);
        std::cout << "[串口] 已关闭 " << config_.device << std::endl;
        fd_ = -1;
    }
}
