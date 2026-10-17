#pragma once
// ============================================================================
// ThreadSafeSendQueue  —  生产者-消费者队列（用于串口发送）
// ============================================================================
// 生产者（ProcessThread）： push() 入队检测结果，并唤醒一个等待线程。
// 消费者（SerialThread）：   pop()  非阻塞出队（主循环使用）；
//                            waitAndPop()  带超时的阻塞出队。
//
// 这是从 ProcessThread 到串口的**唯一**数据通路。
// ProcessThread **严禁**直接调用 SerialPort::writeResult()。
// ============================================================================

#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

// ---------------------------------------------------------------------------
// DetectionResult  —  单个目标的串口发送数据
// ---------------------------------------------------------------------------
struct DetectionResult {
    uint8_t target_type;  // 目标类型（协议 TargetType）：
                          //   0 = 无目标帧（0 值包；仅 serial.enable_no_target_frame=true 时推送）
                          //   1 = 颜色物块  2 = 圆环  3 = 圆柱
    uint8_t color_id;     // 槽位号（协议 Color 字段）：
                          //   模式0：外部颜色编号 1-6（= 内部索引 0-5 + 1）
                          //   模式1：环号 1-3
                          //   模式2/3：固定 0
    uint16_t x;           // 全图 X 坐标（主机字节序；writeResult 发送时转大端）
    uint16_t y;           // 全图 Y 坐标（同上）
};

// ---------------------------------------------------------------------------
// ThreadSafeSendQueue
// ---------------------------------------------------------------------------
class ThreadSafeSendQueue {
public:
    ThreadSafeSendQueue() = default;

    /// 生产者：将结果加入队列并通知一个等待线程。线程安全。
    void push(const DetectionResult& result) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(result);
        }
        cv_.notify_one();
    }

    /// 消费者：非阻塞出队。
    /// 队列为空时返回 std::nullopt。
    std::optional<DetectionResult> pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        DetectionResult result = queue_.front();
        queue_.pop();
        return result;
    }

    /// 消费者：带超时的阻塞出队（毫秒）。最多等待 timeout_ms，
    /// 超时返回 std::nullopt。
    std::optional<DetectionResult> waitAndPop(int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this] { return !queue_.empty(); })) {
            return std::nullopt;
        }
        DetectionResult result = queue_.front();
        queue_.pop();
        return result;
    }

    /// 当前队列深度（UI 状态栏用于显示发送积压）。
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<DetectionResult> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};
