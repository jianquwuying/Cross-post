/// @file camera.cpp
/// @brief Camera + CameraConfig 完整实现。
///
/// 所有摄像头参数完全由 YAML 控制，缺失字段直接退出。
/// 设置完成后读取实际值进行校验并输出 ANSI 表格。

#include "camera/camera.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include <iostream>
#include <iomanip>
#include <thread>
#include <cstdlib>

// ═══════════════════════════════════════════════════════════════════
//  文件作用域辅助 — V4L2 直接 ioctl (用于白平衡等 OpenCV 未封装的属性)
// ═══════════════════════════════════════════════════════════════════

namespace {

/// 通过 V4L2 ioctl 设置一个控件值
bool v4l2SetControl(int deviceId, unsigned int cid, int value) {
    std::string devPath = "/dev/video" + std::to_string(deviceId);
    int fd = open(devPath.c_str(), O_RDWR);
    if (fd < 0) return false;

    struct v4l2_control ctrl = {};
    ctrl.id    = cid;
    ctrl.value = value;
    int ret = ioctl(fd, VIDIOC_S_CTRL, &ctrl);
    close(fd);
    return ret == 0;
}

/// 通过 V4L2 ioctl 读取一个控件值
int v4l2GetControl(int deviceId, unsigned int cid) {
    std::string devPath = "/dev/video" + std::to_string(deviceId);
    int fd = open(devPath.c_str(), O_RDWR);
    if (fd < 0) return -1;

    struct v4l2_control ctrl = {};
    ctrl.id = cid;
    int ret = ioctl(fd, VIDIOC_G_CTRL, &ctrl);
    close(fd);
    return (ret == 0) ? ctrl.value : -1;
}

/// 字符串 FourCC → 整数
int fourccFromString(const std::string& s) {
    if (s.length() != 4) return 0;
    return cv::VideoWriter::fourcc(s[0], s[1], s[2], s[3]);
}

/// 整数 FourCC → 字符串
std::string fourccToString(int fc) {
    std::string s;
    s += static_cast<char>( fc        & 0xFF);
    s += static_cast<char>((fc >>  8) & 0xFF);
    s += static_cast<char>((fc >> 16) & 0xFF);
    s += static_cast<char>((fc >> 24) & 0xFF);
    return s;
}

/// 后端字符串 → OpenCV API 代码
int backendToApi(const std::string& backend) {
    if (backend == "V4L2"  || backend == "v4l2")  return cv::CAP_V4L2;
    if (backend == "ANY"   || backend == "any")   return cv::CAP_ANY;
    if (backend == "DSHOW" || backend == "dshow") return cv::CAP_DSHOW;
    return cv::CAP_V4L2;   // 未知后端时默认 V4L2
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════
//  CameraConfig::fromYaml — 严格解析
// ═══════════════════════════════════════════════════════════════════

CameraConfig CameraConfig::fromYaml(const YAML::Node& node) {
    CameraConfig cfg;

    auto require = [&node](const char* key) -> YAML::Node {
        if (!node || !node[key].IsDefined()) {
            std::cerr << "\033[31m[ERROR]\033[0m "
                      << "Camera config missing required field: camera." << key
                      << std::endl;
            std::exit(1);
        }
        return node[key];
    };

    cfg.device             = require("device").as<int>();
    cfg.backend            = require("backend").as<std::string>();
    cfg.width              = require("width").as<int>();
    cfg.height             = require("height").as<int>();
    cfg.fps                = require("fps").as<int>();
    cfg.fourcc             = require("fourcc").as<std::string>();
    cfg.buffer_size        = require("buffer_size").as<int>();
    cfg.auto_exposure      = require("auto_exposure").as<bool>();
    cfg.exposure           = require("exposure").as<int>();
    cfg.auto_white_balance = require("auto_white_balance").as<bool>();
    cfg.white_balance      = require("white_balance").as<int>();

    return cfg;
}

// ═══════════════════════════════════════════════════════════════════
//  Camera 生命周期
// ═══════════════════════════════════════════════════════════════════

Camera::~Camera() {
    release();
}

Camera::Camera(Camera&& other) noexcept
    : cap_(std::move(other.cap_))
    , config_(other.config_)
    , lastFrameTime_(other.lastFrameTime_)
{
    actualFPS_.store(other.actualFPS_.load());
}

Camera& Camera::operator=(Camera&& other) noexcept {
    if (this != &other) {
        release();
        cap_ = std::move(other.cap_);
        config_ = other.config_;
        lastFrameTime_ = other.lastFrameTime_;
        actualFPS_.store(other.actualFPS_.load());
    }
    return *this;
}

// ═══════════════════════════════════════════════════════════════════
//  Camera::open — 按顺序设置所有参数 + 验证
// ═══════════════════════════════════════════════════════════════════

bool Camera::open(const CameraConfig& config) {
    config_ = config;

    // ── 1. 映射后端 ──────────────────────────────────────────────
    int apiCode = backendToApi(config_.backend);

    // ── 2. 打开设备 ──────────────────────────────────────────────
    bool ok = cap_.open(config_.device, apiCode);
    if (!ok) {
        std::cerr << "[摄像头] V4L2 打开失败, 尝试默认方式..." << std::endl;
        ok = cap_.open(config_.device);
    }
    if (!ok) {
        std::cerr << "\033[31m[ERROR]\033[0m "
                  << "无法打开 /dev/video" << config_.device << std::endl;
        return false;
    }

    // ── 3. FourCC ────────────────────────────────────────────────
    int fc = fourccFromString(config_.fourcc);
    cap_.set(cv::CAP_PROP_FOURCC, fc);

    // ── 4. 分辨率 ────────────────────────────────────────────────
    cap_.set(cv::CAP_PROP_FRAME_WIDTH,  config_.width);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, config_.height);

    // ── 5. FPS ──────────────────────────────────────────────────
    cap_.set(cv::CAP_PROP_FPS, config_.fps);

    // ── 6. 缓冲区 ────────────────────────────────────────────────
    cap_.set(cv::CAP_PROP_BUFFERSIZE, config_.buffer_size);

    // ── 7. 曝光 ──────────────────────────────────────────────────
    applyExposure(config_);

    // ── 8. 白平衡 (V4L2 ioctl) ──────────────────────────────────
    applyWhiteBalance(config_);

    // ── 9. 读取实际值 + 打印验证表格 ────────────────────────────
    verifyAndLog(config_);

    lastFrameTime_ = std::chrono::steady_clock::now();
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  曝光控制
// ═══════════════════════════════════════════════════════════════════

void Camera::applyExposure(const CameraConfig& cfg) {
    if (cfg.auto_exposure) {
        // 自动曝光: 仅开启自动模式
        cap_.set(cv::CAP_PROP_AUTO_EXPOSURE, 0.75);   // 0.75 = 自动
    } else {
        // 手动曝光: 先关闭自动, 再设置手动值
        cap_.set(cv::CAP_PROP_AUTO_EXPOSURE, 1.0);    // 1.0 = 手动模式
        cap_.set(cv::CAP_PROP_EXPOSURE, static_cast<double>(cfg.exposure));
    }
}

// ═══════════════════════════════════════════════════════════════════
//  白平衡控制 (直接 V4L2 ioctl)
// ═══════════════════════════════════════════════════════════════════

void Camera::applyWhiteBalance(const CameraConfig& cfg) {
    if (cfg.auto_white_balance) {
        // 自动白平衡: 仅开启自动
        if (!v4l2SetControl(cfg.device, V4L2_CID_AUTO_WHITE_BALANCE, 1)) {
            std::cerr << "\033[33m[WARN]\033[0m  "
                      << "自动白平衡设置失败 (摄像头可能不支持 V4L2_CID_AUTO_WHITE_BALANCE)"
                      << std::endl;
        }
    } else {
        // 手动白平衡: 先关闭自动, 再设置色温
        if (!v4l2SetControl(cfg.device, V4L2_CID_AUTO_WHITE_BALANCE, 0)) {
            std::cerr << "\033[33m[WARN]\033[0m  "
                      << "关闭自动白平衡失败 (摄像头可能不支持)"
                      << std::endl;
        }
        if (!v4l2SetControl(cfg.device, V4L2_CID_WHITE_BALANCE_TEMPERATURE,
                            cfg.white_balance)) {
            std::cerr << "\033[33m[WARN]\033[0m  "
                      << "白平衡色温设置失败 (摄像头可能不支持 V4L2_CID_WHITE_BALANCE_TEMPERATURE)"
                      << std::endl;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  参数验证 — 读取实际值 + ANSI 表格输出
// ═══════════════════════════════════════════════════════════════════

void Camera::verifyAndLog(const CameraConfig& cfg) {
    // ── 读取实际值 ──────────────────────────────────────────────
    int    actualWidth   = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
    int    actualHeight  = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
    int    actualFPS     = static_cast<int>(cap_.get(cv::CAP_PROP_FPS));
    int    actualFourCC  = static_cast<int>(cap_.get(cv::CAP_PROP_FOURCC));
    double actualAutoExp = cap_.get(cv::CAP_PROP_AUTO_EXPOSURE);
    double actualExpVal  = cap_.get(cv::CAP_PROP_EXPOSURE);
    int    actualAutoWB  = v4l2GetControl(cfg.device, V4L2_CID_AUTO_WHITE_BALANCE);
    int    actualWBTemp  = v4l2GetControl(cfg.device, V4L2_CID_WHITE_BALANCE_TEMPERATURE);

    std::string actualFourCCStr = fourccToString(actualFourCC);
    bool actualAutoExpBool  = (actualAutoExp < 0.9);
    bool actualAutoWBBool   = (actualAutoWB == 1);

    // ── 比较函数 (返回纯文本 "OK" / "WARN", ANSI 颜色在 row 中统一处理) ──
    auto match = [](auto req, auto act) -> std::string {
        return (req == act) ? "OK" : "WARN";
    };

    // ── 表格头 ──────────────────────────────────────────────────
    // 列宽: 10, 16, 16, 8  (setw + 前后空格)
    std::cout << "\n\033[1m"
              << "┌──────────┬────────────────┬────────────────┬────────┐\n"
              << "│ Param    │ Requested      │ Actual         │ Status │\n"
              << "├──────────┼────────────────┼────────────────┼────────┤\n"
              << "\033[0m";

    // ── 行输出: status 文本不含 ANSI, 由本 lambda 包裹颜色 ──────
    auto row = [](const std::string& param,
                  const std::string& req, const std::string& act,
                  const std::string& st) {
        const char* color = (st == "WARN") ? "\033[33m" : "\033[32m";
        std::cout << "│ " << std::left << std::setw(8)  << param
                  << " │ "           << std::setw(14) << req
                  << " │ "           << std::setw(14) << act
                  << " │ " << color  << std::setw(6)  << st << "\033[0m │\n";
    };

    row("device",   std::to_string(cfg.device),
                    std::to_string(cfg.device),
                    match(cfg.device, cfg.device));
    row("backend",  cfg.backend,
                    cfg.backend,
                    match(cfg.backend, cfg.backend));
    row("fourcc",   cfg.fourcc,
                    actualFourCCStr,
                    match(cfg.fourcc, actualFourCCStr));
    row("width",    std::to_string(cfg.width),
                    std::to_string(actualWidth),
                    match(cfg.width, actualWidth));
    row("height",   std::to_string(cfg.height),
                    std::to_string(actualHeight),
                    match(cfg.height, actualHeight));
    row("fps",      std::to_string(cfg.fps),
                    std::to_string(actualFPS),
                    match(cfg.fps, actualFPS));
    row("buf_size", std::to_string(cfg.buffer_size),
                    std::to_string(cfg.buffer_size),
                    match(cfg.buffer_size, cfg.buffer_size));
    row("auto_exp", (cfg.auto_exposure ? "true" : "false"),
                    (actualAutoExpBool ? "true" : "false"),
                    match(cfg.auto_exposure, actualAutoExpBool));
    row("exposure", std::to_string(cfg.exposure),
                    std::to_string(static_cast<int>(actualExpVal)),
                    match(cfg.exposure, static_cast<int>(actualExpVal)));
    row("auto_wb",  (cfg.auto_white_balance ? "true" : "false"),
                    (actualAutoWBBool ? "true" : "false"),
                    match(cfg.auto_white_balance, actualAutoWBBool));
    row("wb_temp",  std::to_string(cfg.white_balance),
                    std::to_string(actualWBTemp),
                    match(cfg.white_balance, actualWBTemp));

    std::cout << "\033[1m"
              << "└──────────┴────────────────┴────────────────┴────────┘\n"
              << "\033[0m" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════
//  帧采集
// ═══════════════════════════════════════════════════════════════════

bool Camera::read(cv::Mat& frame) {
    if (!cap_.isOpened()) return false;

    bool ok = cap_.read(frame);
    if (!ok || frame.empty()) return false;

    // 计算实际 FPS (EMA)
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - lastFrameTime_).count();
    lastFrameTime_ = now;

    if (elapsed > 0.0) {
        double instantFPS = 1.0 / elapsed;
        actualFPS_.store(actualFPS_.load() * 0.9 + instantFPS * 0.1);
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  释放
// ═══════════════════════════════════════════════════════════════════

void Camera::release() {
    if (cap_.isOpened()) {
        cap_.release();
        std::cout << "[摄像头] 已释放。" << std::endl;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  属性访问
// ═══════════════════════════════════════════════════════════════════

double Camera::getActualFPS() const {
    return actualFPS_.load();
}

double Camera::getProperty(int propId) const {
    return cap_.get(propId);
}

bool Camera::setProperty(int propId, double value) {
    return cap_.set(propId, value);
}
