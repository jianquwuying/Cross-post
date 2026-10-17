#!/usr/bin/env bash
# ============================================================================
# autostart-gui.sh — 开机自启动入口（供 ~/.config/autostart/*.desktop 调用）
# 26年第十二届工程实践与创新大赛（参赛作品）· 树莓派 5 视觉终端 visionpost
#
# 为什么需要这一层：
#   登录初期 gnome-terminal-server / D-Bus 往往还没就绪，此时直接
#   `gnome-terminal ...` 会以
#     "# Error constructing proxy for org.gnome.Terminal ... 已到超时限制"
#   失败（退出码 1），终端不弹出、看门狗也就不会启动。
#
# 策略：
#   1) 优先弹终端窗口跑看门狗（方便现场看日志）——最多重试 GUI_TRIES 次
#   2) 若图形终端始终起不来，退化为后台无窗口运行，保证机器人一定跑起来
#
# 说明：.desktop 的 Exec 不经过 shell，$HOME / ~ 都不会被展开
#       （写了会导致自启动项非法、根本不执行）；因此 Exec 必须写指向
#       本脚本的绝对路径，例如 /home/r9/Cross-post/scripts/autostart-gui.sh。
# ============================================================================
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WATCHDOG="${DIR}/autostart.sh"
PID_FILE="/tmp/visionpost-autostart.pid"
LOG="${DIR}/../record/autostart-gui.log"

TITLE='Visionpost 看门狗'
GUI_TRIES=3        # 尝试弹终端次数
GUI_WAIT=6         # 每次失败后的等待（秒）

mkdir -p "$(dirname "${LOG}")"

log_gui() {
    echo "[$(date '+%F %T')] $*" >>"${LOG}"
}

is_running() {                       # 排除僵尸进程（kill -0 对僵尸也返回真）
    local pid="${1:-}"
    [[ -n "${pid}" ]] || return 1
    kill -0 "${pid}" 2>/dev/null || return 1
    [[ "$(ps -o stat= -p "${pid}" 2>/dev/null | tr -d '[:space:]')" != Z* ]]
}

# 看门狗已在运行 → 什么都不做
if [[ -f "${PID_FILE}" ]] && is_running "$(cat "${PID_FILE}")"; then
    log_gui "看门狗已在运行 (PID $(cat "${PID_FILE}"))，跳过"
    exit 0
fi

log_gui "自启动入口启动：WATCHDOG=${WATCHDOG}"

if command -v gnome-terminal >/dev/null 2>&1; then
    for ((i = 1; i <= GUI_TRIES; i++)); do
        if gnome-terminal --title="${TITLE}" -- bash "${WATCHDOG}" 2>>"${LOG}"; then
            log_gui "已通过 gnome-terminal 拉起看门狗（第 ${i} 次尝试成功）"
            exit 0
        fi
        log_gui "gnome-terminal 拉起失败（第 ${i}/${GUI_TRIES} 次），${GUI_WAIT}s 后重试"
        sleep "${GUI_WAIT}"
    done
else
    log_gui "未找到 gnome-terminal"
fi

log_gui "图形终端不可用 → 退化为后台运行（无窗口；看门狗日志见 record/autostart.log）"
nohup bash "${WATCHDOG}" >>"${LOG}" 2>&1 &
exit 0
