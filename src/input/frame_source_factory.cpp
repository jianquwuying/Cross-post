/// @file frame_source_factory.cpp
/// @brief FrameSourceFactory 实现 — 使用 CameraConfig 统一配置。

#include "src/input/frame_source_factory.hpp"

#include <yaml-cpp/yaml.h>
#include <iostream>

// ── 参数化创建 ────────────────────────────────────────────────────

FrameSourcePtr FrameSourceFactory::create(
    const std::string& inputType,
    const std::string& videoPath,
    bool videoLoop,
    const CameraConfig& camCfg)
{
    if (inputType == "camera") {
        auto src = std::make_unique<CameraSource>(camCfg);
        return src;
    }

    if (inputType == "video") {
        auto src = std::make_unique<VideoSource>(videoPath, videoLoop);
        return src;
    }

    std::cerr << "[Factory] 错误: 不支持的输入类型 '" << inputType
              << "', 支持: camera | video" << std::endl;
    return nullptr;
}

// ── 从 YAML 创建 ──────────────────────────────────────────────────

FrameSourcePtr FrameSourceFactory::createFromYaml(const std::string& yamlPath)
{
    try {
        YAML::Node root = YAML::LoadFile(yamlPath);

        // 读取输入类型
        std::string inputType = "camera";
        if (root["input"] && root["input"]["type"]) {
            inputType = root["input"]["type"].as<std::string>();
        }

        // 读取视频参数
        std::string videoPath = "./data/test.mp4";
        bool videoLoop = false;
        if (root["video"]) {
            if (root["video"]["path"])
                videoPath = root["video"]["path"].as<std::string>();
            if (root["video"]["loop"])
                videoLoop = root["video"]["loop"].as<bool>();
        }

        // 摄像头配置 — 严格解析, 无默认值
        if (!root["camera"].IsDefined()) {
            std::cerr << "\033[31m[Factory]\033[0m "
                      << "Missing required 'camera' section in config"
                      << std::endl;
            return nullptr;
        }
        CameraConfig camCfg = CameraConfig::fromYaml(root["camera"]);

        return create(inputType, videoPath, videoLoop, camCfg);

    } catch (const YAML::Exception& e) {
        std::cerr << "\033[31m[Factory]\033[0m YAML 解析失败: "
                  << e.what() << std::endl;
        return nullptr;
    }
}
