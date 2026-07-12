/// @file frame_source_factory.cpp
/// @brief FrameSourceFactory 实现。

#include "src/input/frame_source_factory.hpp"

#include <yaml-cpp/yaml.h>
#include <iostream>

// ── 参数化创建 ────────────────────────────────────────────────────

FrameSourcePtr FrameSourceFactory::create(
    const std::string& inputType,
    const std::string& videoPath,
    bool videoLoop,
    int cameraId, int camWidth, int camHeight, int camFps)
{
    if (inputType == "camera") {
        Camera::Config cfg;
        cfg.deviceId = cameraId;
        cfg.width    = camWidth;
        cfg.height   = camHeight;
        cfg.fps      = camFps;
        cfg.bufferSize = 1;

        auto src = std::make_unique<CameraSource>(cfg);
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

        // 读取摄像头参数
        int camId = 0, camW = 640, camH = 480, camFps = 30;
        if (root["camera"]) {
            if (root["camera"]["id"])     camId  = root["camera"]["id"].as<int>();
            if (root["camera"]["width"])  camW   = root["camera"]["width"].as<int>();
            if (root["camera"]["height"]) camH   = root["camera"]["height"].as<int>();
            if (root["camera"]["fps"])    camFps = root["camera"]["fps"].as<int>();
        }

        return create(inputType, videoPath, videoLoop,
                      camId, camW, camH, camFps);

    } catch (const YAML::Exception& e) {
        std::cerr << "[Factory] YAML 解析失败: " << e.what() << std::endl;
        return nullptr;
    }
}
