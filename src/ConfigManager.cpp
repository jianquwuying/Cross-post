// ============================================================================
// ConfigManager.cpp  —  YAML 配置实现
// ============================================================================

#include "ConfigManager.h"
#include "Logger.h"
#include <cstdlib>
#include <yaml-cpp/yaml.h>

// ---------------------------------------------------------------------------
// 单例
// ---------------------------------------------------------------------------
ConfigManager& ConfigManager::instance() {
    static ConfigManager inst;
    return inst;
}

// ---------------------------------------------------------------------------
// 加载
// ---------------------------------------------------------------------------
bool ConfigManager::load(const std::string& yaml_path) {
    try {
        config_ = YAML::LoadFile(yaml_path);
    } catch (const YAML::Exception& e) {
        LOG_ERROR("加载 YAML 文件失败 '%s': %s",
                  yaml_path.c_str(), e.what());
        return false;
    }

    // 验证顶层节存在
    const char* sections[] = {
        "camera", "ui", "serial", "kalman", "recording",
        "roi", "mode0", "mode1", "mode2", "logging"
    };
    for (const auto* sec : sections) {
        if (!config_[sec]) {
            LOG_ERROR("缺少必需的配置节: %s", sec);
            return false;
        }
    }

    loaded_ = true;
    LOG_INFO("配置已从 %s 加载", yaml_path.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// 辅助函数 —— 解析 HSV 范围映射
// ---------------------------------------------------------------------------
ConfigManager::HSVRange ConfigManager::parseHSVRange(
    const YAML::Node& node) const {
    HSVRange r;
    r.h_min = getRequired<int>(node, "h_min");
    r.s_min = getRequired<int>(node, "s_min");
    r.v_min = getRequired<int>(node, "v_min");
    r.h_max = getRequired<int>(node, "h_max");
    r.s_max = getRequired<int>(node, "s_max");
    r.v_max = getRequired<int>(node, "v_max");
    return r;
}

// ---------------------------------------------------------------------------
// 摄像头
// ---------------------------------------------------------------------------
int ConfigManager::cameraIndex() const {
    return getRequired<int>(config_["camera"], "index");
}
std::string ConfigManager::cameraVideoPath() const {
    return getRequired<std::string>(config_["camera"], "video_path");
}
int ConfigManager::cameraWidth() const {
    return getRequired<int>(config_["camera"], "width");
}
int ConfigManager::cameraHeight() const {
    return getRequired<int>(config_["camera"], "height");
}
int ConfigManager::cameraFps() const {
    return getRequired<int>(config_["camera"], "fps");
}

// ---------------------------------------------------------------------------
// 用户界面
// ---------------------------------------------------------------------------
bool ConfigManager::imshowEnable() const {
    return getRequired<bool>(config_["ui"], "imshow_enable");
}
bool ConfigManager::uiDrawEnable() const {
    return getRequired<bool>(config_["ui"], "ui_draw_enable");
}
bool ConfigManager::showBinary() const {
    return getRequired<bool>(config_["ui"], "show_binary");
}
std::string ConfigManager::windowName() const {
    return getRequired<std::string>(config_["ui"], "window_name");
}

// ---------------------------------------------------------------------------
// 视频录制
// ---------------------------------------------------------------------------
bool ConfigManager::recordMode() const {
    return getRequired<bool>(config_["recording"], "record_mode");
}
std::string ConfigManager::recordOutputDir() const {
    return getRequired<std::string>(config_["recording"], "output_dir");
}
std::string ConfigManager::recordFourcc() const {
    return getRequired<std::string>(config_["recording"], "fourcc");
}
double ConfigManager::recordFps() const {
    return getRequired<double>(config_["recording"], "fps");
}

// ---------------------------------------------------------------------------
// 串口
// ---------------------------------------------------------------------------
std::string ConfigManager::serialPort() const {
    return getRequired<std::string>(config_["serial"], "port");
}
int ConfigManager::serialBaudrate() const {
    return getRequired<int>(config_["serial"], "baudrate");
}
int ConfigManager::serialSendIntervalMs() const {
    return getRequired<int>(config_["serial"], "send_interval_ms");
}

// ---------------------------------------------------------------------------
// ROI
// ---------------------------------------------------------------------------
int ConfigManager::roiX() const {
    return getRequired<int>(config_["roi"], "x");
}
int ConfigManager::roiY() const {
    return getRequired<int>(config_["roi"], "y");
}
int ConfigManager::roiWidth() const {
    return getRequired<int>(config_["roi"], "width");
}
int ConfigManager::roiHeight() const {
    return getRequired<int>(config_["roi"], "height");
}

// ---------------------------------------------------------------------------
// Kalman
// ---------------------------------------------------------------------------
float ConfigManager::kalmanProcessNoiseCov() const {
    return getRequired<float>(config_["kalman"], "process_noise_cov");
}
float ConfigManager::kalmanMeasurementNoiseCov() const {
    return getRequired<float>(config_["kalman"], "measurement_noise_cov");
}
int ConfigManager::kalmanInitFrames() const {
    return getRequired<int>(config_["kalman"], "init_frames");
}
int ConfigManager::kalmanLostFrameLimit() const {
    return getRequired<int>(config_["kalman"], "lost_frame_limit");
}
float ConfigManager::kalmanStableDistanceThreshold() const {
    return getRequired<float>(config_["kalman"], "stable_distance_threshold");
}

// ---------------------------------------------------------------------------
// 模式0
// ---------------------------------------------------------------------------
std::vector<ConfigManager::HSVRange> ConfigManager::mode0ColorHSV(
    const std::string& color_name) const {
    auto colors_node = config_["mode0"]["colors"];
    if (!colors_node[color_name]) {
        LOG_ERROR("配置中缺少 mode0.colors.%s", color_name.c_str());
        std::exit(EXIT_FAILURE);
    }
    auto color_node = colors_node[color_name];

    std::vector<HSVRange> ranges;

    // range1 始终必需
    if (!color_node["range1"]) {
        LOG_ERROR("配置中缺少 mode0.colors.%s.range1",
                  color_name.c_str());
        std::exit(EXIT_FAILURE);
    }
    ranges.push_back(parseHSVRange(color_node["range1"]));

    // range2 可选（仅红色使用）
    if (color_node["range2"]) {
        ranges.push_back(parseHSVRange(color_node["range2"]));
    }

    return ranges;
}
int ConfigManager::mode0MinContourArea() const {
    return getRequired<int>(config_["mode0"], "min_contour_area");
}
int ConfigManager::mode0MorphKernelSize() const {
    return getRequired<int>(config_["mode0"], "morph_kernel_size");
}

// ---------------------------------------------------------------------------
// 模式1
// ---------------------------------------------------------------------------
int ConfigManager::mode1GaussianKernelSize() const {
    return getRequired<int>(config_["mode1"], "gaussian_kernel_size");
}
int ConfigManager::mode1AdaptiveBlockSize() const {
    return getRequired<int>(config_["mode1"], "adaptive_block_size");
}
int ConfigManager::mode1AdaptiveC() const {
    return getRequired<int>(config_["mode1"], "adaptive_c");
}
int ConfigManager::mode1MinContourArea() const {
    return getRequired<int>(config_["mode1"], "min_contour_area");
}

bool ConfigManager::mode1EnableCollinearityCheck() const {
    return getRequired<bool>(config_["mode1"]["geometry_filter"],
                             "enable_collinearity_check");
}
double ConfigManager::mode1CollinearityTolerancePx() const {
    return getRequired<double>(config_["mode1"]["geometry_filter"],
                               "collinearity_tolerance_px");
}
bool ConfigManager::mode1EnableRadiusCheck() const {
    return getRequired<bool>(config_["mode1"]["geometry_filter"],
                             "enable_radius_check");
}
double ConfigManager::mode1RadiusTolerancePx() const {
    return getRequired<double>(config_["mode1"]["geometry_filter"],
                               "radius_tolerance_px");
}

// ---------------------------------------------------------------------------
// 模式2
// ---------------------------------------------------------------------------
ConfigManager::HSVRange ConfigManager::mode2BlackHSV() const {
    auto node = config_["mode2"];
    HSVRange r;
    r.h_min = getRequired<int>(node, "h_min");
    r.s_min = getRequired<int>(node, "s_min");
    r.v_min = getRequired<int>(node, "v_min");
    r.h_max = getRequired<int>(node, "h_max");
    r.s_max = getRequired<int>(node, "s_max");
    r.v_max = getRequired<int>(node, "v_max");
    return r;
}
double ConfigManager::mode2AspectRatioThreshold() const {
    return getRequired<double>(config_["mode2"], "aspect_ratio_threshold");
}
int ConfigManager::mode2MinArea() const {
    return getRequired<int>(config_["mode2"], "min_area");
}
int ConfigManager::mode2MorphKernelSize() const {
    return getRequired<int>(config_["mode2"], "morph_kernel_size");
}

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------
std::string ConfigManager::logLevel() const {
    return getRequired<std::string>(config_["logging"], "level");
}
bool ConfigManager::logToFile() const {
    return getRequired<bool>(config_["logging"], "log_to_file");
}
std::string ConfigManager::logFilePath() const {
    return getRequired<std::string>(config_["logging"], "log_file_path");
}
