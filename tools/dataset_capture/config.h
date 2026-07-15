/// @file config.h
/// @brief Dataset Capture 轻量配置模块 — 采集与显示参数。
///
/// 摄像头参数统一使用 CameraConfig (camera/camera.h)，不再在此定义。

#pragma once

#include <string>

// 前向声明, 避免在此引入完整 yaml-cpp 头文件
namespace YAML { class Node; }

struct CaptureConfig {
    // ── 采集 ──────────────────────────────────────────────────
    bool   captureEnable    = true;
    int    captureIntervalMs = 100;       ///< 自动保存间隔 (毫秒)
    std::string saveDir     = "./dataset";
    std::string prefix      = "img";
    std::string format      = "jpg";
    int    jpegQuality      = 95;

    // ── 显示 ──────────────────────────────────────────────────
    bool   displayEnable      = true;
    bool   showWindow         = true;
    bool   showFps            = true;
    bool   showSavedCount     = true;

    /// 从 YAML 文件路径加载采集/显示配置
    static CaptureConfig load(const std::string& yamlPath);

    /// 从已解析的 YAML::Node 加载 (避免重复解析文件)
    static CaptureConfig load(const YAML::Node& root);
};
