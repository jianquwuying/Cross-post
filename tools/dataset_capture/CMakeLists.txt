cmake_minimum_required(VERSION 3.20)
project(dataset_capture VERSION 1.0.0 LANGUAGES CXX)

# ── C++ 标准 ──────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── 构建配置 ─────────────────────────────────────────────────────
set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")

# ── 依赖 ─────────────────────────────────────────────────────────
find_package(OpenCV 4.8 REQUIRED
    COMPONENTS core imgproc videoio highgui imgcodecs)

find_package(yaml-cpp REQUIRED)

# ── 源文件 ───────────────────────────────────────────────────────
set(SOURCES
    main.cpp
    config.cpp
    dataset_capture.cpp
)

set(HEADERS
    config.h
    dataset_capture.h
)

# ── 可执行文件 ────────────────────────────────────────────────────
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OpenCV_LIBS}
    yaml-cpp
    pthread
)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
