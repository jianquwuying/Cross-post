/// @file logger.h
/// @brief 基于 ANSI 转义序列的终端彩色日志系统。
///
/// 零外部依赖，仅使用 Linux 终端标准 ANSI 颜色。
/// 编译期可通过 LOG_LEVEL 宏控制日志级别以优化性能。
///
/// 使用方式:
///   LOG_INFO  << "[YOLO] 模型加载成功。";
///   LOG_WARN  << "[颜色] 颜色置信度过低: " << conf;
///   LOG_ERROR << "[串口] 无法打开设备: " << path;
///   LOG_DEBUG << "[ROI] 裁剪区域: " << x << "x" << y;
///
/// ANSI 颜色方案:
///   DEBUG → 青色 (36)
///   INFO  → 绿色 (32)
///   WARN  → 黄色 (33)
///   ERROR → 红色 (31)
///   标签  → 粗体 (1)

#pragma once

#include <iostream>
#include <sstream>
#include <string>

// ── 日志级别 ──────────────────────────────────────────────────────

enum class LogLevel {
    DEBUG = 0,   ///< 调试信息
    INFO  = 1,   ///< 常规状态
    WARN  = 2,   ///< 警告（可恢复异常）
    ERROR = 3,   ///< 错误（可能影响运行）
    NONE  = 4,   ///< 关闭所有日志
};

// ── Logger 类 ─────────────────────────────────────────────────────

class Logger {
public:
    /// 设置全局日志级别（低于此级别的日志将被静默丢弃）
    static void setLevel(LogLevel level) { gLevel_ = level; }

    /// 设置是否启用 ANSI 颜色（管道重定向时建议关闭）
    static void setUseColor(bool use) { gUseColor_ = use; }

    /// 获取当前日志级别
    static LogLevel getLevel() { return gLevel_; }

    /// 构造日志记录器
    /// @param level  本条日志级别
    /// @param tag    标签（通常为模块名，如 "[YOLO]"）
    Logger(LogLevel level, const char* tag);

    /// 析构时自动刷新到 std::cout / std::cerr
    ~Logger();

    /// 流式操作符 — 拼接日志内容
    template<typename T>
    Logger& operator<<(const T& val) {
        if (shouldLog_) ss_ << val;
        return *this;
    }

private:
    static LogLevel gLevel_;      ///< 全局日志级别
    static bool     gUseColor_;   ///< 是否使用颜色

    LogLevel level_;              ///< 本条日志级别
    bool     shouldLog_;          ///< 是否实际输出
    std::ostringstream ss_;       ///< 日志内容缓冲区
    const char* tag_;             ///< 模块标签

    /// 获取级别对应的 ANSI 颜色码
    const char* colorCode() const;

    /// 获取级别对应的中文标签
    const char* levelStr() const;
};

// ── 便捷宏 ────────────────────────────────────────────────────────

#define LOG_DEBUG   Logger(LogLevel::DEBUG, __FILE__)
#define LOG_INFO    Logger(LogLevel::INFO,  __FILE__)
#define LOG_WARN    Logger(LogLevel::WARN,  __FILE__)
#define LOG_ERROR   Logger(LogLevel::ERROR, __FILE__)
