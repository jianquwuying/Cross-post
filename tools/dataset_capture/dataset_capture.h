/// @file dataset_capture.h
/// @brief 原始数据集采集工具 — 仅采集图片, 不涉及推理。
///
/// 功能:
///   - 使用 Camera 类打开 USB 摄像头, 实时显示画面
///   - 按 interval_ms 自动保存原始 JPEG 图片
///   - 快捷键: ESC退出, SPACE立即保存, S暂停/恢复, R重新扫描编号
///   - 自动创建保存目录, 增量编号不覆盖已有图片
///   - 摄像头参数完全由 YAML 控制 (CameraConfig)
///
/// 使用:
///   DatasetCapture cap;
///   cap.Init("vision.yaml");
///   cap.Run();

#pragma once

#include "config.h"
#include "camera/camera.h"

#include <yaml-cpp/yaml.h>

#include <opencv2/core.hpp>

#include <chrono>
#include <string>

class DatasetCapture {
public:
    DatasetCapture() = default;
    ~DatasetCapture();

    // ── 不可拷贝 ──────────────────────────────────────────────
    DatasetCapture(const DatasetCapture&) = delete;
    DatasetCapture& operator=(const DatasetCapture&) = delete;

    /// 初始化: 解析 YAML + 打开摄像头 + 创建目录 + 扫描编号
    /// @param root  已解析的 YAML::Node (由调用方加载, 避免重复解析文件)
    bool Init(const YAML::Node& root);

    /// 主循环: 采集 + 显示 + 快捷键处理
    void Run();

private:
    /// 保存一帧为 JPEG
    void SaveFrame(const cv::Mat& frame);

    /// 在帧上绘制状态信息 (FPS, 已保存数量, 状态等)
    void DrawOverlay(cv::Mat& frame);

    /// 扫描 save_dir 中已有图片, 确定下一个编号
    void ScanImageIndex();

    /// 生成文件名: img_000123.jpg
    std::string GenerateFileName() const;

    // ── 成员变量 ──────────────────────────────────────────────

    CaptureConfig cfg_;
    Camera camera_;
    int nextIndex_ = 1;
    bool paused_   = false;
    bool running_  = true;
    int savedCount_ = 0;
    std::chrono::steady_clock::time_point lastSaveTime_;
};
