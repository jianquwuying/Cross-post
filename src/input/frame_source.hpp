/// @file frame_source.hpp
/// @brief 输入源抽象接口 — 统一摄像头和视频文件的帧读取。
///
/// 设计目标:
///   - YOLO 推理模块不感知输入来源 (摄像头 / 视频文件 / 未来 RTSP)
///   - 通过 YAML 配置切换输入源, 无需重编译
///   - 扩展新输入源只需实现 FrameSource 接口
///
/// 继承层次:
///   FrameSource (抽象)
///   ├── CameraSource  (实时摄像头)
///   ├── VideoSource   (离线视频文件)
///   └── (future) RtspSource (网络流)

#pragma once

#include <opencv2/core.hpp>
#include <memory>
#include <string>

class FrameSource {
public:
    virtual ~FrameSource() = default;

    /// 打开输入源 (摄像头/视频文件/网络流)
    /// @return 成功返回 true
    virtual bool open() = 0;

    /// 读取一帧 (阻塞)
    /// @param frame  输出的 BGR 帧
    /// @return false 表示输入源结束或不可用
    virtual bool read(cv::Mat& frame) = 0;

    /// 关闭输入源, 释放资源
    virtual void close() = 0;

    /// 是否已成功打开并可用
    virtual bool isOpened() const = 0;

    /// 获取帧率
    /// camera: 实际采集帧率 (EMA 平滑)
    /// video:   文件标称帧率
    /// @return FPS, 未知时返回 0
    virtual double getFps() const = 0;

    /// 获取总帧数
    /// camera: 返回 0 表示无限流
    /// video:   返回文件总帧数
    virtual int getTotalFrames() const = 0;

    /// 获取当前帧号 (从 1 开始)
    virtual int getCurrentFrame() const = 0;

    /// 输入源宽度 (像素)
    virtual int getWidth() const = 0;

    /// 输入源高度 (像素)
    virtual int getHeight() const = 0;

    /// 输入源类型名称 (用于日志)
    virtual std::string typeName() const = 0;
};

// ── 类型别名 ──────────────────────────────────────────────────────

using FrameSourcePtr = std::unique_ptr<FrameSource>;
