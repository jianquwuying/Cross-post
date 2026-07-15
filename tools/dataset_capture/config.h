/// @file config.h
/// @brief Dataset Capture 轻量配置模块 — 采集与显示参数。
///
/// 摄像头参数统一使用 CameraConfig (camera/camera.h)，不再在此定义。

#pragma once

#include <string>

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

    /// 从 YAML 文件加载采集/显示配置
    /// @note 摄像头参数需单独通过 CameraConfig::fromYaml() 加载
    static CaptureConfig load(const std::string& yamlPath);
};
