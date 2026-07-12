/// @file yolo26.h
/// @brief 基于 NCNN 推理引擎的 YOLO26n 目标检测。
///
/// 专为 Raspberry Pi 5 ARM64 设计。
/// - 加载 model.ncnn.param + model.ncnn.bin
/// - 预处理: BGR→RGB, 缩放, 归一化 (1/255.0)
/// - 后处理: 解析输出张量, 解码边界框, NMS
///
/// 模型导出命令: yolo export model=best.pt format=ncnn imgsz=320

#pragma once

#include <opencv2/core.hpp>
#include <net.h>  // ncnn/net.h

#include <vector>
#include <string>
#include <chrono>
#include <atomic>

// ── 模型类型 ──────────────────────────────────────────────────────

/// 推理精度模式
enum class ModelType {
    FP16 = 0,   ///< FP16 半精度 — 高精度, 调试场景
    INT8 = 1,   ///< INT8 量化 — 高速度, 生产部署
};

/// 模型类型转字符串
inline const char* modelTypeStr(ModelType t) {
    return t == ModelType::INT8 ? "int8" : "fp16";
}

// ── 检测结果 ──────────────────────────────────────────────────────

/// 单个检测到的目标
struct Object {
    cv::Rect_<float> rect;   ///< 边界框 (x, y, width, height), 图像坐标
    int    label;             ///< 类别标签: 0=A类, 1=B类
    float  confidence;        ///< 检测置信度 [0.0, 1.0]

    // ── 派生坐标 ──────────────────────────────────────────────

    float x1() const { return rect.x; }
    float y1() const { return rect.y; }
    float x2() const { return rect.x + rect.width; }
    float y2() const { return rect.y + rect.height; }
    float cx() const { return rect.x + rect.width  * 0.5f; }
    float cy() const { return rect.y + rect.height * 0.5f; }
    float w()  const { return rect.width; }
    float h()  const { return rect.height; }

    /// 四角点: P1(左上), P2(右上), P3(右下), P4(左下)
    cv::Point2f p1() const { return cv::Point2f(x1(), y1()); }
    cv::Point2f p2() const { return cv::Point2f(x2(), y1()); }
    cv::Point2f p3() const { return cv::Point2f(x2(), y2()); }
    cv::Point2f p4() const { return cv::Point2f(x1(), y2()); }
};

// ── YOLO 检测器 ───────────────────────────────────────────────────

class YOLO26Detector {
public:
    /// 检测配置
    struct Config {
        // ── 模型路径（从 modelDir + modelType 自动推导） ──────
        std::string modelDir  = "model";           ///< 模型根目录
        ModelType   modelType = ModelType::FP16;   ///< 推理精度: FP16 或 INT8

        // ── 自动推导的完整路径（由 load() 填充） ──────────────
        std::string paramPath;   ///< 模型结构文件完整路径
        std::string binPath;     ///< 模型权重文件完整路径

        // ── 推理参数 ──────────────────────────────────────────
        int    inputWidth       = 320;       ///< 模型输入宽度
        int    inputHeight      = 320;       ///< 模型输入高度
        int    numClasses       = 2;         ///< 类别数量: A, B
        int    numThreads       = 4;         ///< ncnn 线程数
        float  confThreshold    = 0.25f;     ///< 置信度阈值
        float  nmsThreshold     = 0.45f;     ///< NMS IoU 阈值
        bool   useVulkan        = false;     ///< 启用 Vulkan 计算
        int    gpuDevice        = 0;         ///< Vulkan 设备索引
    };

    YOLO26Detector();
    ~YOLO26Detector();

    // ── 不可拷贝，可移动 ────────────────────────────────────────
    YOLO26Detector(const YOLO26Detector&) = delete;
    YOLO26Detector& operator=(const YOLO26Detector&) = delete;
    YOLO26Detector(YOLO26Detector&&) noexcept;
    YOLO26Detector& operator=(YOLO26Detector&&) noexcept;

    /// 加载模型并配置
    bool load(const Config& config);

    /// 检查模型是否已加载
    bool isLoaded() const { return loaded_; }

    /// 对 BGR 图像执行目标检测
    /// @param bgrImage  输入图像（任意尺寸，内部自动缩放）
    /// @param objects   输出的检测目标，包含图像坐标的边界框
    /// @return 是否成功
    bool detect(const cv::Mat& bgrImage, std::vector<Object>& objects);

    /// 获取平均推理时间（毫秒）
    float getAvgInferenceMs() const { return avgInferenceMs_.load(); }

    /// 获取模型输入尺寸
    int getInputWidth()  const { return config_.inputWidth; }
    int getInputHeight() const { return config_.inputHeight; }

    /// 获取配置
    const Config& getConfig() const { return config_; }

private:
    /// 预处理: 缩放, BGR→RGB, 归一化, 转换为 ncnn::Mat
    void preprocess(const cv::Mat& bgr, ncnn::Mat& input);

    /// 后处理: 解析输出张量, 解码, NMS
    void postprocess(const std::vector<ncnn::Mat>& outputs,
                     int imgWidth, int imgHeight,
                     std::vector<Object>& objects);

    /// 非极大值抑制（按类别分别处理）
    static void nms(std::vector<Object>& objects, float threshold);

    /// 将边界框裁剪到图像范围内
    static cv::Rect_<float> clipBox(const cv::Rect_<float>& box,
                                     int imgW, int imgH);

    ncnn::Net net_;
    Config   config_;
    bool     loaded_ = false;

    std::atomic<float> avgInferenceMs_{0.0f};
};
