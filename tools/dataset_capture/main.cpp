/// @file main.cpp
/// @brief Dataset Capture — 原始数据集采集工具入口。
///
/// 用法:
///   ./dataset_capture [--config vision.yaml]
///
/// 使用 Camera 类统一管理摄像头, 参数完全由 YAML 控制。

#include "dataset_capture.h"

#include <iostream>
#include <csignal>

static bool g_running = true;

static void signalHandler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ── 解析 --config ──────────────────────────────────────────
    std::string configPath = "vision.yaml";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "Dataset Capture — 原始数据集采集工具\n\n"
                      << "用法: " << argv[0] << " [--config PATH]\n"
                      << "  --config PATH  配置文件路径 (默认: vision.yaml)\n\n"
                      << "快捷键:\n"
                      << "  ESC    退出程序\n"
                      << "  SPACE  立即保存一张图片\n"
                      << "  S      暂停/恢复自动采集\n"
                      << "  R      重新扫描目录编号\n";
            return 0;
        }
    }

    // 关闭 g_running 标志传递 (DatasetCapture 内部管理运行状态)
    (void)g_running;

    DatasetCapture cap;
    if (!cap.Init(configPath)) {
        std::cerr << "\033[31m[ERROR]\033[0m 初始化失败, 退出。\n";
        return 1;
    }

    cap.Run();

    return 0;
}
