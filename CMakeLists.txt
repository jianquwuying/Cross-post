cmake_minimum_required(VERSION 3.20)
project(vision_system VERSION 1.1.0 LANGUAGES CXX)

# ── C++ 标准 ──────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── 构建配置 ─────────────────────────────────────────────────────
set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -fopenmp")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -Wall -Wextra -fopenmp")

# ── 依赖查找 ─────────────────────────────────────────────────────

# OpenCV 4.8 — 摄像头采集和图像处理
find_package(OpenCV 4.8 REQUIRED
    COMPONENTS core imgproc videoio highgui)

# NCNN — 神经网络推理引擎
# 优先尝试 cmake config, 其次 pkg-config, 最后手动指定路径
find_package(ncnn QUIET)
if(NOT ncnn_FOUND)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(NCNN QUIET ncnn)
    endif()
    if(NOT NCNN_FOUND)
        set(NCNN_INCLUDE_DIRS /usr/local/include/ncnn CACHE PATH "ncnn 头文件路径")
        set(NCNN_LIBRARIES ncnn CACHE STRING "ncnn 库名")
        message(STATUS "ncnn: 使用手动路径 (${NCNN_INCLUDE_DIRS})")
    endif()
    set(ncnn_FOUND TRUE)
endif()

# yaml-cpp — YAML 配置文件解析
find_package(yaml-cpp QUIET)
if(NOT yaml-cpp_FOUND)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(YAMLCPP QUIET yaml-cpp)
    endif()
    if(NOT YAMLCPP_FOUND)
        set(YAMLCPP_INCLUDE_DIRS /usr/local/include CACHE PATH "yaml-cpp 头文件路径")
        set(YAMLCPP_LIBRARIES yaml-cpp CACHE STRING "yaml-cpp 库名")
        message(STATUS "yaml-cpp: 使用手动路径")
    endif()
    set(yaml-cpp_FOUND TRUE)
endif()

# ── 源文件列表 ────────────────────────────────────────────────────
set(SOURCES
    main.cpp
    camera/camera.cpp
    yolo/yolo26.cpp
    color/hsv.cpp
    serial/serial.cpp
    config/config.cpp
    roi/roi.cpp
    logger/logger.cpp
)

set(HEADERS
    protocol/protocol.h
    camera/camera.h
    yolo/yolo26.h
    color/hsv.h
    serial/serial.h
    config/config.h
    roi/roi.h
    logger/logger.h
)

# ── 可执行文件 ────────────────────────────────────────────────────
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

# ── 头文件搜索路径 ────────────────────────────────────────────────
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

if(NCNN_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${NCNN_INCLUDE_DIRS})
elseif(ncnn_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${ncnn_INCLUDE_DIRS})
else()
    target_include_directories(${PROJECT_NAME} PRIVATE /usr/local/include/ncnn)
endif()

if(YAMLCPP_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${YAMLCPP_INCLUDE_DIRS})
elseif(yaml-cpp_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${yaml-cpp_INCLUDE_DIRS})
endif()

# ── 链接库 ────────────────────────────────────────────────────────
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OpenCV_LIBS}
    ncnn
    yaml-cpp
    pthread
)

# ── 编译选项 ──────────────────────────────────────────────────────
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra
    $<$<CONFIG:Release>:-O3 -march=native -flto>
)

# ── 第二个可执行文件: vision_demo (视频推理 Demo) ──────────────────
set(DEMO_SOURCES
    src/vision_demo.cpp
    camera/camera.cpp
    yolo/yolo26.cpp
    color/hsv.cpp
    roi/roi.cpp
    logger/logger.cpp
    src/input/frame_source_factory.cpp
)

set(DEMO_HEADERS
    src/input/frame_source.hpp
    src/input/camera_source.hpp
    src/input/video_source.hpp
    src/input/frame_source_factory.hpp
    yolo/yolo26.h
    color/hsv.h
    roi/roi.h
    logger/logger.h
    camera/camera.h
)

add_executable(vision_demo ${DEMO_SOURCES} ${DEMO_HEADERS})

target_include_directories(vision_demo PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

# NCNN include
if(NCNN_INCLUDE_DIRS)
    target_include_directories(vision_demo PRIVATE ${NCNN_INCLUDE_DIRS})
elseif(ncnn_INCLUDE_DIRS)
    target_include_directories(vision_demo PRIVATE ${ncnn_INCLUDE_DIRS})
else()
    target_include_directories(vision_demo PRIVATE /usr/local/include/ncnn)
endif()

# yaml-cpp include
if(YAMLCPP_INCLUDE_DIRS)
    target_include_directories(vision_demo PRIVATE ${YAMLCPP_INCLUDE_DIRS})
elseif(yaml-cpp_INCLUDE_DIRS)
    target_include_directories(vision_demo PRIVATE ${yaml-cpp_INCLUDE_DIRS})
endif()

target_link_libraries(vision_demo PRIVATE
    ${OpenCV_LIBS}
    ncnn
    yaml-cpp
    pthread
)

target_compile_options(vision_demo PRIVATE
    -Wall -Wextra
    $<$<CONFIG:Release>:-O3 -march=native -flto>
)

# ── 安装规则 ──────────────────────────────────────────────────────
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
install(FILES ${HEADERS} DESTINATION include/${PROJECT_NAME})

# ── 打印配置摘要 ──────────────────────────────────────────────────
message(STATUS "========================================")
message(STATUS "  视觉识别系统 v${PROJECT_VERSION}")
message(STATUS "  C++ 标准: ${CMAKE_CXX_STANDARD}")
message(STATUS "  构建类型: ${CMAKE_BUILD_TYPE}")
message(STATUS "  OpenCV:   ${OpenCV_VERSION}")
message(STATUS "  ncnn:     已找到")
message(STATUS "  yaml-cpp: 已找到")
message(STATUS "========================================")
