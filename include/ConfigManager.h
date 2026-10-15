#pragma once
// ============================================================================
// ConfigManager.h  —  YAML 配置单例
// ============================================================================
// 启动时加载 config/vision.yaml 一次。
// 每个访问器验证键是否存在 —— 缺少 → LOG_ERROR + exit。
// 代码中**没有任何**硬编码默认值。
// ============================================================================

#include "Logger.h"

#include <cstdint>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

class ConfigManager {
public:
    // -------------------------------------------------------------------
    // 单例
    // -------------------------------------------------------------------
    static ConfigManager& instance();

    /// 加载并解析 YAML 文件。必须在任何访问器之前调用。
    /// 文件读取/解析失败返回 false（已记录日志）。
    bool load(const std::string& yaml_path);

    /// load() 成功后返回 true。
    bool isLoaded() const { return loaded_; }

    // -------------------------------------------------------------------
    // 摄像头
    // -------------------------------------------------------------------
    int         cameraIndex()     const;
    std::string cameraVideoPath() const;
    int         cameraWidth()     const;
    int         cameraHeight()    const;
    int         cameraFps()       const;

    // -------------------------------------------------------------------
    // 用户界面
    // -------------------------------------------------------------------
    bool        imshowEnable()  const;
    bool        uiDrawEnable()  const;
    bool        showBinary()    const;
    std::string windowName()    const;

    // -------------------------------------------------------------------
    // 视频录制
    // -------------------------------------------------------------------
    bool        recordMode()      const;
    std::string recordOutputDir() const;
    std::string recordFourcc()    const;
    double      recordFps()       const;

    // -------------------------------------------------------------------
    // 串口
    // -------------------------------------------------------------------
    std::string serialPort()              const;
    int         serialBaudrate()          const;
    int         serialSendIntervalMs()    const;

    /// 无目标帧开关（`serial.enable_no_target_frame`）。
    /// true  = 无有效目标时发送 TargetType=0 无目标帧；Kalman 未超时期间
    ///         发送预测坐标（target_type 保持 1/2/3）
    /// false = 完全回退：无目标不发送，也未发预测坐标
    bool        serialEnableNoTargetFrame() const;

    // -------------------------------------------------------------------
    // ROI 感兴趣区域
    // -------------------------------------------------------------------
    int roiX()      const;
    int roiY()      const;
    int roiWidth()  const;
    int roiHeight() const;

    // -------------------------------------------------------------------
    // Kalman 滤波器（标量 —— 调用方负责转换为 cv::Mat::eye * scalar）
    // -------------------------------------------------------------------
    float kalmanProcessNoiseCov()        const;
    float kalmanMeasurementNoiseCov()    const;
    int   kalmanInitFrames()             const;
    int   kalmanLostFrameLimit()         const;
    float kalmanStableDistanceThreshold() const;

    // -------------------------------------------------------------------
    // 模式0  —  颜色物块 HSV 范围
    // -------------------------------------------------------------------
    struct HSVRange {
        int h_min, s_min, v_min;
        int h_max, s_max, v_max;
    };

    /// 大多数颜色返回 1 个范围，红色返回 2 个范围（双 HSV 区间）。
    std::vector<HSVRange> mode0ColorHSV(const std::string& color_name) const;
    int mode0MinContourArea()   const;
    int mode0MorphKernelSize()  const;

    // -------------------------------------------------------------------
    // 模式1  —  单圆环数字识别（Hu矩 + cv::matchShapes 模板匹配）
    // -------------------------------------------------------------------
    int mode1GaussianKernelSize()  const;
    int mode1AdaptiveBlockSize()   const;
    int mode1AdaptiveC()           const;
    int mode1MinContourArea()      const;
    int mode1MorphOpenKernelSize() const;

    double mode1DigitRoiScale()        const;   // 数字 ROI 半宽 / 圆环半径
    double mode1DigitOpenRatio()       const;   // 数字区剥环开运算核 / 圆环半径
    double mode1DigitAdaptiveBlockRatio() const; // 数字二值化邻域 / 数字ROI短边
    bool   mode1InvertDigit()          const;   // 数字二值化是否反色
    int    mode1DigitMatchSize()       const;   // 数字匹配归一化位图边长（像素）
    int    mode1DigitMatchAngleRange() const;   // 多角度模板搜索范围（±度）
    int    mode1DigitMatchAngleStep()  const;   // 多角度模板搜索步长（度）
    double mode1DigitMinMargin()       const;   // 判号置信度门槛（最高IoU-次高IoU）
    int    mode1MaxRingsPerFrame()     const;   // 每帧最多做数字识别的圆环数
    bool   mode1EnableShapeFilter()    const;   // 形状过滤开关
    double mode1MinCircularity()       const;   // 圆度下限 4π·面积/周长²
    double mode1MaxAspectRatio()       const;   // 长宽比上限（minAreaRect 长边/短边）
    double mode1MinFillRatio()         const;   // 填充率下限 面积/(π·外接圆半径²)
    std::string mode1TemplatePath(int digit_id) const;  // digit_id ∈ {1,2,3}

    // -------------------------------------------------------------------
    // 模式2  —  黑色圆柱检测
    // -------------------------------------------------------------------
    HSVRange mode2BlackHSV()            const;
    double   mode2AspectRatioThreshold() const;
    int      mode2MinArea()             const;
    int      mode2MorphKernelSize()     const;

    // -------------------------------------------------------------------
    // 日志
    // -------------------------------------------------------------------
    std::string logLevel()     const;
    bool        logToFile()    const;
    std::string logFilePath()  const;

private:
    ConfigManager()  = default;
    ~ConfigManager() = default;

    ConfigManager(const ConfigManager&)            = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    // 辅助函数：从 YAML 节点读取必需的标量值。失败则退出。
    template <typename T>
    T getRequired(const YAML::Node& parent,
                  const std::string& key) const;

    // 辅助函数：解析 {h_min, s_min, v_min, h_max, s_max, v_max} 映射。
    HSVRange parseHSVRange(const YAML::Node& node) const;

    YAML::Node config_;
    bool loaded_ = false;
};

// ========================================================================
// 模板实现（必须在头文件中）
// ========================================================================
template <typename T>
T ConfigManager::getRequired(const YAML::Node& parent,
                             const std::string& key) const {
    if (!parent[key]) {
        LOG_ERROR("缺少必需的配置键: %s", key.c_str());
        std::exit(EXIT_FAILURE);
    }
    try {
        return parent[key].as<T>();
    } catch (const YAML::Exception& e) {
        LOG_ERROR("配置键 '%s' 类型不匹配: %s", key.c_str(), e.what());
        std::exit(EXIT_FAILURE);
    }
}
