#pragma once
// ============================================================================
// Logger.h  —  基于 printf 的彩色控制台日志
// ============================================================================
// 线程安全：每个宏展开为单次 printf 调用。
// ANSI 颜色：INFO=绿色  WARN=黄色  ERROR=红色  DEBUG=蓝色
// ============================================================================

#include <cstdio>

#define COLOR_RESET  "\033[0m"
#define COLOR_GREEN  "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RED    "\033[31m"
#define COLOR_BLUE   "\033[34m"

#define LOG_INFO(fmt, ...)  \
    std::printf(COLOR_GREEN "[INFO]  " COLOR_RESET fmt "\n", ##__VA_ARGS__)

#define LOG_WARN(fmt, ...)  \
    std::printf(COLOR_YELLOW "[WARN]  " COLOR_RESET fmt "\n", ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    std::printf(COLOR_RED   "[ERROR] " COLOR_RESET fmt "\n", ##__VA_ARGS__)

#ifdef DEBUG
#define LOG_DEBUG(fmt, ...) \
    std::printf(COLOR_BLUE  "[DEBUG] " COLOR_RESET fmt "\n", ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif
