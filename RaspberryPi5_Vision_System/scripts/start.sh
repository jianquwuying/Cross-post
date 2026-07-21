#!/usr/bin/env bash
# ============================================================================
# start.sh  —  启动 RaspberryPi5 视觉系统
# ============================================================================
# 用法：./start.sh [config_path]
#
# 1. 相对于本脚本定位项目目录。
# 2. 设置串口权限。
# 3. 启动 visionpost。
# ============================================================================

set -euo pipefail

# ---- 定位项目根目录 ----------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
CONFIG_FILE="${1:-${PROJECT_DIR}/config/vision.yaml}"

echo "========================================"
echo "RaspberryPi5 视觉系统 —— 启动"
echo "========================================"
echo "项目目录 : ${PROJECT_DIR}"
echo "配置文件 : ${CONFIG_FILE}"
echo ""

# ---- 串口权限 ----------------------------------------------------------
SERIAL_PORT="/dev/ttyAMA10"
if [ -e "${SERIAL_PORT}" ]; then
    echo "正在设置 ${SERIAL_PORT} 权限 ..."
    sudo chmod 666 "${SERIAL_PORT}" 2>/dev/null || {
        echo "警告：无法 chmod ${SERIAL_PORT} —— 继续运行"
    }
else
    echo "警告：未找到 ${SERIAL_PORT}"
fi

# ---- 检查可执行文件 ----------------------------------------------------
EXEC="${BUILD_DIR}/visionpost"
if [ ! -x "${EXEC}" ]; then
    echo "错误：在 ${EXEC} 未找到 visionpost"
    echo "       请先构建：cd ${PROJECT_DIR} && mkdir -p build && cd build && cmake .. && make -j4"
    exit 1
fi

# ---- 启动 ---------------------------------------------------------------
echo "正在启动 visionpost ..."
cd "${PROJECT_DIR}"
exec "${EXEC}" "${CONFIG_FILE}"
