/// @file frame_source_factory.hpp
/// @brief 输入源工厂 — 根据 YAML 配置创建 FrameSource 实例。

#pragma once

#include "src/input/frame_source.hpp"
#include "src/input/camera_source.hpp"
#include "src/input/video_source.hpp"

#include <string>
#include <memory>

/// @brief 根据 YAML 配置创建输入源。
///
/// 配置格式 (vision.yaml):
///   input:
///     type: "camera"  |  "video"
///
///   camera:
///     id: 0
///     width: 640
///     height: 480
///     fps: 30
///
///   video:
///     path: "./data/test.mp4"
///     loop: false
///
/// 使用示例:
///   auto source = FrameSourceFactory::create(cfg);
///   if (!source || !source->open()) { /* error */ }
///   while (source->read(frame)) { /* process */ }

class FrameSourceFactory {
public:
    /// 从 YAML 节点创建输入源
    /// @param inputNode  YAML 的 "input" 节点
    /// @param cameraNode YAML 的 "camera" 节点
    /// @param videoNode  YAML 的 "video" 节点
    /// @return FrameSource 实例, 失败返回 nullptr
    static FrameSourcePtr create(
        const std::string& inputType,
        const std::string& videoPath,
        bool videoLoop,
        int cameraId, int camWidth, int camHeight, int camFps);

    /// 便捷接口: 从 YAML 文件路径创建
    /// @param yamlPath  vision.yaml 文件路径
    /// @return FrameSource 实例, 失败返回 nullptr
    static FrameSourcePtr createFromYaml(const std::string& yamlPath);
};
