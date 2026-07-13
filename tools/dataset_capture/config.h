/// @file config.h
/// @brief Dataset Capture 轻量配置模块 — 独立于工程主 config 系统。

#pragma once

#include <string>

struct CaptureConfig {
    // ── 摄像头 ────────────────────────────────────────────────
    int    cameraDevice  = 0;
    int    cameraWidth   = 1280;
    int    cameraHeight  = 720;
    int    cameraFps     = 30;

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

    /// 从 YAML 文件加载配置
    static CaptureConfig load(const std::string& yamlPath);
};
