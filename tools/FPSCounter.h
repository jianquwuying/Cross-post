#pragma once
// ============================================================================
// FPSCounter.h  —  轻量级帧率计算器
// ============================================================================
// 分段窗口（tumbling window）统计，不是滑动窗口：
//   从窗口起点累计帧数，距起点 ≥1.0 s 时用「帧数 / 实际耗时」更新 FPS，
//   随后清空计数，并以当前时刻作为下一个窗口的起点。
// 单线程使用：内部不加锁，每个线程各持有一个实例，不要跨线程共享。
// ============================================================================

#include <chrono>

class FPSCounter {
public:
    FPSCounter() : last_time_(std::chrono::steady_clock::now()) {}

    /// 每帧调用一次。窗口满 1 秒后计算 FPS 并开启新窗口；
    /// 两次更新之间 fps() 保持上一次的结果。
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

    /// 最近一个完整窗口的 FPS；窗口尚未完成（含刚 reset）时返回 0.0。
    double fps() const { return fps_; }

    /// 清空计数与 FPS，并以当前时刻作为新窗口起点。
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
