#include "logger/logger.h"

// ── 静态成员初始化 ────────────────────────────────────────────────

LogLevel Logger::gLevel_    = LogLevel::INFO;
bool     Logger::gUseColor_ = true;

// ── 构造 ──────────────────────────────────────────────────────────

Logger::Logger(LogLevel level, const char* tag)
    : level_(level)
    , shouldLog_(static_cast<int>(level) >= static_cast<int>(gLevel_))
    , tag_(tag)
{
    if (!shouldLog_) return;

    // 格式: [颜色][粗体]标签[重置] [级别颜色]级别[重置]:
    if (gUseColor_) {
        ss_ << "\033[1m" << tag_ << "\033[0m "       // 粗体标签
            << colorCode() << "[" << levelStr() << "]\033[0m ";  // 彩色级别
    } else {
        ss_ << tag_ << " [" << levelStr() << "] ";
    }
}

// ── 析构 ──────────────────────────────────────────────────────────

Logger::~Logger() {
    if (!shouldLog_) return;

    ss_ << '\n';

    // 错误级别输出到 stderr，其余到 stdout
    if (level_ >= LogLevel::ERROR) {
        std::cerr << ss_.str();
        std::cerr.flush();
    } else {
        std::cout << ss_.str();
        std::cout.flush();
    }
}

// ── ANSI 颜色码 ───────────────────────────────────────────────────

const char* Logger::colorCode() const {
    if (!gUseColor_) return "";

    switch (level_) {
        case LogLevel::DEBUG: return "\033[36m";  // 青色
        case LogLevel::INFO:  return "\033[32m";  // 绿色
        case LogLevel::WARN:  return "\033[33m";  // 黄色
        case LogLevel::ERROR: return "\033[31m";  // 红色
        default:              return "\033[0m";
    }
}

// ── 级别文本 ──────────────────────────────────────────────────────

const char* Logger::levelStr() const {
    switch (level_) {
        case LogLevel::DEBUG: return "调试";
        case LogLevel::INFO:  return "信息";
        case LogLevel::WARN:  return "警告";
        case LogLevel::ERROR: return "错误";
        default:              return "未知";
    }
}
