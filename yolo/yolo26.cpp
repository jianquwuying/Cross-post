#include "yolo/yolo26.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>

// ── YOLO26Detector ────────────────────────────────────────────────

YOLO26Detector::YOLO26Detector()
    : net_(std::make_unique<ncnn::Net>()) {}

YOLO26Detector::~YOLO26Detector() = default;

YOLO26Detector::YOLO26Detector(YOLO26Detector&& other) noexcept
    : net_(std::move(other.net_))
    , config_(std::move(other.config_))
    , loaded_(other.loaded_)
{
    avgInferenceMs_.store(other.avgInferenceMs_.load());
    other.loaded_ = false;
}

YOLO26Detector& YOLO26Detector::operator=(YOLO26Detector&& other) noexcept {
    if (this != &other) {
        net_    = std::move(other.net_);
        config_ = std::move(other.config_);
        loaded_ = other.loaded_;
        avgInferenceMs_.store(other.avgInferenceMs_.load());
        other.loaded_ = false;
    }
    return *this;
}

bool YOLO26Detector::load(const Config& config) {
    config_ = config;

    // 移动后 re-initialize
    if (!net_) net_ = std::make_unique<ncnn::Net>();

    // ── 推导模型路径 ───────────────────────────────────────────
    std::string subdir = (config_.modelType == ModelType::INT8) ? "int8" : "fp16";
    config_.paramPath = config_.modelDir + "/" + subdir + "/model.ncnn.param";
    config_.binPath   = config_.modelDir + "/" + subdir + "/model.ncnn.bin";

    // ── 配置 NCNN 选项 ─────────────────────────────────────────
    // 默认使用 CPU + NEON + OpenMP, 不启用 Vulkan
    net_->opt.use_vulkan_compute = false;

    // CPU 线程数 (OpenMP 多线程)
    net_->opt.num_threads = config_.numThreads;

    // 根据模型类型设置不同的优化标志
    if (config_.modelType == ModelType::INT8) {
        // INT8 量化模型 — 启用 INT8 推理
        net_->opt.use_int8_inference  = true;
        net_->opt.use_fp16_packed    = false;
        net_->opt.use_fp16_storage   = false;
        net_->opt.use_fp16_arithmetic = false;
        std::cout << "[YOLO] INT8 推理模式" << std::endl;
    } else {
        // FP16 模型 — ARM NEON FP16 加速
        net_->opt.use_int8_inference  = false;
        net_->opt.use_fp16_packed    = true;   // ARM 上更快
        net_->opt.use_fp16_storage   = true;   // 节省内存
        net_->opt.use_fp16_arithmetic = false;  // 中间层保持 FP32 精度
        std::cout << "[YOLO] FP16 推理模式" << std::endl;
    }

    // 通用优化
    net_->opt.use_packing_layout = true;   // 优化内存布局
    net_->opt.lightmode          = true;   // 使用后立即释放中间 blob

    std::cout << "[YOLO] 加载模型: " << config_.paramPath << std::endl;
    std::cout << "[YOLO]   类型=" << modelTypeStr(config_.modelType)
              << " 线程数=" << config_.numThreads
              << " 后端=CPU+NEON+OpenMP"
              << std::endl;

    // ── 加载模型文件 ───────────────────────────────────────────
    int ret = net_->load_param(config_.paramPath.c_str());
    if (ret != 0) {
        std::cerr << "[YOLO] 错误: 无法加载 param 文件: "
                  << config_.paramPath << " (错误码=" << ret << ")" << std::endl;
        return false;
    }

    ret = net_->load_model(config_.binPath.c_str());
    if (ret != 0) {
        std::cerr << "[YOLO] 错误: 无法加载 bin 文件: "
                  << config_.binPath << " (错误码=" << ret << ")" << std::endl;
        return false;
    }

    // INT8 模型: 尝试加载校准表
    if (config_.modelType == ModelType::INT8) {
        std::string tablePath = config_.modelDir + "/int8/calibration.table";
        // ncnn INT8 模型将校准信息嵌入 param/bin, 无需单独加载
        std::cout << "[YOLO] INT8 校准数据已嵌入模型" << std::endl;
    }

    loaded_ = true;
    std::cout << "[YOLO] 模型加载成功。" << std::endl;
    std::cout << "[YOLO]   输入尺寸: " << config_.inputWidth
              << "x" << config_.inputHeight << std::endl;
    std::cout << "[YOLO]   类别数: " << config_.numClasses << std::endl;

    return true;
}

// ── 预处理 ────────────────────────────────────────────────────────

void YOLO26Detector::preprocess(const cv::Mat& bgr, ncnn::Mat& input) {
    // 缩放到模型输入尺寸
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(config_.inputWidth, config_.inputHeight),
               0, 0, cv::INTER_LINEAR);

    // 转换 BGR → RGB 并归一化到 [0, 1]
    // ncnn::Mat::from_pixels_resize 可在一步内完成 BGR→RGB 和缩放,
    // 但我们已经缩放过了, 所以对缩放后的图像使用 from_pixels。
    //
    // 像素格式: RGB (3 通道), 类型 float32, 范围 [0, 1]
    input = ncnn::Mat::from_pixels(
        resized.data,
        ncnn::Mat::PIXEL_BGR2RGB,
        resized.cols, resized.rows
    );

    // 归一化: 除以 255.0
    const float norm = 1.0f / 255.0f;
    input.substract_mean_normalize(nullptr, &norm);
}

// ── 检测管道 ──────────────────────────────────────────────────────

bool YOLO26Detector::detect(const cv::Mat& bgrImage,
                             std::vector<Object>& objects) {
    objects.clear();

    if (!loaded_ || bgrImage.empty()) return false;

    auto t0 = std::chrono::steady_clock::now();

    // ── 步骤1: 预处理 ─────────────────────────────────────────
    ncnn::Mat input;
    preprocess(bgrImage, input);

    // ── 步骤2: 创建提取器 ─────────────────────────────────────
    ncnn::Extractor ex = net_->create_extractor();

    // 每个提取器可独立启用轻量模式
    ex.set_light_mode(true);

    // 输入 blob 名称: 一般为 "in0" / "input" / "images"
    // 大多数 ultralytics 导出使用 "in0"
    ex.input("in0", input);

    // ── 步骤3: 前向推理 ───────────────────────────────────────
    std::vector<ncnn::Mat> outputs;

    // 尝试提取所有输出 blob
    // ultralytics NCNN 导出的常见输出 blob 名称:
    //   "out0" — 单输出（后处理或原始）
    //   "output0", "output1", "output2" — 多尺度原始输出
    //
    // 尝试常见的命名模式:
    for (int i = 0; i < 5; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "out%d", i);
        ncnn::Mat out;
        int ret = ex.extract(name, out);
        if (ret == 0 && !out.empty()) {
            outputs.push_back(out.clone());
        } else {
            break; // 假设为顺序命名
        }
    }

    // 回退: 尝试 "output"（单个 blob 名称）
    if (outputs.empty()) {
        ncnn::Mat out;
        int ret = ex.extract("output", out);
        if (ret == 0 && !out.empty()) {
            outputs.push_back(out.clone());
        }
    }

    // 回退: 尝试 "output0", "output1", ...
    if (outputs.empty()) {
        for (int i = 0; i < 3; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "output%d", i);
            ncnn::Mat out;
            int ret = ex.extract(name, out);
            if (ret == 0 && !out.empty()) {
                outputs.push_back(out.clone());
            }
        }
    }

    if (outputs.empty()) {
        std::cerr << "[YOLO] 错误: 模型未产生任何输出 blob。"
                  << "请检查模型导出格式。" << std::endl;
        return false;
    }

    // ── 步骤4: 后处理 ─────────────────────────────────────────
    postprocess(outputs, bgrImage.cols, bgrImage.rows, objects);

    // ── 计时 ──────────────────────────────────────────────────
    auto t1 = std::chrono::steady_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

    // 指数移动平均
    avgInferenceMs_.store(avgInferenceMs_.load() * 0.9f + ms * 0.1f);

    return true;
}

// ── 后处理 ────────────────────────────────────────────────────────

void YOLO26Detector::postprocess(
    const std::vector<ncnn::Mat>& outputs,
    int imgWidth, int imgHeight,
    std::vector<Object>& objects)
{
    const float confThresh = config_.confThreshold;
    const float nmsThresh  = config_.nmsThreshold;
    const int   inputW     = config_.inputWidth;
    const int   inputH     = config_.inputHeight;
    const int   nc         = config_.numClasses;

    // 缩放因子: 将模型坐标映射回原始图像坐标
    const float scaleW = static_cast<float>(imgWidth)  / static_cast<float>(inputW);
    const float scaleH = static_cast<float>(imgHeight) / static_cast<float>(inputH);

    std::vector<Object> proposals;

    for (const auto& out : outputs) {
        const int dims = out.dims;

        // ── 情况1: 后处理输出 ─────────────────────────────────
        // 形状: (N, 6), 每行为 [label, score, x1, y1, x2, y2]
        // 这是 ultralytics YoloV8DetectionOutput 层的输出格式。
        if (dims == 2 && out.w == 6) {
            const int numDet = out.h;
            for (int i = 0; i < numDet; ++i) {
                const float* row = out.row(i);

                int   label = static_cast<int>(row[0]);
                float score = row[1];
                float x1    = row[2];
                float y1    = row[3];
                float x2    = row[4];
                float y2    = row[5];

                if (score < confThresh) continue;
                if (label < 0 || label >= nc) continue;

                // 坐标已归一化到 [0, 1]
                Object obj;
                obj.label      = label;
                obj.confidence = score;

                // 缩放到原始图像坐标
                obj.rect.x      = x1 * scaleW * static_cast<float>(inputW);
                obj.rect.y      = y1 * scaleH * static_cast<float>(inputH);
                obj.rect.width  = (x2 - x1) * scaleW * static_cast<float>(inputW);
                obj.rect.height = (y2 - y1) * scaleH * static_cast<float>(inputH);

                obj.rect = clipBox(obj.rect, imgWidth, imgHeight);
                proposals.push_back(obj);
            }
        }
        // ── 情况2: 转置原始输出 ───────────────────────────────
        // 形状: (1, nc+4, num_proposals) — YOLOv8 原始导出的典型格式
        // nc+4 = [cx, cy, w, h, ...class_scores]
        else if (dims == 3 && out.c >= (nc + 4)) {
            const int numProps = out.h;

            for (int i = 0; i < numProps; ++i) {
                const float* row = out.channel(0).row(i);

                float cx = row[0];
                float cy = row[1];
                float w  = row[2];
                float h  = row[3];

                // 找到最大类别得分
                int   bestClass = -1;
                float bestScore = -1.0f;
                for (int c = 0; c < nc; ++c) {
                    float score = row[4 + c];
                    if (score > bestScore) {
                        bestScore = score;
                        bestClass = c;
                    }
                }

                if (bestScore < confThresh) continue;

                // 中心格式转角落格式
                // 坐标已归一化到 [0, 1]
                float x1 = (cx - w * 0.5f);
                float y1 = (cy - h * 0.5f);
                float x2 = (cx + w * 0.5f);
                float y2 = (cy + h * 0.5f);

                Object obj;
                obj.label      = bestClass;
                obj.confidence = bestScore;

                // 缩放到原始图像坐标
                obj.rect.x      = x1 * scaleW * static_cast<float>(inputW);
                obj.rect.y      = y1 * scaleH * static_cast<float>(inputH);
                obj.rect.width  = (x2 - x1) * scaleW * static_cast<float>(inputW);
                obj.rect.height = (y2 - y1) * scaleH * static_cast<float>(inputH);

                obj.rect = clipBox(obj.rect, imgWidth, imgHeight);

                // 过滤极小框（大概率是噪声）
                if (obj.rect.width >= 2.0f && obj.rect.height >= 2.0f) {
                    proposals.push_back(obj);
                }
            }
        }
        // ── 情况3: 多尺度输出 ─────────────────────────────────
        // 形状: (1, nc+4, H, W) — 基于网格, 需要不同的解码方式
        // 通用处理: 当作3D张量解码
        else if (dims == 3 && out.c >= (nc + 4)) {
            const int gridH = out.h;
            const int gridW = out.w;
            const int strideVal = inputW / gridW;  // 近似步长

            for (int gy = 0; gy < gridH; ++gy) {
                for (int gx = 0; gx < gridW; ++gx) {
                    const float* ptr = out.channel(0).row(gy) + gx * out.c;

                    // 找到最佳类别
                    int   bestClass = -1;
                    float bestScore = -1.0f;
                    for (int c = 0; c < nc; ++c) {
                        float score = ptr[4 + c];
                        if (score > bestScore) {
                            bestScore = score;
                            bestClass = c;
                        }
                    }

                    if (bestScore < confThresh) continue;

                    float cx = (ptr[0] + static_cast<float>(gx)) * static_cast<float>(strideVal) / static_cast<float>(inputW);
                    float cy = (ptr[1] + static_cast<float>(gy)) * static_cast<float>(strideVal) / static_cast<float>(inputH);
                    float w  = std::exp(ptr[2]) * static_cast<float>(strideVal) / static_cast<float>(inputW);
                    float h  = std::exp(ptr[3]) * static_cast<float>(strideVal) / static_cast<float>(inputH);

                    float x1 = (cx - w * 0.5f);
                    float y1 = (cy - h * 0.5f);
                    float x2 = (cx + w * 0.5f);
                    float y2 = (cy + h * 0.5f);

                    Object obj;
                    obj.label      = bestClass;
                    obj.confidence = bestScore;

                    obj.rect.x      = x1 * static_cast<float>(imgWidth);
                    obj.rect.y      = y1 * static_cast<float>(imgHeight);
                    obj.rect.width  = (x2 - x1) * static_cast<float>(imgWidth);
                    obj.rect.height = (y2 - y1) * static_cast<float>(imgHeight);

                    obj.rect = clipBox(obj.rect, imgWidth, imgHeight);

                    if (obj.rect.width >= 2.0f && obj.rect.height >= 2.0f) {
                        proposals.push_back(obj);
                    }
                }
            }
        }
    }

    // ── 应用 NMS ───────────────────────────────────────────────
    nms(proposals, nmsThresh);

    objects = std::move(proposals);
}

// ── 非极大值抑制 (NMS) ─────────────────────────────────────────────

void YOLO26Detector::nms(std::vector<Object>& objects, float threshold) {
    // 按置信度降序排列
    std::sort(objects.begin(), objects.end(),
              [](const Object& a, const Object& b) {
                  return a.confidence > b.confidence;
              });

    std::vector<Object> kept;
    kept.reserve(objects.size());

    for (size_t i = 0; i < objects.size(); ++i) {
        bool suppress = false;

        for (size_t j = 0; j < kept.size(); ++j) {
            // 仅抑制同类别的目标
            if (objects[i].label != kept[j].label) continue;

            // 计算 IoU
            float interX1 = std::max(objects[i].x1(), kept[j].x1());
            float interY1 = std::max(objects[i].y1(), kept[j].y1());
            float interX2 = std::min(objects[i].x2(), kept[j].x2());
            float interY2 = std::min(objects[i].y2(), kept[j].y2());

            if (interX1 >= interX2 || interY1 >= interY2) continue;

            float interArea = (interX2 - interX1) * (interY2 - interY1);
            float areaI = objects[i].w() * objects[i].h();
            float areaJ = kept[j].w() * kept[j].h();
            float unionArea = areaI + areaJ - interArea;

            if (unionArea <= 0.0f) continue;

            float iou = interArea / unionArea;

            if (iou >= threshold) {
                suppress = true;
                break;
            }
        }

        if (!suppress) {
            kept.push_back(objects[i]);
        }
    }

    objects = std::move(kept);
}

// ── 边界框裁剪 ────────────────────────────────────────────────────

cv::Rect_<float> YOLO26Detector::clipBox(const cv::Rect_<float>& box,
                                          int imgW, int imgH) {
    cv::Rect_<float> clipped = box;

    if (clipped.x < 0) { clipped.width += clipped.x; clipped.x = 0; }
    if (clipped.y < 0) { clipped.height += clipped.y; clipped.y = 0; }
    if (clipped.x + clipped.width  > static_cast<float>(imgW))
        clipped.width  = static_cast<float>(imgW) - clipped.x;
    if (clipped.y + clipped.height > static_cast<float>(imgH))
        clipped.height = static_cast<float>(imgH) - clipped.y;
    if (clipped.width  < 0) clipped.width  = 0;
    if (clipped.height < 0) clipped.height = 0;

    return clipped;
}
