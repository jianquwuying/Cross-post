/// @file frame_source_factory.hpp
/// @brief 输入源工厂 — 根据 YAML 配置创建 FrameSource 实例。

#pragma once

#include "src/input/frame_source.hpp"
#include "src/input/camera_source.hpp"
#include "src/input/video_source.hpp"
#include "camera/camera.h"

#include <string>
#include <memory>

/// @brief 根据 YAML 配置创建输入源。
///
/// 配置格式 (vision.yaml):
///   input:
///     type: "camera"  |  "video"
///
///   camera:
///     device: 0
///     backend: "V4L2"
///     width: 640
///     height: 480
///     fps: 30
///     fourcc: "MJPG"
///     ...
///
///   video:
///     path: "./data/test.mp4"
///     loop: false
///
/// 使用示例:
///   auto source = FrameSourceFactory::createFromYaml("config/vision.yaml");
///   if (!source || !source->open()) { /* error */ }
///   while (source->read(frame)) { /* process */ }

class FrameSourceFactory {
public:
    /// 参数化创建输入源
    /// @param inputType   "camera" | "video"
    /// @param videoPath   视频文件路径
    /// @param videoLoop   是否循环播放
    /// @param camCfg      摄像头配置 (来自 CameraConfig::fromYaml)
    /// @return FrameSource 实例, 失败返回 nullptr
    static FrameSourcePtr create(
        const std::string& inputType,
        const std::string& videoPath,
        bool videoLoop,
        const CameraConfig& camCfg);

    /// 便捷接口: 从 YAML 文件路径创建
    /// @param yamlPath  vision.yaml 文件路径
    /// @return FrameSource 实例, 失败返回 nullptr
    static FrameSourcePtr createFromYaml(const std::string& yamlPath);
};
