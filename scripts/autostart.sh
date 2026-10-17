#!/usr/bin/env bash
# ============================================================================
# autostart.sh — 26年第十二届工程实践与创新大赛（参赛作品）
#                树莓派 5 视觉终端 visionpost：开机自启动 + 看门狗（整合为一）
#
# ── 子命令 ────────────────────────────────────────────────────────────────
#   start    启动看门狗并监管 visionpost（无参数时默认 = start）
#            visionpost 崩溃/退出后自动拉起；连续快速崩溃超过上限则放弃，
#            保持终端提示（不重启整机）
#   stop     优雅停止 visionpost 与看门狗（SIGINT 优雅关闭）
#   restart  stop 后再 start（当前终端转为看门狗）
#   status   查看看门狗 / visionpost 运行状态
#   doctor   环境自检：可执行文件 / 配置文件 / 串口 / 摄像头 / sudo 权限
#   用法示例：bash scripts/autostart.sh [start|stop|restart|status|doctor] [config.yaml]
#
# ── 开机自启动（推荐使用 autostart-gui.sh）────────────────────────────────
#   1. 打开「启动应用程序」(Startup Applications)
#   2. 添加启动项，名称随意（例如：Visionpost）
#   3. “命令”一栏填写下面这【一行】（必须为绝对路径）：
#
#       bash /home/r9/Cross-post/scripts/autostart-gui.sh
#
#       .desktop 的 Exec / 启动命令不经过 shell，$HOME、~ 都不会展开，
#       必须写绝对路径；以上为本机示例，换机器时替换为实际项目路径。
#       autostart-gui.sh 先尝试弹终端运行本脚本，失败则退化为后台运行。
#   4. 保存。重启后即可自动运行。
#
# ── 停止 ──────────────────────────────────────────────────────────────────
#   在弹出来的看门狗终端里按 Ctrl+C，或在项目根目录执行：
#       bash scripts/autostart.sh stop
#
# ── 串口权限 ──────────────────────────────────────────────────────────────
#   放权对象不写死：优先使用 config/vision.yaml 的 serial.port，再自动扫描
#   /dev/ttyACM* /dev/ttyUSB* /dev/ttyCH341USB*（USB 转串口）。
#   本机不用板载 UART（ttyAMA0/ttyAMA10），故不纳入放权范围。
#   注：运行用户不在 dialout 组时，USB 串口必须放权才能打开。
#   放权顺序：先试免密 sudo（sudo -n），失败再退回密码管道
#   `echo <密码> | sudo -S chmod 666`；密码见下方 SUDO_PASS（本机为 1，
#   可用环境变量 AUTOSTART_SUDO_PASS 覆盖）。
#   更稳妥方案（无需 sudo / 密码）：改用 udev 规则，见文件末尾注释。
# ============================================================================

# 不用 set -e：看门狗需要自行处理子进程非零退出；只打开“未定义变量即报错”
set -u

# ---------------------------------------------------------------------------
# 可配置参数
# ---------------------------------------------------------------------------
SUDO_PASS="${AUTOSTART_SUDO_PASS:-1}"  # sudo 密码（改密码请改本行，或用环境变量覆盖）
SEC=2                                  # 快速崩溃后的重启间隔（秒）
PROGRAM='visionpost'
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
EXEC="${BUILD_DIR}/${PROGRAM}"
CONFIG="${PROJECT_DIR}/config/vision.yaml"
MAX_RESTARTS=10                        # 连续快速崩溃上限（超过即放弃，最多自动重启 10 次）
STABLE_UPTIME_S=30                     # 上次存活 ≥ 此秒数视为“已恢复”，清零崩溃计数
# 串口放权范围：config/vision.yaml 的 serial.port + 以下通配扫描（不存在则跳过）
# 本机使用 USB 转串口（/dev/ttyACM0），不使用板载 UART，故不含 /dev/ttyAMA*
SERIAL_GLOBS=(
    /dev/ttyACM*
    /dev/ttyUSB*
    /dev/ttyCH341USB*
)
HW_RETRY_SEC=5                         # 硬件未就绪（摄像头/串口缺失）时的重试间隔

PID_FILE="/tmp/${PROGRAM}-autostart.pid"
LOG_FILE="${PROJECT_DIR}/record/autostart.log"
APP_LOG="${PROJECT_DIR}/record/visionpost.out.log"   # visionpost 的 stdout/stderr

# 判定“硬件未就绪”的关键字（命中则长间隔重试，不消耗快速崩溃额度）
HW_MISSING_PATTERN='无法打开摄像头|摄像头打开失败|串口打开失败|视频源|无法打开 /dev/'

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

# kill -0 对"僵尸进程"（已退出但父进程还没回收）同样返回真，
# 直接用它会误判"还在运行"，导致 stop 白等 8s 并打印误导性警告。
is_running() {
    local pid="${1:-}"
    [[ -n "${pid}" ]] || return 1
    kill -0 "${pid}" 2>/dev/null || return 1
    [[ "$(ps -o stat= -p "${pid}" 2>/dev/null | tr -d '[:space:]')" != Z* ]]
}

vp_running() {                           # 是否存在"非僵尸"的 visionpost 进程
    local p
    for p in $(pgrep -x "${PROGRAM}" 2>/dev/null); do
        is_running "${p}" && return 0
    done
    return 1
}

cleanup() {                              # EXIT trap：停后台 sleep、优雅停止子进程、清理 pid 文件
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

sudo_run() {                             # 先免密 sudo，失败再退回密码管道
    sudo -n "$@" 2>/dev/null && return 0
    echo "${SUDO_PASS}" | sudo -S "$@" 2>/dev/null
}

config_serial_port() {                   # 读配置文件中第一个 port: 键（即 serial.port，最权威来源）
    [[ -f "${CONFIG}" ]] || return 0
    sed -n 's/^[[:space:]]*port:[[:space:]]*"\{0,1\}\([^"#[:space:]]*\)"\{0,1\}.*/\1/p' \
        "${CONFIG}" 2>/dev/null | head -1
}

serial_devices() {                       # 配置端口 + 通配扫描（只输出实际存在的设备）
    local dev
    dev="$(config_serial_port)"
    [[ -n "${dev}" && -e "${dev}" ]] && echo "${dev}"
    for dev in "${SERIAL_GLOBS[@]}"; do
        [[ -e "${dev}" ]] && echo "${dev}"
    done
    return 0
}

fix_serial() {                           # 串口放权：仅对"存在但当前不可写"的设备动手
    local dev seen=" "
    while IFS= read -r dev; do
        [[ -n "${dev}" && -e "${dev}" ]] || continue
        case "${seen}" in *" ${dev} "*) continue ;; esac
        seen="${seen}${dev} "
        [[ -w "${dev}" ]] && continue
        if sudo_run chmod 666 "${dev}"; then
            echo "[autostart] 串口已放权: ${dev}"
        else
            echo "[autostart] 警告：无法放权 ${dev}（检查 SUDO_PASS，或改用 udev 规则，见文件末尾）"
        fi
    done < <(serial_devices)
}

hw_missing_reason() {                    # 最近输出里是否提到摄像头/串口缺失
    [[ -f "${APP_LOG}" ]] || return 1
    tail -n 40 "${APP_LOG}" 2>/dev/null | grep -qE "${HW_MISSING_PATTERN}"
}

log_app_tail() {                         # 把程序最后几行输出写进看门狗日志
    [[ -f "${APP_LOG}" ]] || return 0
    local line
    while IFS= read -r line; do
        [[ -n "${line}" ]] && log "  | ${line}"
    done < <(tail -n 5 "${APP_LOG}" 2>/dev/null)
}

# ---------------------------------------------------------------------------
# start —— 看门狗主循环
# ---------------------------------------------------------------------------
run_watchdog() {
    local crash_streak=0 uptime=0 launch_ts=0 rc=0 hw_waits=0

    # 单实例守卫
    if [[ -f "${PID_FILE}" ]] && is_running "$(cat "${PID_FILE}")"; then
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
    # 每次看门狗启动重建程序输出日志（避免无限增长，便于查看本次运行的问题）
    : >"${APP_LOG}"
    log "程序输出将显示在本终端，同时记录到 ${APP_LOG}"
    cd "${BUILD_DIR}"                    # 以 build/ 为工作目录：yaml 内相对路径（模板/录制等）以此为基准

    while :; do
        # 与外部手动启动的 visionpost 冲突检测（本脚本拉起的子进程不计）
        if [[ "${child_pid}" == "0" ]] && vp_running; then
            log "检测到外部已有 ${PROGRAM} 进程，停止看门狗以避免冲突。"
            exit 0
        fi

        fix_serial
        launch_ts="$(date +%s)"
        echo "===== $(date '+%F %T') 启动 ${PROGRAM} =====" >>"${APP_LOG}"
        # 非交互 shell 的后台子进程会继承“忽略 SIGINT/QUIT”，
        # 在子 shell 中先恢复默认信号再 exec，确保 stop / Ctrl+C 能送达 visionpost
        ( trap - INT QUIT HUP; exec "${EXEC}" "${CONFIG}" ) \
            > >(tee -a "${APP_LOG}") 2>&1 &
        child_pid=$!
        log "${PROGRAM} 已启动 (PID ${child_pid})"

        # 阻塞等待子进程退出（崩溃 / 收到信号均在此返回）
        wait "${child_pid}"
        rc=$?
        child_pid=0
        uptime=$(( $(date +%s) - launch_ts ))

        # 硬件未就绪（开机时摄像头/串口还没枚举完）：长间隔重试，且不消耗崩溃额度
        if (( uptime < STABLE_UPTIME_S )) && hw_missing_reason; then
            log "${PROGRAM} 已退出 (code=${rc}, 存活 ${uptime}s) —— 判定为硬件未就绪"
            log_app_tail
            hw_waits=$((hw_waits + 1))
            if (( hw_waits == 1 || hw_waits % 12 == 0 )); then
                log "未检测到摄像头/串口，${HW_RETRY_SEC}s 后重试（不计入快速崩溃，约每分钟提示一次）"
            fi
            interruptible_sleep "${HW_RETRY_SEC}"
            continue
        fi

        # 快速崩溃计数：存活 < STABLE_UPTIME 才累计，否则视为已恢复并清零
        if (( uptime < STABLE_UPTIME_S )); then
            crash_streak=$((crash_streak + 1))
        else
            crash_streak=0
        fi
        log "${PROGRAM} 已退出 (code=${rc}, 存活 ${uptime}s, 快速崩溃 x${crash_streak})"

        # 快速崩溃时把程序最后几行输出写进日志，便于定位退出原因
        if (( uptime < STABLE_UPTIME_S )); then
            log_app_tail
        fi

        if (( crash_streak > MAX_RESTARTS )); then
            log "连续快速崩溃超过 ${MAX_RESTARTS} 次，放弃自动重启。"
            log "处理：按 Ctrl+C 关闭本终端，排障后重新运行；或执行 bash scripts/autostart.sh start"
            # 若希望改成“整机重启”兜底，取消下面一行的注释（使用 SUDO_PASS 密码管道）：
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
    if [[ -f "${PID_FILE}" ]] && is_running "$(cat "${PID_FILE}")"; then
        wp="$(cat "${PID_FILE}")"
        kill -TERM "${wp}" 2>/dev/null || true
        for _ in $(seq 1 80); do         # 最长等 8s，让看门狗自行优雅收尾
            is_running "${wp}" || { stopped=1; break; }
            sleep 0.1
        done
        [[ "${stopped}" == "1" ]] && echo "看门狗已停止" \
            || echo "警告：看门狗未在 8s 内退出，请在其终端按 Ctrl+C。"
    else
        echo "未检测到运行中的看门狗。"
    fi

    # 兜底：仍有残留的 visionpost（如曾被手动运行）则优雅关闭
    if vp_running; then
        pkill -SIGINT -x "${PROGRAM}" 2>/dev/null || true
        for _ in $(seq 1 50); do
            vp_running || { echo "visionpost 已停止"; return 0; }
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
    if [[ -f "${PID_FILE}" ]] && is_running "$(cat "${PID_FILE}")"; then
        wp="$(cat "${PID_FILE}")"
    fi
    vp_count="$(pgrep -c -x "${PROGRAM}" 2>/dev/null || true)"
    [[ -z "${vp_count}" ]] && vp_count=0
    echo "看门狗 PID  : ${wp}"
    echo "visionpost  : ${vp_count} 个进程"
    if vp_running; then
        echo "运行时长    : $(ps -o etime= -p "$(pgrep -x "${PROGRAM}" | head -1)")"
    fi
    echo "日志文件    : ${LOG_FILE}"
    echo "程序输出    : ${APP_LOG}"
}

# ---------------------------------------------------------------------------
# doctor —— 环境自检（适配本机：看清楚到底缺什么）
# ---------------------------------------------------------------------------
cmd_doctor() {
    echo "=== visionpost 环境自检 ==="
    echo "项目目录    : ${PROJECT_DIR}"
    if [[ -x "${BUILD_DIR}/${PROGRAM}" ]]; then
        echo "可执行文件  : 存在 (${EXEC})"
    else
        echo "可执行文件  : 缺失 —— 先编译：mkdir -p ${PROJECT_DIR}/build && cd ${PROJECT_DIR}/build && cmake .. && make -j4"
    fi
    echo "配置文件    : $([[ -f "${CONFIG}" ]] && echo "存在 (${CONFIG})" || echo "缺失 (${CONFIG})")"

    local port
    port="$(config_serial_port)"
    echo "配置串口    : ${port:-<未配置>}"
    if [[ -n "${port}" ]]; then
        if [[ -e "${port}" ]]; then
            echo "串口状态    : $([[ -w "${port}" ]] && echo 可写 || echo 不可写（看门狗会自动放权）)"
        else
            echo "串口状态    : 设备不存在（未插好 / 主控未上电）"
        fi
    fi
    echo "扫描到串口  : $(serial_devices | sort -u | tr '\n' ' ')"
    echo "  （USB 转串口；运行用户不在 dialout 组时由看门狗自动放权。不使用板载 UART）"

    local cam f name
    cam="$(for f in /sys/class/video4linux/*/name; do
               [[ -f "${f}" ]] || continue
               name="$(cat "${f}")"
               case "${name}" in
                   rpivid*|pispbe*) ;;       # 树莓派 5 内部 ISP/codec，不是摄像头
                   *) echo "${name}" ;;
               esac
           done | sort -u | tr '\n' ' ')"
    if [[ -n "${cam}" ]]; then
        echo "USB 摄像头  : ${cam}"
    else
        echo "USB 摄像头  : 未检测到（/dev/video* 里只有树莓派内部 ISP/codec 节点）"
    fi

    if sudo -n true 2>/dev/null; then
        echo "sudo        : 免密可用"
    else
        echo "sudo        : 需要密码（脚本使用 SUDO_PASS，可用 AUTOSTART_SUDO_PASS 覆盖）"
    fi
    echo "用户/组     : $(id -un) groups=$(id -Gn)"
    if [[ -f "${PID_FILE}" ]] && is_running "$(cat "${PID_FILE}")"; then
        echo "看门狗 PID  : $(cat "${PID_FILE}")"
    else
        echo "看门狗 PID  : 未运行"
    fi
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
        doctor)       cmd_doctor ;;
        *)
            echo "用法: $0 {start|stop|restart|status|doctor} [config.yaml]"
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
#
#   一键写入（本机默认 SUDO_PASS=1，可用 AUTOSTART_SUDO_PASS 覆盖）：
#     echo 1 | sudo -S bash -c 'cat > /etc/udev/rules.d/99-visionpost.rules <<EOF
#     KERNEL=="ttyACM[0-9]*", MODE="0666"
#     KERNEL=="ttyUSB[0-9]*", MODE="0666"
#     KERNEL=="ttyCH341USB[0-9]*", MODE="0666"
#     EOF'
#     sudo udevadm control --reload-rules && sudo udevadm trigger
# ============================================================================
