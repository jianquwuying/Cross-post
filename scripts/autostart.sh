#!/usr/bin/env bash
# ============================================================================
# autostart.sh — RaspberryPi5 视觉系统：开机自启动 + 看门狗（整合为一）
#
# 参考模板：Horizon_Rm_Vision_26_SP/autostart_V1.sh
#
# ── 子命令 ────────────────────────────────────────────────────────────────
#   start    启动看门狗并监管 visionpost（无参数时默认 = start）
#            visionpost 崩溃/退出后自动拉起；连续快速崩溃超过上限则放弃，
#            保持终端提示（不重启整机）
#   stop     优雅停止 visionpost 与看门狗（SIGINT 优雅关闭）
#   restart  stop 后再 start（当前终端转为看门狗）
#   status   查看看门狗 / visionpost 运行状态
#   用法示例：bash scripts/autostart.sh [start|stop|restart|status] [config.yaml]
#
# ── 开机自启动（手动部署到“启动应用程序”）────────────────────────────────
#   1. 打开「启动应用程序」(Startup Applications)
#   2. 添加启动项，名称随意（例如：Visionpost）
#   3. “命令”一栏粘贴下面这【一行】（开机后会自动弹出终端实时显示看门狗日志）：
#
#       gnome-terminal --title="Visionpost 看门狗" -- bash /home/shu/Cross-post/scripts/autostart.sh
#
#   4. 保存。重启后即可自动运行。
#
# ── 停止 ──────────────────────────────────────────────────────────────────
#   在弹出来的看门狗终端里按 Ctrl+C，或在任意终端执行：bash scripts/autostart.sh stop
#
# ── 串口权限 ──────────────────────────────────────────────────────────────
#   机器人实际使用 /dev/ttyACM0。脚本沿用参考脚本做法：
#   `echo <密码> | sudo -S chmod 666` 开机放权。系统 sudo 密码为 1；
#   若已改密码，请修改下方 SUDO_PASS 一行。
#   更稳妥方案（无需 sudo / 密码）：改用 udev 规则，见文件末尾注释。
# ============================================================================

# 不用 set -e：看门狗需要自行处理子进程非零退出；只打开“未定义变量即报错”
set -u

# ---------------------------------------------------------------------------
# 可配置参数
# ---------------------------------------------------------------------------
SUDO_PASS='1'                          # sudo 密码（改密码请同步修改本行）
SEC=2                                  # 崩溃后重启间隔（秒）
PROGRAM='visionpost'
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
EXEC="${BUILD_DIR}/${PROGRAM}"
CONFIG="${PROJECT_DIR}/config/vision.yaml"
MAX_RESTARTS=10                        # 连续快速崩溃上限（超过 → 放弃自动重启）
STABLE_UPTIME_S=30                     # 上次存活 ≥ 此秒数视为“已恢复”，清零崩溃计数
# 机器人串口实际为 /dev/ttyACM0；其余设备按需放权（不存在则自动跳过）
SERIAL_PORTS=(
    /dev/ttyACM0
    /dev/ttyACM1
    /dev/ttyACM2
    /dev/ttyACM3
    /dev/ttyUSB0
    /dev/ttyUSB1
    /dev/ttyCH341USB0
    /dev/ttyCH341USB1
)

PID_FILE="/tmp/${PROGRAM}-autostart.pid"
LOG_FILE="${PROJECT_DIR}/record/autostart.log"

# 便于快速自测的覆盖开关（用法）：
#   AUTOSTART_EXEC=false AUTOSTART_SEC=0 AUTOSTART_MAX_RESTARTS=3 \
#       bash scripts/autostart.sh
EXEC="${AUTOSTART_EXEC:-$EXEC}"
SEC="${AUTOSTART_SEC:-$SEC}"
MAX_RESTARTS="${AUTOSTART_MAX_RESTARTS:-$MAX_RESTARTS}"

child_pid=0
bg_sleep_pid=0

# ---------------------------------------------------------------------------
# 工具函数
# ---------------------------------------------------------------------------
interruptible_sleep() {                  # bash 等待前台外部命令结束前不会执行 trap，
    sleep "$1" &                         # 故用“后台 sleep + wait”保证 INT/TERM 能被立即响应
    bg_sleep_pid=$!
    wait "$bg_sleep_pid"
    bg_sleep_pid=0
}

log() {                                  # 同时输出到终端与日志文件
    local ts
    ts="$(date '+%F %T')"
    echo "[${ts}] $*"
    echo "[${ts}] $*" >>"${LOG_FILE}"
}

cleanup() {                              # EXIT trap：停掉子进程 + 清理 pid 文件
    if [[ "${bg_sleep_pid}" != "0" ]]; then
        kill -9 "${bg_sleep_pid}" 2>/dev/null
        bg_sleep_pid=0
    fi
    if [[ "${child_pid}" != "0" ]] && kill -0 "${child_pid}" 2>/dev/null; then
        log "正在停止 ${PROGRAM} (PID ${child_pid}) ..."
        kill -INT "${child_pid}" 2>/dev/null
        for _ in $(seq 1 50); do
            kill -0 "${child_pid}" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "${child_pid}" 2>/dev/null; then
            log "未能在 5s 内优雅退出，强制终止 (SIGKILL)。"
            kill -9 "${child_pid}" 2>/dev/null
        fi
    fi
    rm -f "${PID_FILE}"
}
handle_signal() {                        # INT / TERM / HUP：优雅整体退出
    log "收到退出信号，正在退出。"
    exit 0
}

fix_serial() {                           # 开机串口放权（参考脚本做法，密码管道 sudo）
    local dev
    for dev in "${SERIAL_PORTS[@]}"; do
        [[ -e "$dev" ]] || continue
        if [[ ! -w "$dev" ]]; then
            echo "${SUDO_PASS}" | sudo -S chmod 666 "$dev" 2>/dev/null || \
                echo "[autostart] 警告：无法 chmod $dev —— 检查密码或改用 udev 规则"
        fi
    done
}

# ---------------------------------------------------------------------------
# start —— 看门狗主循环
# ---------------------------------------------------------------------------
run_watchdog() {
    local crash_streak=0 uptime=0 launch_ts=0 rc=0

    # 单实例守卫
    if [[ -f "${PID_FILE}" ]] && kill -0 "$(cat "${PID_FILE}")" 2>/dev/null; then
        echo "[autostart] 已有看门狗在运行 (PID $(cat "${PID_FILE}"))，本实例退出。"
        exit 0
    fi
    if [[ ! -x "${BUILD_DIR}/${PROGRAM}" ]]; then
        echo "错误：未找到 ${BUILD_DIR}/${PROGRAM}，请先编译。"
        echo "       cd ${PROJECT_DIR} && mkdir -p build && cd build && cmake .. && make -j4"
        exit 1
    fi
    # 信号与退出清理：仅在看门狗进程内安装，避免 stop/status 误删运行中的 pid 文件
    trap handle_signal INT TERM HUP
    trap cleanup EXIT
    echo $$ >"${PID_FILE}"
    mkdir -p "$(dirname "${LOG_FILE}")"
    fix_serial

    log "================================================="
    log "看门狗已启动  PID=$$ | 程序=${EXEC} | 配置=${CONFIG}"
    cd "${BUILD_DIR}"                    # 与 README 一致，yaml 内相对路径以 build 为基准

    while :; do
        # 与外部手动启动的 visionpost 冲突检测（本脚本拉起的子进程不计）
        if [[ "${child_pid}" == "0" ]] && pgrep -x "${PROGRAM}" >/dev/null 2>&1; then
            log "检测到外部已有 ${PROGRAM} 进程，停止看门狗以避免冲突。"
            exit 0
        fi

        fix_serial
        launch_ts="$(date +%s)"
        # 非交互 shell 的后台子进程会继承“忽略 SIGINT/QUIT”，
        # 在子 shell 中先恢复默认信号再 exec，确保 stop / Ctrl+C 能送达 visionpost
        ( trap - INT QUIT HUP; exec "${EXEC}" "${CONFIG}" ) &
        child_pid=$!
        log "${PROGRAM} 已启动 (PID ${child_pid})"

        # 阻塞等待子进程退出（崩溃 / 收到信号均在此返回）
        wait "${child_pid}"
        rc=$?
        child_pid=0
        uptime=$(( $(date +%s) - launch_ts ))

        # 快速崩溃计数：存活 < STABLE_UPTIME 才累计，否则视为已恢复并清零
        if (( uptime < STABLE_UPTIME_S )); then
            crash_streak=$((crash_streak + 1))
        else
            crash_streak=0
        fi
        log "${PROGRAM} 已退出 (code=${rc}, 存活 ${uptime}s, 快速崩溃 x${crash_streak})"

        if (( crash_streak > MAX_RESTARTS )); then
            log "连续快速崩溃超过 ${MAX_RESTARTS} 次，放弃自动重启。"
            log "处理：按 Ctrl+C 关闭本终端，排障后重新运行；或执行 bash scripts/autostart.sh start"
            # 若希望改成“整机重启”兜底，取消下面一行的注释（需已配置免密 sudo）：
            # echo "${SUDO_PASS}" | sudo -S reboot
            while :; do interruptible_sleep 3600; done    # 保持终端打开，且可被 stop/Ctrl+C 打断
        fi

        log "${SEC}s 后重新启动 ${PROGRAM} ..."
        interruptible_sleep "${SEC}"
    done
}

# ---------------------------------------------------------------------------
# stop —— 优雅停止
# ---------------------------------------------------------------------------
cmd_stop() {
    echo "正在停止 visionpost 看门狗 ..."
    local wp stopped=0
    if [[ -f "${PID_FILE}" ]] && kill -0 "$(cat "${PID_FILE}")" 2>/dev/null; then
        wp="$(cat "${PID_FILE}")"
        kill -TERM "${wp}" 2>/dev/null || true
        for _ in $(seq 1 80); do         # 最长等 8s，让看门狗自行优雅收尾
            kill -0 "${wp}" 2>/dev/null || { stopped=1; break; }
            sleep 0.1
        done
        [[ "${stopped}" == "1" ]] && echo "看门狗已停止" \
            || echo "警告：看门狗未在 8s 内退出，请在其终端按 Ctrl+C。"
    else
        echo "未检测到运行中的看门狗。"
    fi

    # 兜底：仍有残留的 visionpost（如曾被手动运行）则优雅关闭
    if pgrep -x "${PROGRAM}" >/dev/null 2>&1; then
        pkill -SIGINT -x "${PROGRAM}" 2>/dev/null || true
        for _ in $(seq 1 50); do
            pgrep -x "${PROGRAM}" >/dev/null 2>&1 || { echo "visionpost 已停止"; return 0; }
            sleep 0.1
        done
        pkill -9 -x "${PROGRAM}" 2>/dev/null || true
        echo "visionpost 已强制终止"
    fi
}

# ---------------------------------------------------------------------------
# status —— 状态查询
# ---------------------------------------------------------------------------
cmd_status() {
    local wp="(无)" vp_count
    if [[ -f "${PID_FILE}" ]] && kill -0 "$(cat "${PID_FILE}")" 2>/dev/null; then
        wp="$(cat "${PID_FILE}")"
    fi
    vp_count="$(pgrep -c -x "${PROGRAM}" 2>/dev/null || true)"
    [[ -z "${vp_count}" ]] && vp_count=0
    echo "看门狗 PID  : ${wp}"
    echo "visionpost  : ${vp_count} 个进程"
    if pgrep -x "${PROGRAM}" >/dev/null 2>&1; then
        echo "运行时长    : $(ps -o etime= -p "$(pgrep -x "${PROGRAM}" | head -1)")"
    fi
    echo "日志文件    : ${LOG_FILE}"
}

# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------
main() {
    local cmd="${1:-start}"
    if [[ $# -gt 0 ]]; then shift; fi
    if [[ $# -ge 1 ]]; then CONFIG="$1"; fi   # 可选：覆盖配置文件路径

    case "${cmd}" in
        start|"")     run_watchdog ;;
        stop)         cmd_stop ;;
        restart)      cmd_stop; run_watchdog ;;
        status)       cmd_status ;;
        *)
            echo "用法: $0 {start|stop|restart|status} [config.yaml]"
            exit 1
            ;;
    esac
}

main "$@"

# ============================================================================
# 【可选 · 更稳妥的串口放权方式（udev，插拔/重启后自动 666，无需 sudo/密码）】
#   新建 /etc/udev/rules.d/99-visionpost.rules，内容：
#       KERNEL=="ttyACM[0-9]*", MODE="0666"
#       KERNEL=="ttyUSB[0-9]*", MODE="0666"
#       KERNEL=="ttyCH341USB[0-9]*", MODE="0666"
#   然后执行：sudo udevadm control --reload-rules
#   之后插拔串口即自动放权，可移除脚本内的 SUDO_PASS / fix_serial 逻辑。
# ============================================================================
