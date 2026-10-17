#pragma once
// ============================================================================
// Logger.h  —  彩色控制台 + 文件双通道日志（单例，线程安全）
// ============================================================================
// 控制台：ANSI 彩色（INFO=绿 WARN=黄 ERROR=红 DEBUG=蓝），每条后立即 fflush。
// 文件：  纯文本，无 ANSI 转义码；每条后立即 flush，崩溃时尽量保留日志。
// 级别：  LOG_DEBUG 仅在编译期定义 DEBUG 时生效（CMake Debug 构建带 -DDEBUG），
//         其余构建中展开为空操作，不输出日志。
//
// 用法：
//   Logger::instance().init(cfg.logToFile(), cfg.logFilePath());  // main() 启动时
//   LOG_INFO("xxx");                                              // 任意位置直接使用
//   Logger::instance().shutdown();                                // main() 退出时
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

    /// 初始化文件日志。to_file=true 且 path 非空时，以追加模式打开 path。
    /// 仅第一次调用生效（已初始化则直接返回）；相对路径按工作目录解析。
    /// 打开成功会写入启动标记；打开失败只在控制台打印错误，程序继续运行。
    void init(bool to_file, const std::string& path);

    /// 写入结束标记并关闭文件流（未打开时无操作）。
    void shutdown();

    /// 核心输出：用 1024 字节缓冲区格式化后，同时写控制台（带颜色）和文件
    /// （纯文本）。控制台与文件输出在同一把 mutex 内完成，多线程不会交错。
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
// 便捷宏 —— 统一级别标签与颜色
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
// Release 构建：LOG_DEBUG 展开为空操作，不输出日志
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
    // ---- 格式化消息（超长消息由 vsnprintf 截断）------------------------
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
