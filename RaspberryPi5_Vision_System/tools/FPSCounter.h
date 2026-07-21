#pragma once
// ============================================================================
// FPSCounter.h  —  轻量级帧率计算器
// ============================================================================
// 线程安全：每个线程应持有独立的实例。
// 每 ~1 秒窗口更新一次 FPS。
// ============================================================================

#include <chrono>

class FPSCounter {
public:
    FPSCounter() : last_time_(std::chrono::steady_clock::now()) {}

    /// 每帧调用一次。约每 1 秒重新计算 FPS。
    void tick() {
        frame_count_++;
        auto now = std::chrono::steady_clock::now();
        double elapsed =
            std::chrono::duration<double>(now - last_time_).count();
        if (elapsed >= 1.0) {
            fps_ = static_cast<double>(frame_count_) / elapsed;
            frame_count_ = 0;
            last_time_ = now;
        }
    }

    /// 当前 FPS（首个窗口完成前返回 0.0）。
    double fps() const { return fps_; }

    /// 重置内部计数器。
    void reset() {
        frame_count_ = 0;
        fps_ = 0.0;
        last_time_ = std::chrono::steady_clock::now();
    }

private:
    int frame_count_ = 0;
    double fps_ = 0.0;
    std::chrono::steady_clock::time_point last_time_;
};
