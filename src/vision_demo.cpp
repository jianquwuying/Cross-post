/// @file vision_demo.cpp
/// @brief 视觉识别 Demo 程序 — 支持摄像头/视频文件双模式推理。
///
/// 运行:
///   ./vision_demo [--config config/vision.yaml]
///
/// 输入源由 YAML 配置决定, 无需重编译:
///   input.type = "camera"  → 实时摄像头
///   input.type = "video"   → 离线视频文件
///
/// 功能:
///   - YOLO26n 目标检测 (NCNN)
///   - HSV 颜色识别 (仅 A 类)
///   - 实时显示 (检测框 + 类别 + 置信度 + 颜色 + 坐标)
///   - 可选视频保存 (result.mp4)
///   - Benchmark 性能统计

#include "src/input/frame_source.hpp"
#include "src/input/frame_source_factory.hpp"
#include <yaml-cpp/yaml.h>
#include "yolo/yolo26.h"
#include "color/hsv.h"
#include "roi/roi.h"
#include "logger/logger.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

// ═══════════════════════════════════════════════════════════════════
//  全局状态
// ═══════════════════════════════════════════════════════════════════

static std::atomic<bool> g_running{true};

static void signalHandler(int sig) {
    (void)sig;
    g_running = false;
}

// ═══════════════════════════════════════════════════════════════════
//  ColoredObject — 检测结果 (本地定义, 与 main.cpp 一致)
// ═══════════════════════════════════════════════════════════════════

struct ColoredObject {
    int    classId      = -1;
    float  confidence   = 0.0f;
    float  x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float  cx = 0, cy = 0;
    Color  colorId          = Color::Unknown;
    float  colorConfidence  = 0.0f;
};

static ColoredObject fromYoloObject(const Object& obj) {
    ColoredObject co;
    co.classId    = obj.label;
    co.confidence = obj.confidence;
    co.x1 = obj.x1(); co.y1 = obj.y1();
    co.x2 = obj.x2(); co.y2 = obj.y2();
    co.cx = obj.cx(); co.cy = obj.cy();
    return co;
}

// ═══════════════════════════════════════════════════════════════════
//  Benchmark 统计
// ═══════════════════════════════════════════════════════════════════

struct Benchmark {
    int   totalFrames    = 0;
    double totalPreMs    = 0.0;
    double totalInferMs  = 0.0;
    double totalPostMs   = 0.0;
    double totalDrawMs   = 0.0;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    void print() const {
        auto endTime = std::chrono::steady_clock::now();
        double totalSec = std::chrono::duration<double>(endTime - startTime).count();
        double avgFps = totalSec > 0 ? totalFrames / totalSec : 0;

        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════╗\n";
        std::cout << "║           Benchmark 结果                  ║\n";
        std::cout << "╠══════════════════════════════════════════╣\n";
        std::cout << "║  总帧数:     " << totalFrames << "\n";
        std::cout << "║  总耗时:     " << totalSec << " s\n";
        std::cout << "║  平均 FPS:   " << avgFps << "\n";

        if (totalFrames > 0) {
            double n = static_cast<double>(totalFrames);
            std::cout << "╠══════════════════════════════════════════╣\n";
            std::cout << "║  预处理:     " << totalPreMs / n   << " ms\n";
            std::cout << "║  推理:       " << totalInferMs / n << " ms\n";
            std::cout << "║  后处理:     " << totalPostMs / n  << " ms\n";
            std::cout << "║  绘制:       " << totalDrawMs / n  << " ms\n";
        }
        std::cout << "╚══════════════════════════════════════════╝\n\n";
    }
};

// ═══════════════════════════════════════════════════════════════════
//  绘制函数
// ═══════════════════════════════════════════════════════════════════

/// 类别对应的边界框颜色
static cv::Scalar boxColor(int classId) {
    return classId == 0 ? cv::Scalar(0, 255, 0)   // A类: 绿色
                        : cv::Scalar(255, 0, 0);   // B类: 蓝色
}

/// 在帧上绘制单个检测结果
static void drawDetection(cv::Mat& frame, const ColoredObject& co) {
    const int x1 = static_cast<int>(co.x1);
    const int y1 = static_cast<int>(co.y1);
    const int x2 = static_cast<int>(co.x2);
    const int y2 = static_cast<int>(co.y2);

    // 边界框
    cv::Scalar color = boxColor(co.classId);
    cv::rectangle(frame, cv::Point(x1, y1), cv::Point(x2, y2), color, 2);

    // 中心点十字线
    int cx = static_cast<int>(co.cx);
    int cy = static_cast<int>(co.cy);
    cv::drawMarker(frame, cv::Point(cx, cy), cv::Scalar(0, 255, 255),
                   cv::MARKER_CROSS, 12, 2);

    // ── 文字信息 (左上角标注) ──────────────────────────────────
    int baseY = y1 - 10;
    if (baseY < 20) baseY = y2 + 20;  // 框太靠上时放到框下方

    const float fontScale = 0.5f;
    const int thickness = 1;
    const int lineHeight = 18;

    // 类别 + YOLO 置信度
    std::string label = "Class: " + std::string(co.classId == 0 ? "A" : "B");
    cv::putText(frame, label, cv::Point(x1, baseY),
                cv::FONT_HERSHEY_SIMPLEX, fontScale, color, thickness);
    baseY += lineHeight;

    std::string confStr = "conf: " + std::to_string(co.confidence).substr(0, 4);
    cv::putText(frame, confStr, cv::Point(x1, baseY),
                cv::FONT_HERSHEY_SIMPLEX, fontScale, color, thickness);
    baseY += lineHeight;

    // 颜色 + 颜色置信度 (仅 A 类)
    if (co.classId == 0 && co.colorId != Color::Unknown) {
        std::string colorStr = "color: " + std::string(colorName(co.colorId));
        cv::putText(frame, colorStr, cv::Point(x1, baseY),
                    cv::FONT_HERSHEY_SIMPLEX, fontScale,
                    cv::Scalar(0, 255, 255), thickness);
        baseY += lineHeight;

        std::string ccStr = "color_conf: " + std::to_string(co.colorConfidence).substr(0, 4);
        cv::putText(frame, ccStr, cv::Point(x1, baseY),
                    cv::FONT_HERSHEY_SIMPLEX, fontScale,
                    cv::Scalar(0, 255, 255), thickness);
        baseY += lineHeight;
    }

    // 中心坐标
    std::string centerStr = "(" + std::to_string(cx) + ", " + std::to_string(cy) + ")";
    cv::putText(frame, centerStr, cv::Point(x1, baseY),
                cv::FONT_HERSHEY_SIMPLEX, fontScale,
                cv::Scalar(200, 200, 200), thickness);
}

/// 在帧上绘制帧计数器/FPS
static void drawStatus(cv::Mat& frame, int frameNum, int totalFrames, double fps) {
    std::string status;
    if (totalFrames > 0) {
        status = "Frame: " + std::to_string(frameNum) + "/" + std::to_string(totalFrames);
    } else {
        status = "Frame: " + std::to_string(frameNum);
    }
    status += "  FPS: " + std::to_string(static_cast<int>(fps));

    cv::putText(frame, status, cv::Point(10, 30),
                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
}

// ═══════════════════════════════════════════════════════════════════
//  主函数
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ── 解析 --config ──────────────────────────────────────────
    std::string configPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "用法: " << argv[0] << " [--config PATH]\n"
                      << "  --config PATH  配置文件路径 (默认: 自动查找)\n"
                      << "  --help, -h     显示本帮助\n"
                      << "\n输入源由 YAML 中的 input.type 决定: camera | video\n"
                      << "自动搜索 (基于 /proc/self/exe): <exe>/../config/vision.yaml\n";
            return 0;
        }
    }

    // ── 自动搜索配置文件 (基于二进制位置) ──────────────────────
    if (configPath.empty()) {
        std::vector<std::string> candidates;

        // 通过 /proc/self/exe 获取二进制所在目录
        std::error_code ec;
        auto exePath = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            std::string exeDir = std::filesystem::path(exePath).parent_path().string();
            candidates.push_back(exeDir + "/../config/vision.yaml");  // build/ → config/
        }

        // CWD 回退
        candidates.push_back("../config/vision.yaml");
        candidates.push_back("config/vision.yaml");

        for (const auto& p : candidates) {
            if (std::filesystem::exists(p)) {
                configPath = p;
                break;
            }
        }

        if (configPath.empty()) {
            std::cerr << "\033[31m[ERROR]\033[0m "
                      << "找不到配置文件。搜索路径:\n";
            for (const auto& p : candidates) {
                std::cerr << "  " << p << "\n";
            }
            std::cerr << "\n请通过 --config 指定配置文件路径。\n";
            return 1;
        }
        std::cout << "\033[32m[INFO]\033[0m  "
                  << "自动发现配置: " << configPath << "\n";
    }

    // ── 加载 YAML ──────────────────────────────────────────────
    Logger::setLevel(LogLevel::INFO);
    Logger::setUseColor(true);

    std::cout << "╔══════════════════════════════════════════════════╗\n";
    std::cout << "║  视觉识别 Demo — YOLO26n + HSV Color             ║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n";

    LOG_INFO << "加载配置: " << configPath;

    // ── 创建输入源 ────────────────────────────────────────────
    auto source = FrameSourceFactory::createFromYaml(configPath);
    if (!source) {
        LOG_ERROR << "无法创建输入源, 请检查配置文件";
        return 1;
    }

    if (!source->open()) {
        LOG_ERROR << "无法打开输入源: " << source->typeName();
        return 1;
    }

    LOG_INFO << "输入源: " << source->typeName()
             << " (" << source->getWidth() << "x" << source->getHeight()
             << " @ " << source->getFps() << "fps)";

    if (source->getTotalFrames() > 0) {
        LOG_INFO << "总帧数: " << source->getTotalFrames();
    }

    // ── 读取 YAML 参数 ────────────────────────────────────────
    // (简化: 直接解析关键字段, 避免重复完整的 AppConfig)
    YAML::Node root = YAML::LoadFile(configPath);

    // YOLO 配置
    YOLO26Detector::Config ycfg;
    if (root["yolo"].IsDefined()) {
        auto y = root["yolo"];
        if (y["model_dir"].IsDefined())   ycfg.modelDir   = y["model_dir"].as<std::string>();
        if (y["model_type"].IsDefined()) {
            ycfg.modelType = y["model_type"].as<std::string>() == "int8"
                           ? ModelType::INT8 : ModelType::FP16;
        }
        if (y["input_width"].IsDefined())    ycfg.inputWidth    = y["input_width"].as<int>();
        if (y["input_height"].IsDefined())   ycfg.inputHeight   = y["input_height"].as<int>();
        if (y["num_threads"].IsDefined())    ycfg.numThreads    = y["num_threads"].as<int>();
        if (y["conf_threshold"].IsDefined()) ycfg.confThreshold = y["conf_threshold"].as<float>();
        if (y["nms_threshold"].IsDefined())  ycfg.nmsThreshold  = y["nms_threshold"].as<float>();
    }

    YOLO26Detector yolo;
    if (!yolo.load(ycfg)) {
        LOG_ERROR << "YOLO 模型加载失败";
        return 1;
    }

    // HSV 分类器
    HSVClassifier::Config hsvCfg;
    if (root["color"].IsDefined()) {
        auto c = root["color"];
        if (c["threshold"].IsDefined())      hsvCfg.colorThreshold     = c["threshold"].as<float>();
        if (c["conf_threshold"].IsDefined()) hsvCfg.colorConfThreshold  = c["conf_threshold"].as<float>();
    }
    HSVClassifier hsv(hsvCfg);

    // ROI
    RoiCrop roi;
    if (root["roi"].IsDefined()) {
        RoiConfig rc;
        auto r = root["roi"];
        if (r["enabled"].IsDefined()) rc.enabled = r["enabled"].as<bool>();
        if (r["x"].IsDefined()) rc.x = r["x"].as<int>();
        if (r["y"].IsDefined()) rc.y = r["y"].as<int>();
        if (r["width"].IsDefined())  rc.width  = r["width"].as<int>();
        if (r["height"].IsDefined()) rc.height = r["height"].as<int>();
        roi.configure(rc);
    }

    // 显示
    bool displayEnable = true;
    std::string windowName = "YOLO26n Vision";
    if (root["display"].IsDefined()) {
        auto d = root["display"];
        if (d["enable"].IsDefined()) displayEnable = d["enable"].as<bool>();
        if (d["window_name"].IsDefined()) windowName = d["window_name"].as<std::string>();
    }

    // 视频输出
    bool saveVideo = false;
    std::string outputPath = "./result.mp4";
    if (root["output"].IsDefined()) {
        auto o = root["output"];
        if (o["save_video"].IsDefined()) saveVideo = o["save_video"].as<bool>();
        if (o["path"].IsDefined()) outputPath = o["path"].as<std::string>();
    }

    // Benchmark
    bool benchmarkEnable = true;
    if (root["benchmark"].IsDefined()) {
        auto b = root["benchmark"];
        if (b["enable"].IsDefined()) benchmarkEnable = b["enable"].as<bool>();
    }

    // ── 视频写入器 ────────────────────────────────────────────
    cv::VideoWriter videoWriter;
    if (saveVideo) {
        int fourcc = cv::VideoWriter::fourcc('a', 'v', 'c', '1');
        videoWriter.open(outputPath, fourcc, source->getFps(),
                         cv::Size(source->getWidth(), source->getHeight()));
        if (videoWriter.isOpened()) {
            LOG_INFO << "视频保存: " << outputPath;
        } else {
            LOG_WARN << "无法创建视频文件: " << outputPath;
            saveVideo = false;
        }
    }

    // ── 显示窗口 ──────────────────────────────────────────────
    if (displayEnable) {
        cv::namedWindow(windowName, cv::WINDOW_AUTOSIZE);
    }

    // ── 主循环 ────────────────────────────────────────────────
    Benchmark bench;
    cv::Mat frame;
    double instantFps = 0.0;
    auto lastFrameTime = std::chrono::steady_clock::now();

    LOG_INFO << "推理开始, 按 Q 或 ESC 退出...";

    while (g_running && source->read(frame)) {
        if (frame.empty()) continue;

        auto t0 = std::chrono::steady_clock::now();

        // ── 预处理: ROI 裁剪 ───────────────────────────────────
        cv::Mat processedFrame = roi.isEnabled() ? roi.apply(frame) : frame;
        auto t1 = std::chrono::steady_clock::now();

        // ── YOLO 推理 ──────────────────────────────────────────
        std::vector<Object> objects;
        if (!yolo.detect(processedFrame, objects)) {
            objects.clear();
        }
        auto t2 = std::chrono::steady_clock::now();

        // ── 后处理: HSV 颜色分类 + 坐标映射 ────────────────────
        std::vector<ColoredObject> colored;
        colored.reserve(objects.size());

        for (auto& obj : objects) {
            ColoredObject co = fromYoloObject(obj);

            // ROI 坐标映射
            if (roi.isEnabled()) {
                cv::Rect_<float> mapped = roi.mapBack(obj.rect);
                co.x1 = mapped.x;
                co.y1 = mapped.y;
                co.x2 = mapped.x + mapped.width;
                co.y2 = mapped.y + mapped.height;
                co.cx = mapped.x + mapped.width  * 0.5f;
                co.cy = mapped.y + mapped.height * 0.5f;
            }

            // HSV 颜色 (仅 A 类)
            if (obj.label == 0) {
                cv::Rect roiRect(
                    std::max(0, static_cast<int>(obj.x1())),
                    std::max(0, static_cast<int>(obj.y1())),
                    std::min(static_cast<int>(obj.w()), processedFrame.cols),
                    std::min(static_cast<int>(obj.h()), processedFrame.rows));
                if (roiRect.width > 0 && roiRect.height > 0) {
                    ColorResult cr = hsv.classify(processedFrame(roiRect));
                    co.colorId         = cr.color;
                    co.colorConfidence = cr.confidence;
                }
            }
            colored.push_back(std::move(co));
        }
        auto t3 = std::chrono::steady_clock::now();

        // ── 绘制 ───────────────────────────────────────────────
        for (const auto& co : colored) {
            drawDetection(frame, co);
        }

        // 帧计数器
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - lastFrameTime).count();
        if (elapsed > 0.0) instantFps = 1.0 / elapsed;
        lastFrameTime = now;

        drawStatus(frame, source->getCurrentFrame(),
                   source->getTotalFrames(), instantFps);
        auto t4 = std::chrono::steady_clock::now();

        // ── Benchmark 累加 ─────────────────────────────────────
        if (benchmarkEnable) {
            ++bench.totalFrames;
            bench.totalPreMs   += std::chrono::duration<double, std::milli>(t1 - t0).count();
            bench.totalInferMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
            bench.totalPostMs  += std::chrono::duration<double, std::milli>(t3 - t2).count();
            bench.totalDrawMs  += std::chrono::duration<double, std::milli>(t4 - t3).count();
        }

        // ── 显示 ───────────────────────────────────────────────
        if (displayEnable) {
            cv::imshow(windowName, frame);
            int key = cv::waitKey(1);
            if (key == 'q' || key == 'Q' || key == 27) {  // q / ESC
                g_running = false;
                break;
            }
        }

        // ── 视频保存 ───────────────────────────────────────────
        if (saveVideo && videoWriter.isOpened()) {
            videoWriter.write(frame);
        }
    }

    // ── 清理 ──────────────────────────────────────────────────
    source->close();

    if (displayEnable) {
        cv::destroyWindow(windowName);
    }

    if (saveVideo && videoWriter.isOpened()) {
        videoWriter.release();
        LOG_INFO << "视频已保存: " << outputPath;
    }

    // ── Benchmark 输出 ─────────────────────────────────────────
    if (benchmarkEnable && bench.totalFrames > 0) {
        bench.print();
    }

    LOG_INFO << "推理结束。";
    return 0;
}
