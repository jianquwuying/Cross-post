#pragma once
// ============================================================================
// Logger.h  —  彩色控制台 + 文件双通道日志（单例，线程安全）
// ============================================================================
// 控制台：ANSI 彩色（INFO=绿 WARN=黄 ERROR=红 DEBUG=蓝）
// 文件：  纯文本，无 ANSI 转义码
//
// 用法：
//   Logger::instance().init(true, "../vision.log");   // main() 启动时
//   LOG_INFO("xxx");                                   // 任意位置直接使用
//   Logger::instance().shutdown();                     // main() 退出时
// ============================================================================

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

// ---- ANSI 颜色码 -----------------------------------------------------------
#define COLOR_RESET  "\033[0m"
#define COLOR_GREEN  "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RED    "\033[31m"
#define COLOR_BLUE   "\033[34m"

// ============================================================================
// Logger 单例
// ============================================================================
class Logger {
public:
    static Logger& instance();

    /// 初始化文件日志。to_file=true 时打开 path 指定的文件。
    /// path 支持相对路径（从工作目录解析）。
    void init(bool to_file, const std::string& path);

    /// 关闭文件流。
    void shutdown();

    /// 核心输出：同时写入控制台（带颜色）和文件（纯文本）。
    /// 线程安全。
    void log(const char* color, const char* level,
             const char* fmt, ...);

private:
    Logger()  = default;
    ~Logger();

    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    std::ofstream file_;
    std::mutex    mutex_;
    bool          file_enabled_ = false;
    bool          initialized_  = false;
};

// ============================================================================
// 便捷宏 —— 与旧代码完全兼容
// ============================================================================
#define LOG_INFO(fmt, ...)  \
    Logger::instance().log(COLOR_GREEN,  "INFO",  fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...)  \
    Logger::instance().log(COLOR_YELLOW, "WARN",  fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    Logger::instance().log(COLOR_RED,    "ERROR", fmt, ##__VA_ARGS__)

#ifdef DEBUG
#define LOG_DEBUG(fmt, ...) \
    Logger::instance().log(COLOR_BLUE,   "DEBUG", fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif

// ============================================================================
// 内联实现（header-only 保持构建简单）
// ============================================================================
inline Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

inline Logger::~Logger() {
    shutdown();
}

inline void Logger::init(bool to_file, const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return;
    initialized_ = true;

    if (to_file && !path.empty()) {
        file_.open(path, std::ios::out | std::ios::app);
        if (file_.is_open()) {
            file_enabled_ = true;
            // 写入启动标记
            file_ << "========== Logger started ==========" << std::endl;
            std::printf(COLOR_GREEN "[INFO]  " COLOR_RESET
                        "日志文件已打开: %s\n", path.c_str());
        } else {
            std::printf(COLOR_RED "[ERROR] " COLOR_RESET
                        "无法打开日志文件: %s\n", path.c_str());
        }
    }
}

inline void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_ << "========== Logger stopped ==========" << std::endl;
        file_.close();
    }
    file_enabled_ = false;
}

inline void Logger::log(const char* color, const char* level,
                         const char* fmt, ...) {
    // ---- 格式化消息 ----------------------------------------------------
    char msg_buf[1024];
    va_list args1;
    va_start(args1, fmt);
    std::vsnprintf(msg_buf, sizeof(msg_buf), fmt, args1);
    va_end(args1);

    // ---- 控制台输出（带颜色）-------------------------------------------
    std::lock_guard<std::mutex> lock(mutex_);
    std::printf("%s[%-5s] " COLOR_RESET "%s\n", color, level, msg_buf);
    std::fflush(stdout);

    // ---- 文件输出（纯文本，无颜色码）-----------------------------------
    if (file_enabled_ && file_.is_open()) {
        file_ << "[" << level << "] " << msg_buf << std::endl;
        file_.flush();   // 立即落盘，防止崩溃丢失日志
    }
}
