#!/usr/bin/env python3
# ============================================================================
# capture_templates.py  —  模式1 数字模板制作工具
# ============================================================================
# 用途：从摄像头（或单张图片）框选数字，自动处理成「白底黑字」模板图，
#       保存到 templates/num1.png、num2.png、num3.png，供 visionpost 模式1 使用。
#
# 依赖：OpenCV Python（cv2）
#   sudo apt install python3-opencv
#
# 用法：
#   python3 capture_templates.py                 # 摄像头 0 实时取图
#   python3 capture_templates.py --index 1       # 指定摄像头索引
#   python3 capture_templates.py --image pic.jpg # 从已有图片取图
#   python3 capture_templates.py --outdir <dir>  # 自定义输出目录
#
# 操作：
#   [1]/[2]/[3]  为对应数字制作模板（冻结画面 → 拖动框选数字）
#   [q]/[ESC]    退出（需先点击摄像头/图片窗口获得键盘焦点）
#   框选时：拖动鼠标，SPACE/ENTER 确认，[c] 取消
#   预览时：[y] 保存   [r] 重新框选   其他键放弃
#   终端中可直接 Ctrl+C 退出
#
# 处理流程（与 visionpost 模板加载约定一致）：
#   灰度 → 3x3 高斯 → Otsu(THRESH_BINARY) → 数字黑、背景白
#   → 取最大轮廓（数字）→ 加 25% 边距裁剪 → 最长边归一化 240px → PNG
# ============================================================================

import argparse
import sys
from pathlib import Path

import cv2
import numpy as np

# 默认输出目录：项目根目录 templates/
DEFAULT_OUT_DIR = Path(__file__).resolve().parent.parent / "templates"

# 模板最长边归一化到该像素（Hu 矩尺度不变，尺寸不影响匹配）
TARGET_SIZE = 240

SELECT_WIN = "Select digit: drag + SPACE/ENTER, c=cancel"
PREVIEW_WIN = ("Preview: left=template (white bg, black digit)   "
               "right=algorithm view   [y]=save [r]=reselect [c]=cancel")


def process_digit(frame, roi):
    """把框选区域处理成白底黑字模板。

    返回 (template, preview, warning)；失败时 template 为 None。
    """
    x, y, w, h = (int(v) for v in roi)
    if w < 8 or h < 8:
        return None, None, "框选区域太小，请重新框选"

    crop = frame[y:y + h, x:x + w]
    gray = cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY)
    gray = cv2.GaussianBlur(gray, (3, 3), 0)

    # Otsu 二值化：数字黑(0)、背景白(255)
    _, th = cv2.threshold(gray, 0, 255,
                          cv2.THRESH_BINARY + cv2.THRESH_OTSU)

    # 反色后找数字轮廓（数字为白前景）
    contours, _ = cv2.findContours(cv2.bitwise_not(th),
                                   cv2.RETR_EXTERNAL,
                                   cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return None, None, "框选区域内没有找到数字轮廓"

    largest = max(contours, key=cv2.contourArea)
    bx, by, bw, bh = cv2.boundingRect(largest)

    warn = ""
    if (bx <= 0 or by <= 0 or
            bx + bw >= th.shape[1] or by + bh >= th.shape[0]):
        warn = "警告：数字贴到框选边缘，可能被裁切，建议重新框选"

    # 按数字外接框加 25% 边距裁剪（白底黑字）
    margin = int(0.25 * max(bw, bh)) + 10
    x0 = max(0, bx - margin)
    y0 = max(0, by - margin)
    x1 = min(th.shape[1], bx + bw + margin)
    y1 = min(th.shape[0], by + bh + margin)
    digit = th[y0:y1, x0:x1]

    # 归一化最长边（NEAREST 插值保持纯 0/255 二值，不引入中间灰度）
    scale = TARGET_SIZE / max(digit.shape)
    digit = cv2.resize(digit, (0, 0), fx=scale, fy=scale,
                       interpolation=cv2.INTER_NEAREST)

    # 预览：左侧最终模板，右侧算法视角（反色，数字为白前景）
    h = digit.shape[0]
    sep = np.full((h, 8), 255, dtype=np.uint8)
    preview = np.hstack([digit, sep, cv2.bitwise_not(digit)])
    return digit, preview, warn


def crop_loop(frame, digit_no, out_dir):
    """对一个数字完成 框选 → 处理 → 预览 → 保存 的交互循环。"""
    while True:
        roi = cv2.selectROI(SELECT_WIN, frame, False)
        cv2.destroyWindow(SELECT_WIN)
        if roi[2] == 0 or roi[3] == 0:
            print(f"已取消数字 {digit_no} 的框选")
            return

        tmpl, preview, warn = process_digit(frame, roi)
        if tmpl is None:
            print(f"数字 {digit_no}: {warn}")
            continue

        print(f"数字 {digit_no} 模板预览{(' — ' + warn) if warn else ''}")
        cv2.imshow(PREVIEW_WIN, preview)
        key = cv2.waitKey(0) & 0xFF
        cv2.destroyWindow(PREVIEW_WIN)

        if key == ord('y'):
            out_dir.mkdir(parents=True, exist_ok=True)
            out = out_dir / f"num{digit_no}.png"
            existed = out.exists()
            cv2.imwrite(str(out), tmpl)
            print(f"{'已覆盖' if existed else '已保存'}: {out}")
            return
        if key == ord('r'):
            print("重新框选")
            continue
        print(f"已放弃数字 {digit_no}")
        return


def open_camera(index):
    """打开摄像头，返回 (cap, shape) 或 (None, None)。

    优先 V4L2 后端：本机 OpenCV 默认的 GStreamer 后端在此 UVC 摄像头上
    无法启动数据流（v4l2src Internal data stream error），
    与 visionpost 直接使用 V4L2 ioctl 的方式一致。
    """
    cap = cv2.VideoCapture(index, cv2.CAP_V4L2)
    if not cap.isOpened():
        cap = cv2.VideoCapture(index)   # 回退默认后端
    if not cap.isOpened():
        print(f"无法打开摄像头索引 {index}（/dev/video{index}）")
        return None, None

    # 与 visionpost 相同的采集参数（MJPG 640x480 @ 60），尽力而为
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    cap.set(cv2.CAP_PROP_FPS, 60)

    # 读首帧验证数据流可用
    ok, frame = cap.read()
    if not ok or frame is None:
        print("摄像头数据流无法启动：可能被 visionpost 或其他程序占用")
        cap.release()
        return None, None

    print(f"摄像头已打开: {frame.shape[1]}x{frame.shape[0]}")
    return cap, frame.shape


def run_camera(cap, out_dir):
    print("操作: [1]/[2]/[3] 制作对应数字模板   [q]/[ESC] 退出（按键需在画面上按下）")
    while True:
        ok, frame = cap.read()
        if not ok:
            print("读取摄像头画面失败")
            return 1
        cv2.putText(frame, "1/2/3: capture digit   q: quit",
                    (10, 25), cv2.FONT_HERSHEY_SIMPLEX, 0.7,
                    (0, 255, 0), 2)
        cv2.imshow("Camera (visionpost template tool)", frame)
        key = cv2.waitKey(1) & 0xFF
        if key in (ord('q'), 27):
            break
        if key in (ord('1'), ord('2'), ord('3')):
            crop_loop(frame, key - ord('0'), out_dir)
            cv2.destroyAllWindows()
    return 0


def run_image(path, out_dir):
    frame = cv2.imread(path)
    if frame is None:
        print(f"无法读取图片: {path}")
        return 1
    print("操作: [1]/[2]/[3] 制作对应数字模板   [q]/[ESC] 退出（按键需在画面上按下）")
    while True:
        cv2.imshow("Image (visionpost template tool)", frame)
        key = cv2.waitKey(0) & 0xFF
        if key in (ord('q'), 27):
            break
        if key in (ord('1'), ord('2'), ord('3')):
            crop_loop(frame, key - ord('0'), out_dir)
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="制作 visionpost 模式1 数字模板 num1/num2/num3.png")
    parser.add_argument("--index", type=int, default=0,
                        help="摄像头设备索引（默认 0，即 /dev/video0）")
    parser.add_argument("--image", type=str, default=None,
                        help="从图片文件取图（不打开摄像头）")
    parser.add_argument("--outdir", type=str, default=str(DEFAULT_OUT_DIR),
                        help="模板输出目录（默认项目根目录 templates/）")
    args = parser.parse_args()

    out_dir = Path(args.outdir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"模板输出目录: {out_dir}")
    print("说明：模板需为白底黑字、正视角、数字清晰；框选时尽量只框住数字本身。")

    ret = 0
    cap = None
    try:
        if args.image:
            ret = run_image(args.image, out_dir)
        else:
            cap, _shape = open_camera(args.index)
            if cap is None:
                return 1
            ret = run_camera(cap, out_dir)
    except KeyboardInterrupt:
        print("\n已退出（Ctrl+C）")
    finally:
        if cap is not None:
            cap.release()
        cv2.destroyAllWindows()
    return ret


if __name__ == "__main__":
    sys.exit(main())
