/// @file config.h
/// @brief 统一 YAML 配置管理系统。
///
/// 所有可调参数由 YAML 文件控制，修改参数无需重新编译。
/// 依赖: yaml-cpp (libyaml-cpp-dev)
///
/// CLI 参数可覆盖 YAML 中的值，优先级: CLI > YAML。
/// 摄像头参数统一使用 CameraConfig (camera/camera.h)，不允许默认值。

#pragma once

#include "camera/camera.h"

#include <set>
#include <string>
#include <vector>

// ── 统一配置结构体 ────────────────────────────────────────────────

struct AppConfig {
    // ── 摄像头 (来自 camera/camera.h，无默认值) ───────────────
    CameraConfig camera;

    // ── YOLO 检测 ─────────────────────────────────────────────
    struct YoloCfg {
        std::string modelDir    = "model";
        std::string modelType   = "fp16";   ///< "fp16" | "int8"
        int    inputWidth       = 320;
        int    inputHeight      = 320;
        int    numClasses       = 2;
        int    numThreads       = 4;
        float  confThreshold    = 0.25f;
        float  nmsThreshold     = 0.45f;
    } yolo;

    // ── 检测控制 ──────────────────────────────────────────────
    struct DetectCfg {
        int    skipFrames          = 0;      ///< 跳帧: 0=每帧检测, 1=隔帧, 2=每3帧
        float  confThreshold       = 0.45f;  ///< DetectionFilter 最低置信度
        int    maxTargets          = 10;     ///< Top-K 最多保留数
        std::set<int> allowedClasses = {0, 1, 2};  ///< 类别白名单
    } detect;

    // ── ROI 裁剪 ──────────────────────────────────────────────
    struct RoiCfg {
        bool enabled = false;
        int  x = 0, y = 0;
        int  width = 640, height = 480;
    } roi;

    // ── 颜色识别 ──────────────────────────────────────────────
    struct HSVRangeCfg {
        int hLow = 0, sLow = 100, vLow = 100;
        int hHigh = 10, sHigh = 255, vHigh = 255;
    };

    struct ColorCfg {
        float threshold      = 0.15f;   ///< 颜色判定像素占比阈值
        float confThreshold  = 0.15f;   ///< 颜色置信度最低阈值
        int   morphKernel    = 3;       ///< 形态学核大小
        int   blurKernel     = 3;       ///< 高斯模糊核大小
        HSVRangeCfg red1   = {  0, 100, 100,  10, 255, 255 };
        HSVRangeCfg red2   = {170, 100, 100, 180, 255, 255 };
        HSVRangeCfg blue   = {100, 100,  50, 130, 255, 255 };
        HSVRangeCfg yellow = { 20, 100, 100,  35, 255, 255 };
    } color;

    // ── 串口 ──────────────────────────────────────────────────
    struct SerialCfg {
        std::string device   = "/dev/ttyAMA0";
        int    baudrate      = 115200;
        int    dataBits      = 8;
        int    stopBits      = 1;
        std::string parity   = "N";
    } serial;

    // ── 日志 ──────────────────────────────────────────────────
    struct LogCfg {
        std::string level = "info";    ///< "debug" | "info" | "warn" | "error"
        bool  useColor    = true;
    } log;

    // ═══════════════════════════════════════════════════════════
    //  方法
    // ═══════════════════════════════════════════════════════════

    /// 从 YAML 文件加载配置
    /// @param yamlPath  YAML 文件路径
    /// @return 解析后的配置（摄像头字段缺失时直接 exit(1)）
    static AppConfig load(const std::string& yamlPath);

    /// CLI 参数覆盖 YAML 配置值
    /// @param argc, argv  命令行参数
    void applyCliOverrides(int argc, char* argv[]);

    /// 打印当前配置到终端（调试用）
    void print() const;
};
