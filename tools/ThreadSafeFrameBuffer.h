#pragma once
// ============================================================================
// ThreadSafeFrameBuffer<T>  —  单槽帧缓冲
// ============================================================================
// 写入端（CameraThread）：  put() 覆盖尚未被消费的旧帧（拷贝或移动）。
// 读取端（ProcessThread）： get() 取出并清空槽位，空则返回 nullopt。
//
// "单槽、仅保留最新"  ——  旧帧被静默丢弃，读取端永远拿到最新一帧。
// empty() 只查询状态，不消费。
// ============================================================================

#include <mutex>
#include <optional>

template <typename T>
class ThreadSafeFrameBuffer {
public:
    ThreadSafeFrameBuffer() = default;

    /// 生产者：存入一帧。覆盖之前未被读取的帧。
    void put(const T& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_ = frame;
        has_frame_ = true;
    }

    /// 生产者：存入一帧（移动语义版本）。
    void put(T&& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_ = std::move(frame);
        has_frame_ = true;
    }

    /// 消费者：获取最新帧。
    /// 无帧时返回 std::nullopt。
    std::optional<T> get() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (has_frame_) {
            has_frame_ = false;
            return buffer_;
        }
        return std::nullopt;
    }

    /// 查询是否有帧（不消费）。
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !has_frame_;
    }

private:
    mutable std::mutex mutex_;
    T buffer_{};
    bool has_frame_ = false;
};
