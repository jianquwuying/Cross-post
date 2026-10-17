# ============================================================================
# 26年第十二届工程实践与创新大赛（参赛作品）
# 树莓派 5 视觉终端 —— visionpost
# C++17 + OpenCV + yaml-cpp；目标平台 Ubuntu 24.04 aarch64（Cortex-A76）
# ============================================================================
cmake_minimum_required(VERSION 3.28)
project(RaspberryPi5_Vision_System VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 强制使用 C++17（不使用 GNU 扩展）
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# ARM Cortex-A76 专用优化（仅适用于树莓派 5；其他架构需去掉这两个选项）
#   armv8.2-a  启用 ARMv8.2-A 指令集与 NEON / asimd（GCC 自动向量化）
#   mtune=cortex-a76  针对 A76 流水线调度指令
# ---------------------------------------------------------------------------
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8.2-a -mtune=cortex-a76")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -march=armv8.2-a -mtune=cortex-a76")

# ---------------------------------------------------------------------------
# 构建类型
#   Release（未指定时默认）：-O3 -DNDEBUG
#   Debug                  ：-O0 -g -DDEBUG（LOG_DEBUG 仅在此构建生效）
# ---------------------------------------------------------------------------
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -DDEBUG")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# ---------------------------------------------------------------------------
# 依赖项
#   OpenCV ≥ 4.8（6 个组件） / yaml-cpp / 系统线程库
# ---------------------------------------------------------------------------
find_package(OpenCV 4.8 REQUIRED COMPONENTS
    core
    imgproc
    videoio
    highgui
    imgcodecs
    video
)
find_package(yaml-cpp REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# 头文件路径：include/ 为项目头文件，tools/ 为 header-only 工具
# ---------------------------------------------------------------------------
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/tools)

# ---------------------------------------------------------------------------
# 源文件（5 个 .cpp）与头文件清单
# ---------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/ConfigManager.cpp
    src/CameraCapture.cpp
    src/SerialPort.cpp
    src/VisionPipeline.cpp
)

set(HEADERS
    include/ConfigManager.h
    include/CameraCapture.h
    include/SerialPort.h
    include/VisionPipeline.h
    tools/Logger.h
    tools/FPSCounter.h
    tools/ThreadSafeFrameBuffer.h
    tools/ThreadSafeSendQueue.h
    tools/ColorOrder.h
)

# ---------------------------------------------------------------------------
# 可执行文件：产物 visionpost，链接 OpenCV / yaml-cpp / pthread
# ---------------------------------------------------------------------------
add_executable(visionpost ${SOURCES} ${HEADERS})

target_link_libraries(visionpost
    ${OpenCV_LIBS}
    yaml-cpp
    Threads::Threads
)


# ---------------------------------------------------------------------------
# 安装
#   可执行文件 → bin/
#   config/    → share/RaspberryPi5_Vision_System/config/
#   scripts/   → share/RaspberryPi5_Vision_System/scripts/（保留可执行权限）
# ---------------------------------------------------------------------------
install(TARGETS visionpost DESTINATION bin)
install(DIRECTORY config/  DESTINATION share/${PROJECT_NAME}/config)
install(DIRECTORY scripts/ DESTINATION share/${PROJECT_NAME}/scripts
    USE_SOURCE_PERMISSIONS)
