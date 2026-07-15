/// @file main.cpp
/// @brief Dataset Capture — 原始数据集采集工具入口。
///
/// 用法:
///   ./dataset_capture                           # 自动查找 vision.yaml
///   ./dataset_capture ../vision.yaml            # 位置参数指定路径
///   ./dataset_capture --config path/to.yaml     # 显式指定路径
///
/// 配置搜索: 基于 /proc/self/exe 找到二进制所在目录, 相对查找 YAML。
/// 使用 Camera 类统一管理摄像头, 参数完全由 YAML 控制。

#include "dataset_capture.h"

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <csignal>

namespace fs = std::filesystem;

// ── 配置路径自动发现 ──────────────────────────────────────────────

/// 通过 /proc/self/exe 获取二进制所在目录 (Linux)
static std::string getExeDir() {
    std::error_code ec;
    auto exePath = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return "";
    return fs::path(exePath).parent_path().string();
}

/// 按顺序搜索候选路径, 返回第一个存在的文件, 都不存在返回空串
static std::string resolveConfigPath(const std::vector<std::string>& candidates) {
    for (const auto& p : candidates) {
        std::error_code ec;
        if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
            return p;
        }
    }
    return "";
}

int main(int argc, char* argv[]) {
    // ── 信号处理 ──────────────────────────────────────────────
    std::signal(SIGINT,  [](int){});
    std::signal(SIGTERM, [](int){});

    // ── 解析命令行参数 ──────────────────────────────────────────
    std::string configPath;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // --config PATH
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
            continue;
        }

        // --help / -h
        if (arg == "--help" || arg == "-h") {
            std::cout << "Dataset Capture — 原始数据集采集工具\n\n"
                      << "用法: " << argv[0] << " [CONFIG_PATH] [--config PATH]\n"
                      << "  CONFIG_PATH   配置文件路径 (位置参数, 可选)\n"
                      << "  --config PATH 配置文件路径 (显式指定)\n\n"
                      << "自动搜索 (基于 /proc/self/exe):\n"
                      << "  1. 命令行指定的路径\n"
                      << "  2. <exe>/../vision.yaml\n"
                      << "  3. <exe>/../../config/vision.yaml\n\n"
                      << "快捷键:\n"
                      << "  ESC    退出程序\n"
                      << "  SPACE  立即保存一张图片\n"
                      << "  S      暂停/恢复自动采集\n"
                      << "  R      重新扫描目录编号\n";
            return 0;
        }

        // 位置参数 (第一个非 flag 参数视为配置路径)
        if (arg[0] != '-' && configPath.empty()) {
            configPath = arg;
            continue;
        }
    }

    // ── 自动搜索配置 (基于二进制位置) ──────────────────────────
    if (configPath.empty()) {
        std::vector<std::string> candidates;

        std::string exeDir = getExeDir();
        if (!exeDir.empty()) {
            // 二进制在 tools/dataset_capture/build/
            candidates.push_back(exeDir + "/../vision.yaml");              // tools/dataset_capture/
            candidates.push_back(exeDir + "/../../config/vision.yaml");    // config/
        }
        // CWD 回退
        candidates.push_back("../vision.yaml");
        candidates.push_back("vision.yaml");

        configPath = resolveConfigPath(candidates);

        if (configPath.empty()) {
            std::cerr << "\033[31m[ERROR]\033[0m "
                      << "找不到配置文件。搜索路径:\n";
            for (const auto& p : candidates) {
                std::cerr << "  " << p << "\n";
            }
            std::cerr << "\n请通过位置参数或 --config 指定配置文件路径。\n";
            return 1;
        }

        std::cout << "\033[32m[INFO]\033[0m  "
                  << "自动发现配置: " << configPath
                  << "  (二进制: " << exeDir << ")\n";
    }

    // ── 加载 YAML (仅此一次) ──────────────────────────────────
    YAML::Node root;
    try {
        root = YAML::LoadFile(configPath);
    } catch (const YAML::Exception& e) {
        std::cerr << "\033[31m[ERROR]\033[0m "
                  << "YAML 解析失败 (" << configPath << "): "
                  << e.what() << "\n";
        return 1;
    }

    // ── 初始化 + 运行 ──────────────────────────────────────────
    DatasetCapture cap;
    if (!cap.Init(root)) {
        std::cerr << "\033[31m[ERROR]\033[0m 初始化失败, 退出。\n";
        return 1;
    }

    cap.Run();

    return 0;
}
