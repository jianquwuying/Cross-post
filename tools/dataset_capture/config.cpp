/// @file config.cpp
/// @brief YAML 配置加载实现 — 仅采集和显示参数。

#include "config.h"

#include <yaml-cpp/yaml.h>
#include <iostream>

CaptureConfig CaptureConfig::load(const std::string& yamlPath) {
    CaptureConfig cfg;

    try {
        YAML::Node root = YAML::LoadFile(yamlPath);

        // ── 采集 ────────────────────────────────────────────
        if (root["capture"].IsDefined()) {
            auto cp = root["capture"];
            if (cp["enable"].IsDefined())
                cfg.captureEnable = cp["enable"].as<bool>();
            if (cp["interval_ms"].IsDefined())
                cfg.captureIntervalMs = cp["interval_ms"].as<int>();
            if (cp["save_dir"].IsDefined())
                cfg.saveDir      = cp["save_dir"].as<std::string>();
            if (cp["prefix"].IsDefined())
                cfg.prefix       = cp["prefix"].as<std::string>();
            if (cp["format"].IsDefined())
                cfg.format       = cp["format"].as<std::string>();
            if (cp["jpeg_quality"].IsDefined())
                cfg.jpegQuality  = cp["jpeg_quality"].as<int>();
        }

        // ── 显示 ────────────────────────────────────────────
        if (root["display"].IsDefined()) {
            auto d = root["display"];
            if (d["enable"].IsDefined())
                cfg.displayEnable = d["enable"].as<bool>();
            if (d["show_window"].IsDefined())
                cfg.showWindow = d["show_window"].as<bool>();
            if (d["show_fps"].IsDefined())
                cfg.showFps = d["show_fps"].as<bool>();
            if (d["show_saved_count"].IsDefined())
                cfg.showSavedCount = d["show_saved_count"].as<bool>();
        }

        std::cout << "\033[32m[INFO]\033[0m 配置已加载: " << yamlPath << "\n";

    } catch (const YAML::Exception& e) {
        std::cerr << "\033[31m[ERROR]\033[0m YAML 解析失败: "
                  << e.what() << ", 使用默认配置。\n";
    }

    return cfg;
}
