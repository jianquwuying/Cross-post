/// @file config.cpp
/// @brief 统一 YAML 配置加载实现。
///
/// 摄像头部分严格解析 — 缺失字段直接 exit(1)。
/// 其他模块保持 IsDefined() 默认值回退（兼容已有逻辑）。

#include "config/config.h"

#include <yaml-cpp/yaml.h>

#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>

// ── YAML 辅助宏 ───────────────────────────────────────────────────
// 安全读取 YAML 节点，节点不存在时静默跳过 (仅用于非摄像头模块)

#define YAML_READ_INT(node, key, field)   \
    if (node[#key].IsDefined()) field = node[#key].as<int>()
#define YAML_READ_FLOAT(node, key, field) \
    if (node[#key].IsDefined()) field = node[#key].as<float>()
#define YAML_READ_BOOL(node, key, field)  \
    if (node[#key].IsDefined()) field = node[#key].as<bool>()
#define YAML_READ_STR(node, key, field)   \
    if (node[#key].IsDefined()) field = node[#key].as<std::string>()

// ── 加载 YAML ─────────────────────────────────────────────────────

AppConfig AppConfig::load(const std::string& yamlPath) {
    AppConfig cfg;

    try {
        YAML::Node root = YAML::LoadFile(yamlPath);

        // ── 摄像头 (严格解析) ─────────────────────────────────
        if (!root["camera"].IsDefined()) {
            std::cerr << "\033[31m[ERROR]\033[0m "
                      << "Missing required 'camera' section in config: "
                      << yamlPath << std::endl;
            std::exit(1);
        }
        cfg.camera = CameraConfig::fromYaml(root["camera"]);

        // ── YOLO ────────────────────────────────────────────
        if (root["yolo"]) {
            auto y = root["yolo"];
            YAML_READ_STR(y, model_dir, cfg.yolo.modelDir);
            YAML_READ_STR(y, model_type, cfg.yolo.modelType);
            YAML_READ_INT(y, input_width, cfg.yolo.inputWidth);
            YAML_READ_INT(y, input_height, cfg.yolo.inputHeight);
            YAML_READ_INT(y, num_classes, cfg.yolo.numClasses);
            YAML_READ_INT(y, num_threads, cfg.yolo.numThreads);
            YAML_READ_FLOAT(y, conf_threshold, cfg.yolo.confThreshold);
            YAML_READ_FLOAT(y, nms_threshold, cfg.yolo.nmsThreshold);
        }

        // ── 检测 ────────────────────────────────────────────
        if (root["detect"]) {
            YAML_READ_INT(root["detect"], skip_frames, cfg.detect.skipFrames);
        }

        // ── ROI ─────────────────────────────────────────────
        if (root["roi"]) {
            auto r = root["roi"];
            YAML_READ_BOOL(r, enabled, cfg.roi.enabled);
            YAML_READ_INT(r, x, cfg.roi.x);
            YAML_READ_INT(r, y, cfg.roi.y);
            YAML_READ_INT(r, width, cfg.roi.width);
            YAML_READ_INT(r, height, cfg.roi.height);
        }

        // ── 颜色 ────────────────────────────────────────────
        if (root["color"]) {
            auto cl = root["color"];
            YAML_READ_FLOAT(cl, threshold, cfg.color.threshold);
            YAML_READ_FLOAT(cl, conf_threshold, cfg.color.confThreshold);
            YAML_READ_INT(cl, morph_kernel, cfg.color.morphKernel);
            YAML_READ_INT(cl, blur_kernel, cfg.color.blurKernel);

            // HSV 范围辅助 lambda
            auto readHSV = [](const YAML::Node& n, AppConfig::HSVRangeCfg& hsv) {
                YAML_READ_INT(n, h_low, hsv.hLow);
                YAML_READ_INT(n, s_low, hsv.sLow);
                YAML_READ_INT(n, v_low, hsv.vLow);
                YAML_READ_INT(n, h_high, hsv.hHigh);
                YAML_READ_INT(n, s_high, hsv.sHigh);
                YAML_READ_INT(n, v_high, hsv.vHigh);
            };
            if (cl["red1"])  readHSV(cl["red1"],  cfg.color.red1);
            if (cl["red2"])  readHSV(cl["red2"],  cfg.color.red2);
            if (cl["blue"])  readHSV(cl["blue"],  cfg.color.blue);
            if (cl["yellow"]) readHSV(cl["yellow"], cfg.color.yellow);
        }

        // ── 串口 ────────────────────────────────────────────
        if (root["serial"]) {
            auto s = root["serial"];
            YAML_READ_STR(s, device, cfg.serial.device);
            YAML_READ_INT(s, baudrate, cfg.serial.baudrate);
            YAML_READ_INT(s, data_bits, cfg.serial.dataBits);
            YAML_READ_INT(s, stop_bits, cfg.serial.stopBits);
            YAML_READ_STR(s, parity, cfg.serial.parity);
        }

        // ── 日志 ────────────────────────────────────────────
        if (root["log"]) {
            YAML_READ_STR(root["log"], level, cfg.log.level);
            YAML_READ_BOOL(root["log"], use_color, cfg.log.useColor);
        }

        std::cout << "[配置] 已加载: " << yamlPath << std::endl;

    } catch (const YAML::Exception& e) {
        std::cerr << "[配置] 警告: YAML 解析失败 (" << e.what()
                  << "), 使用默认配置。" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[配置] 警告: 无法读取配置文件 (" << e.what()
                  << "), 使用默认配置。" << std::endl;
    }

    return cfg;
}

// ── CLI 覆盖 ──────────────────────────────────────────────────────

void AppConfig::applyCliOverrides(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // 配置文件路径
        if (arg == "--config" && i + 1 < argc) {
            // 已在 main() 中处理，此处跳过
            ++i;
        }
        // 模型类型
        else if (arg == "--model" && i + 1 < argc) {
            yolo.modelType = argv[++i];
        }
        // 模型目录
        else if (arg == "--model-dir" && i + 1 < argc) {
            yolo.modelDir = argv[++i];
        }
        // 串口设备
        else if (arg == "--serial" && i + 1 < argc) {
            serial.device = argv[++i];
        }
        // 波特率
        else if (arg == "--baud" && i + 1 < argc) {
            serial.baudrate = std::stoi(argv[++i]);
        }
        // 摄像头
        else if (arg == "--camera" && i + 1 < argc) {
            camera.device = std::stoi(argv[++i]);
        }
        // 输入尺寸
        else if (arg == "--size" && i + 1 < argc) {
            int sz = std::stoi(argv[++i]);
            yolo.inputWidth  = sz;
            yolo.inputHeight = sz;
        }
        // 线程数
        else if (arg == "--threads" && i + 1 < argc) {
            yolo.numThreads = std::stoi(argv[++i]);
        }
        // 置信度
        else if (arg == "--conf" && i + 1 < argc) {
            yolo.confThreshold = std::stof(argv[++i]);
        }
        // 日志级别
        else if (arg == "--log-level" && i + 1 < argc) {
            log.level = argv[++i];
        }
        // 帮助（由 main 处理）
        else if (arg == "--help" || arg == "-h") {
            // main() handles this
        }
    }
}

// ── 打印配置 ──────────────────────────────────────────────────────

void AppConfig::print() const {
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║          当前配置                        ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n";

    std::cout << "[摄像头] 设备=" << camera.device
              << " 后端=" << camera.backend
              << " " << camera.width << "x" << camera.height
              << " @ " << camera.fps << "fps"
              << " " << camera.fourcc
              << " 缓冲=" << camera.buffer_size
              << " 自动曝光=" << (camera.auto_exposure ? "是" : "否")
              << " 曝光=" << camera.exposure
              << " 自动白平衡=" << (camera.auto_white_balance ? "是" : "否")
              << " 白平衡=" << camera.white_balance << "K\n";

    std::cout << "[YOLO] 模型=" << yolo.modelType
              << " 目录=" << yolo.modelDir
              << " 尺寸=" << yolo.inputWidth << "x" << yolo.inputHeight
              << " 线程=" << yolo.numThreads
              << " 置信度=" << yolo.confThreshold
              << " NMS=" << yolo.nmsThreshold
              << " 后端=CPU+NEON+OpenMP\n";

    std::cout << "[检测] 跳帧=" << detect.skipFrames << "\n";

    std::cout << "[ROI] " << (roi.enabled ? "启用" : "禁用");
    if (roi.enabled) {
        std::cout << " (" << roi.x << "," << roi.y
                  << " " << roi.width << "x" << roi.height << ")";
    }
    std::cout << "\n";

    std::cout << "[颜色] 阈值=" << color.threshold
              << " 置信度阈值=" << color.confThreshold
              << " 形态学核=" << color.morphKernel
              << " 模糊核=" << color.blurKernel << "\n";

    std::cout << "[串口] " << serial.device
              << " @ " << serial.baudrate
              << " " << serial.dataBits << serial.parity << serial.stopBits << "\n";

    std::cout << "[日志] 级别=" << log.level
              << " 颜色=" << (log.useColor ? "是" : "否") << "\n\n";
}

// ── 清理宏 ────────────────────────────────────────────────────────
#undef YAML_READ_INT
#undef YAML_READ_FLOAT
#undef YAML_READ_BOOL
#undef YAML_READ_STR
