# ============================================================================
# hsv_calibrator.py  —  模式0 HSV 阈值调参工具
# ============================================================================
# 用途：拖动 6 个 trackbar 实时查看 inRange 掩码，按 's' 打印可粘贴到
#       config/vision.yaml 中 mode0.colors.<颜色>.range1 的 YAML 片段；
#       输出为 2 空格缩进的单行，粘贴时需按现有层级补足缩进。
# 注意：模式0 的红色为双区间（range1 + range2），本工具只输出 range1；
#       range2 需按同样方法另行标定。
#
# 依赖：OpenCV Python（cv2）
#   sudo apt install python3-opencv
#
# 用法：
#   python3 hsv_calibrator.py
#
# 操作：
#   's'  打印当前阈值（range1 格式）
#   'q'  退出
# ============================================================================

import cv2
import numpy as np

# 窗口名称
WINDOW_NAME = "HSV Calibration"
cv2.namedWindow(WINDOW_NAME)
cv2.namedWindow("Mask", cv2.WINDOW_NORMAL)

# 初始 HSV 范围：覆盖 OpenCV 全区间，便于从零开始扫描；
# 实际阈值以 config/vision.yaml 的 mode0.colors 为准
h_min, h_max = 0, 180
s_min, s_max = 0, 255
v_min, v_max = 0, 255

# 创建滑动条
cv2.createTrackbar("H Min", WINDOW_NAME, h_min, 180, lambda x: None)
cv2.createTrackbar("H Max", WINDOW_NAME, h_max, 180, lambda x: None)
cv2.createTrackbar("S Min", WINDOW_NAME, s_min, 255, lambda x: None)
cv2.createTrackbar("S Max", WINDOW_NAME, s_max, 255, lambda x: None)
cv2.createTrackbar("V Min", WINDOW_NAME, v_min, 255, lambda x: None)
cv2.createTrackbar("V Max", WINDOW_NAME, v_max, 255, lambda x: None)

# 打开默认摄像头（索引 0，即 /dev/video0；使用 OpenCV 默认后端与默认分辨率）
cap = cv2.VideoCapture(0)
if not cap.isOpened():
    print("无法打开摄像头，请检查设备号或权限。")
    exit()

print("\n========== HSV 调参工具使用说明 ==========")
print("1. 鼠标拖动滑块，Mask 窗口会实时显示选中的颜色区域。")
print("2. 调整滑块，直到 Mask 画面中只剩下目标颜色（如黄色）的实心块。")
print("3. 调整满意后，在画面窗口按下键盘的 's' 键，终端会打印出 YAML 格式的参数。")
print("4. 按 'q' 键退出程序。")
print("==========================================\n")

while True:
    ret, frame = cap.read()
    if not ret:
        break

    # 转换为 HSV
    hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)

    # 读取当前滑块的值
    h_min = cv2.getTrackbarPos("H Min", WINDOW_NAME)
    h_max = cv2.getTrackbarPos("H Max", WINDOW_NAME)
    s_min = cv2.getTrackbarPos("S Min", WINDOW_NAME)
    s_max = cv2.getTrackbarPos("S Max", WINDOW_NAME)
    v_min = cv2.getTrackbarPos("V Min", WINDOW_NAME)
    v_max = cv2.getTrackbarPos("V Max", WINDOW_NAME)

    # 阈值提取
    lower = np.array([h_min, s_min, v_min])
    upper = np.array([h_max, s_max, v_max])
    mask = cv2.inRange(hsv, lower, upper)

    # 显示画面
    cv2.imshow(WINDOW_NAME, frame)
    cv2.imshow("Mask", mask)

    key = cv2.waitKey(1) & 0xFF
    if key == ord('q'):
        break
    elif key == ord('s'):
        # 打印 mode0.colors.<颜色>.range1 的 YAML 片段（粘贴时需补足缩进）
        print("\n[YAML 配置 (2 空格缩进，粘贴时请按现有层级补足)]")
        print(f"  range1: {{ h_min: {h_min}, s_min: {s_min}, v_min: {v_min}, h_max: {h_max}, s_max: {s_max}, v_max: {v_max} }}")
        print("---------------------------------------------")

cap.release()
cv2.destroyAllWindows()