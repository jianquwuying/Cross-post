// ============================================================================
// main.cpp  —  程序入口 + 线程编排
// ============================================================================
// 职责：
//   1. 加载 YAML 配置
//   2. 初始化硬件模块
//   3. 启动 3 个线程（Camera、Process、Serial）
//   4. 处理 SIGINT / SIGTERM 以优雅退出
//   5. 等待线程完成并释放资源
//
// **禁止**在此文件中写入视觉算法、串口协议或滤波逻辑。
// ============================================================================

#include "CameraCapture.h"
#include "ColorOrder.h"
#include "ConfigManager.h"
#include "FPSCounter.h"
#include "Logger.h"
#include "SerialPort.h"
#include "ThreadSafeFrameBuffer.h"
#include "ThreadSafeSendQueue.h"
#include "VisionPipeline.h"

#include <poll.h>

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// 帧数据（图像 + 捕获时间戳，用于延迟测量）
// ---------------------------------------------------------------------------
struct FrameData {
    cv::Mat image;
    std::chrono::steady_clock::time_point capture_time;
};

// ---------------------------------------------------------------------------
// 全局运行标志（信号处理器写入，所有线程读取）
// ---------------------------------------------------------------------------
static std::atomic<bool> running_{true};

// ---------------------------------------------------------------------------
// 信号处理器 —— 异步信号安全：仅 atomic store
// ---------------------------------------------------------------------------
static void signalHandler(int /*signum*/) {
    running_.store(false);
}

// ========================================================================
// CameraThread  摄像头线程
// ========================================================================
static void cameraLoop(CameraCapture& camera,
                       ThreadSafeFrameBuffer<FrameData>& frame_buffer,
                       FPSCounter& fps_counter,
                       bool record_mode,
                       const std::string& record_dir,
                       const std::string& record_fourcc,
                       double record_fps) {
    LOG_INFO("CameraThread 已启动");

    // ---- 视频录制器 ---------------------------------------------------
    cv::VideoWriter video_writer;
    if (record_mode) {
        // 生成带时间戳的文件名
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        char ts_buf[64];
        std::strftime(ts_buf, sizeof(ts_buf),
                      "%Y%m%d_%H%M%S", std::localtime(&t));

        std::string filename = record_dir + "/vision_" + ts_buf + ".avi";
        int fourcc = cv::VideoWriter::fourcc(
            record_fourcc[0], record_fourcc[1],
            record_fourcc[2], record_fourcc[3]);

        video_writer.open(filename, fourcc, record_fps,
                          cv::Size(ConfigManager::instance().cameraWidth(),
                                   ConfigManager::instance().cameraHeight()));
        if (!video_writer.isOpened()) {
            LOG_WARN("无法打开视频录制器: %s", filename.c_str());
        } else {
            LOG_INFO("正在录制原始视频到: %s", filename.c_str());
        }
    }

    // ---- 主循环 -------------------------------------------------------
    while (running_) {
        auto frame_opt = camera.read();
        if (!frame_opt.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        cv::Mat frame = frame_opt.value();
        auto capture_ts = std::chrono::steady_clock::now();

        // 录制原始帧（无 UI 叠加层）
        if (record_mode && video_writer.isOpened()) {
            video_writer.write(frame);
        }

        // 推入缓冲区（覆盖未被消费的帧）
        FrameData fd;
        fd.image        = frame;
        fd.capture_time = capture_ts;
        frame_buffer.put(std::move(fd));

        fps_counter.tick();
    }

    if (video_writer.isOpened()) {
        video_writer.release();
        LOG_INFO("视频录制已保存");
    }

    LOG_INFO("CameraThread 正在退出");
}

// ========================================================================
// ProcessThread  处理线程
// ========================================================================
static void processLoop(ThreadSafeFrameBuffer<FrameData>& frame_buffer,
                        ThreadSafeSendQueue& send_queue,
                        VisionPipeline& pipeline,
                        FPSCounter& fps_counter,
                        bool imshow_enable,
                        bool ui_draw_enable,
                        const std::string& window_name) {
    LOG_INFO("ProcessThread 已启动");

    double camera_fps     = 0.0;
    double process_fps    = 0.0;
    double latency_ms     = 0.0;
    int    current_mode   = 3;
    size_t queue_depth    = 0;
    bool   serial_ok      = true;  // 占位

    cv::Mat display_frame;

    while (running_) {
        auto fd_opt = frame_buffer.get();
        if (!fd_opt.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto t_start = std::chrono::steady_clock::now();
        auto& fd = fd_opt.value();

        // 计算捕获到处理的延迟（捕获 → 当前）
        double cap_latency =
            std::chrono::duration<double, std::milli>(
                t_start - fd.capture_time).count();

        // ---- 视觉处理 -------------------------------------------------
        auto results = pipeline.process(fd.image);
        current_mode = pipeline.currentMode();

        // ---- 将结果推入发送队列 ---------------------------------------
        if (!results.empty()) {
            for (const auto& r : results) {
                send_queue.push(r);
            }
        }

        auto t_end = std::chrono::steady_clock::now();
        double proc_time =
            std::chrono::duration<double, std::milli>(
                t_end - t_start).count();

        latency_ms  = cap_latency + proc_time;
        queue_depth = send_queue.size();
        fps_counter.tick();
        process_fps = fps_counter.fps();

        // ---- UI 显示 --------------------------------------------------
        if (imshow_enable) {
            display_frame = fd.image.clone();

            if (ui_draw_enable) {
                const auto& cfg = ConfigManager::instance();

                // 绘制 ROI 矩形
                cv::Rect roi_rect(cfg.roiX(), cfg.roiY(),
                                  cfg.roiWidth(), cfg.roiHeight());
                cv::rectangle(display_frame, roi_rect,
                              cv::Scalar(0, 255, 0), 1);

                // 绘制检测结果
                for (const auto& r : results) {
                    cv::Point pt(r.x, r.y);
                    cv::Scalar color(0, 255, 255);  // 黄色
                    cv::circle(display_frame, pt, 5, color, -1);
                    cv::circle(display_frame, pt, 10, color, 1);

                    std::string label;
                    if (r.target_type == 0) {
                        label = std::string(kColorNames[r.color_id]);
                    } else if (r.target_type == 1) {
                        label = "圆环" + std::to_string(r.color_id);
                    } else {
                        label = "圆柱";
                    }
                    cv::putText(display_frame, label,
                                cv::Point(r.x + 12, r.y - 5),
                                cv::FONT_HERSHEY_SIMPLEX,
                                0.5, color, 1);
                }

                // ---- 状态栏（左上角）-----------------------------------
                int y_offset = 25;
                int x_pos    = 10;
                auto putStat = [&](const std::string& text,
                                   const cv::Scalar& colour) {
                    cv::putText(display_frame, text,
                                cv::Point(x_pos, y_offset),
                                cv::FONT_HERSHEY_SIMPLEX,
                                0.55, colour, 2);
                    y_offset += 22;
                };

                char buf[128];

                std::snprintf(buf, sizeof(buf),
                              "摄像头 FPS: %.1f", camera_fps);
                putStat(buf, cv::Scalar(0, 255, 0));

                std::snprintf(buf, sizeof(buf),
                              "处理 FPS: %.1f", process_fps);
                putStat(buf, cv::Scalar(0, 255, 0));

                std::snprintf(buf, sizeof(buf),
                              "延迟: %.1f ms", latency_ms);
                putStat(buf, cv::Scalar(0, 255, 255));

                const char* mode_names[] = {
                    "模式0（颜色物块）",
                    "模式1（圆环检测）",
                    "模式2（黑色圆柱）",
                    "模式3（空闲）"
                };
                std::snprintf(buf, sizeof(buf),
                              "模式: %s", mode_names[current_mode]);
                putStat(buf, cv::Scalar(255, 255, 0));

                std::snprintf(buf, sizeof(buf),
                              "队列: %zu", queue_depth);
                putStat(buf, cv::Scalar(200, 200, 200));

                std::snprintf(buf, sizeof(buf),
                              "串口: %s", serial_ok ? "正常" : "错误");
                putStat(buf, serial_ok
                        ? cv::Scalar(0, 255, 0)
                        : cv::Scalar(0, 0, 255));
            }

            cv::imshow(window_name, display_frame);
            cv::waitKey(1);
        }
    }

    LOG_INFO("ProcessThread 正在退出");
}

// ========================================================================
// SerialThread  串口线程
// ========================================================================
static void serialLoop(SerialPort& serial,
                       ThreadSafeSendQueue& send_queue,
                       VisionPipeline& pipeline) {
    LOG_INFO("SerialThread 已启动");

    struct pollfd pfd;
    pfd.fd     = serial.fd();
    pfd.events = POLLIN;

    while (running_) {
        // ---- 接收模式命令 ----------------------------------------------
        int ret = poll(&pfd, 1, 10);   // 10 ms poll 超时
        if (ret > 0 && (pfd.revents & POLLIN)) {
            // 排空所有可用命令
            for (int i = 0; i < 8; ++i) {   // 每次迭代最多 8 条命令
                auto cmd = serial.readCommand();
                if (!cmd.has_value()) break;
                pipeline.setMode(cmd.value());
            }
        }

        // ---- 排空发送队列（非阻塞）-------------------------------------
        for (int i = 0; i < 16; ++i) {   // 每次迭代最多 16 次发送
            auto result = send_queue.pop();
            if (!result.has_value()) break;

            const auto& r = result.value();
            if (!serial.writeResult(r.target_type, r.color_id, r.x, r.y)) {
                // writeResult 已记录 WARN 日志
                // 继续 —— 不阻塞
            }
        }
    }

    LOG_INFO("SerialThread 正在退出");
}

// ========================================================================
// main  主函数
// ========================================================================
int main(int argc, char* argv[]) {
    // ---- 定位配置文件 ------------------------------------------------
    std::string config_path = "config/vision.yaml";
    if (argc >= 2) {
        config_path = argv[1];
    }

    LOG_INFO("========================================");
    LOG_INFO("RaspberryPi5 视觉系统  v1.0");
    LOG_INFO("========================================");

    // ---- 1. 加载 YAML ------------------------------------------------
    auto& cfg = ConfigManager::instance();
    if (!cfg.load(config_path)) {
        LOG_ERROR("配置加载失败，退出。");
        return EXIT_FAILURE;
    }

    // ---- 2. 打开摄像头 ------------------------------------------------
    CameraCapture camera;
    if (!camera.open()) {
        LOG_ERROR("摄像头打开失败，退出。");
        return EXIT_FAILURE;
    }

    // ---- 3. 打开串口 --------------------------------------------------
    SerialPort serial;
    if (!serial.open(cfg.serialPort(), cfg.serialBaudrate())) {
        LOG_ERROR("串口打开失败，退出。");
        return EXIT_FAILURE;
    }

    // ---- 4. 创建视觉管线 ----------------------------------------------
    VisionPipeline pipeline;

    // ---- 5. 创建共享数据结构 ------------------------------------------
    ThreadSafeFrameBuffer<FrameData> frame_buffer;
    ThreadSafeSendQueue              send_queue;

    // ---- 6. 注册信号处理器 --------------------------------------------
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ---- 7. 创建线程 --------------------------------------------------
    FPSCounter fps_camera;
    FPSCounter fps_process;

    bool imshow_en = cfg.imshowEnable();
    bool draw_en   = cfg.uiDrawEnable();
    bool rec_mode  = cfg.recordMode();
    std::string rec_dir    = cfg.recordOutputDir();
    std::string rec_fourcc = cfg.recordFourcc();
    double      rec_fps    = cfg.recordFps();

    std::thread camera_th(cameraLoop,
                          std::ref(camera),
                          std::ref(frame_buffer),
                          std::ref(fps_camera),
                          rec_mode,
                          rec_dir,
                          rec_fourcc,
                          rec_fps);

    std::thread process_th(processLoop,
                           std::ref(frame_buffer),
                           std::ref(send_queue),
                           std::ref(pipeline),
                           std::ref(fps_process),
                           imshow_en,
                           draw_en,
                           cfg.windowName());

    std::thread serial_th(serialLoop,
                          std::ref(serial),
                          std::ref(send_queue),
                          std::ref(pipeline));

    LOG_INFO("所有线程已启动。按 Ctrl+C 退出。");

    // ---- 8. 等待线程 --------------------------------------------------
    camera_th.join();
    process_th.join();
    serial_th.join();

    // ---- 9. 清理 ------------------------------------------------------
    camera.release();
    serial.close();
    cv::destroyAllWindows();

    LOG_INFO("视觉系统已干净关闭。");
    return EXIT_SUCCESS;
}
