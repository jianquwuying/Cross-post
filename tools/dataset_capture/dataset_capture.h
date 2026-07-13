/// @file dataset_capture.h
/// @brief 原始数据集采集工具 — 仅采集图片, 不涉及推理。
///
/// 功能:
///   - 打开 USB 摄像头, 实时显示画面
///   - 按 interval_ms 自动保存原始 JPEG 图片
///   - 快捷键: ESC退出, SPACE立即保存, S暂停/恢复, R重新扫描编号
///   - 自动创建保存目录, 增量编号不覆盖已有图片
///
/// 使用:
///   DatasetCapture cap;
///   cap.Init("vision.yaml");
///   cap.Run();

#pragma once

#include "config.h"

#include <opencv2/videoio.hpp>
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

    /// 初始化: 加载配置 + 打开摄像头 + 创建目录 + 扫描编号
    bool Init(const std::string& yamlPath);

    /// 主循环: 采集 + 显示 + 快捷键处理
    void Run();

private:
    /// 打开 USB 摄像头
    bool OpenCamera();

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
    cv::VideoCapture cap_;
    int nextIndex_ = 1;
    bool paused_   = false;
    bool running_  = true;
    int savedCount_ = 0;
    double instantFps_ = 0.0;
    std::chrono::steady_clock::time_point lastSaveTime_;
    std::chrono::steady_clock::time_point lastFrameTime_;
};
