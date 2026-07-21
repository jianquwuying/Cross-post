#!/usr/bin/env bash
# ============================================================================
# stop.sh  —  停止 RaspberryPi5 视觉系统
# ============================================================================
# 向 visionpost 发送 SIGINT 以优雅关闭。
# ============================================================================

set -euo pipefail

echo "正在停止 visionpost ..."

# 先尝试优雅关闭
if pkill -SIGINT visionpost 2>/dev/null; then
    echo "已向 visionpost 发送 SIGINT"
    # 等待最多 5 秒以优雅退出
    for i in $(seq 1 50); do
        if ! pgrep visionpost > /dev/null 2>&1; then
            echo "visionpost 已优雅停止"
            exit 0
        fi
        sleep 0.1
    done
fi

# 如果仍在运行则强制终止
if pgrep visionpost > /dev/null 2>&1; then
    echo "正在强制终止 ..."
    pkill -9 visionpost 2>/dev/null || true
    echo "visionpost 已终止"
else
    echo "visionpost 未在运行"
fi
