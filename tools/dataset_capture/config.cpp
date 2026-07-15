/// @file config.cpp
/// @brief YAML 配置加载实现 — 图片采集、视频录制与显示参数。

#include "config.h"

#include <yaml-cpp/yaml.h>
#include <iostream>

// ── 从 YAML 文件加载 ──────────────────────────────────────────────

CaptureConfig CaptureConfig::load(const std::string& yamlPath) {
    try {
        YAML::Node root = YAML::LoadFile(yamlPath);
        return load(root);
    } catch (const YAML::Exception& e) {
        std::cerr << "\033[31m[ERROR]\033[0m YAML 解析失败: "
                  << e.what() << ", 使用默认配置。\n";
        return CaptureConfig{};
    }
}

// ── 从 YAML::Node 加载 (共享解析，避免重复读文件) ─────────────────

CaptureConfig CaptureConfig::load(const YAML::Node& root) {
    CaptureConfig cfg;

    if (root["capture"].IsDefined()) {
        auto cp = root["capture"];

        // ── 图片采集 ────────────────────────────────────────
        if (cp["image"].IsDefined()) {
            auto im = cp["image"];
            if (im["enable"].IsDefined())
                cfg.captureEnable = im["enable"].as<bool>();
            if (im["interval_ms"].IsDefined())
                cfg.captureIntervalMs = im["interval_ms"].as<int>();
            if (im["save_dir"].IsDefined())
                cfg.saveDir = im["save_dir"].as<std::string>();
            if (im["prefix"].IsDefined())
                cfg.prefix = im["prefix"].as<std::string>();
            if (im["format"].IsDefined())
                cfg.format = im["format"].as<std::string>();
            if (im["jpeg_quality"].IsDefined())
                cfg.jpegQuality = im["jpeg_quality"].as<int>();
        }

        // ── 视频录制 ────────────────────────────────────────
        if (cp["video"].IsDefined()) {
            auto vd = cp["video"];
            if (vd["enable"].IsDefined())
                cfg.videoEnable = vd["enable"].as<bool>();
            if (vd["save_path"].IsDefined())
                cfg.videoSavePath = vd["save_path"].as<std::string>();
            if (vd["filename_prefix"].IsDefined())
                cfg.videoFilenamePrefix = vd["filename_prefix"].as<std::string>();
            if (vd["codec"].IsDefined())
                cfg.videoCodec = vd["codec"].as<std::string>();
            if (vd["extension"].IsDefined())
                cfg.videoExtension = vd["extension"].as<std::string>();
        }
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

    return cfg;
}
