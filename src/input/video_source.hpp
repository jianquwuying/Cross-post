/// @file video_source.hpp
/// @brief 视频文件输入源 — 基于 cv::VideoCapture 封装为 FrameSource 接口。

#pragma once

#include "src/input/frame_source.hpp"
#include <opencv2/videoio.hpp>
#include <string>

class VideoSource : public FrameSource {
public:
    /// @param path  视频文件路径 (mp4, avi, mov 等)
    /// @param loop  播放结束后是否循环
    explicit VideoSource(const std::string& path, bool loop = false)
        : path_(path), loop_(loop) {}

    bool open() override {
        if (!cap_.open(path_)) {
            return false;
        }
        totalFrames_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_COUNT));
        fps_ = cap_.get(cv::CAP_PROP_FPS);
        if (fps_ <= 0.0) fps_ = 30.0;  // 回退默认值
        width_  = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
        height_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
        currentFrame_ = 0;
        return true;
    }

    bool read(cv::Mat& frame) override {
        if (!cap_.isOpened()) return false;

        bool ok = cap_.read(frame);
        if (ok && !frame.empty()) {
            ++currentFrame_;
            return true;
        }

        // 到达文件末尾 — 如果启用循环则回到开头
        if (loop_ && totalFrames_ > 0) {
            cap_.set(cv::CAP_PROP_POS_FRAMES, 0);
            currentFrame_ = 0;
            ok = cap_.read(frame);
            if (ok && !frame.empty()) {
                ++currentFrame_;
                return true;
            }
        }

        return false;
    }

    void close() override {
        if (cap_.isOpened()) {
            cap_.release();
        }
        currentFrame_ = 0;
    }

    bool isOpened() const override {
        return cap_.isOpened();
    }

    double getFps() const override {
        return fps_;
    }

    int getTotalFrames() const override {
        return totalFrames_;
    }

    int getCurrentFrame() const override {
        return currentFrame_;
    }

    int getWidth() const override {
        return width_;
    }

    int getHeight() const override {
        return height_;
    }

    std::string typeName() const override {
        return "video";
    }

private:
    cv::VideoCapture cap_;
    std::string path_;
    bool loop_ = false;
    int totalFrames_ = 0;
    int currentFrame_ = 0;
    double fps_ = 30.0;
    int width_ = 640, height_ = 480;
};
