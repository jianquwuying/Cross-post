/// @file camera_source.hpp
/// @brief 摄像头输入源 — 封装现有 Camera 类为 FrameSource 接口。

#pragma once

#include "src/input/frame_source.hpp"
#include "camera/camera.h"

class CameraSource : public FrameSource {
public:
    explicit CameraSource(const CameraConfig& cfg) : cfg_(cfg) {}

    bool open() override {
        return camera_.open(cfg_);
    }

    bool read(cv::Mat& frame) override {
        if (!camera_.isOpened()) return false;
        bool ok = camera_.read(frame);
        if (ok && !frame.empty()) {
            ++frameCount_;
            return true;
        }
        return false;
    }

    void close() override {
        camera_.release();
        frameCount_ = 0;
    }

    bool isOpened() const override {
        return camera_.isOpened();
    }

    double getFps() const override {
        return camera_.getActualFPS();
    }

    int getTotalFrames() const override {
        return 0;  // 摄像头: 无限流
    }

    int getCurrentFrame() const override {
        return frameCount_;
    }

    int getWidth() const override {
        return cfg_.width;
    }

    int getHeight() const override {
        return cfg_.height;
    }

    std::string typeName() const override {
        return "camera";
    }

private:
    Camera camera_;
    CameraConfig cfg_;
    int frameCount_ = 0;
};
