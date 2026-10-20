# -*- coding: utf-8 -*-
"""
grab_window.py —— 用 PrintWindow(PW_RENDERFULLCONTENT) 抓取指定窗口截图
用法：python tools/grab_window.py <exe路径> [窗口标题关键字] [等待秒数] [输出png]
说明：屏幕锁定时 CopyFromScreen 拍到的是锁屏，PrintWindow 可直接抓窗口内容
      （含 DirectComposition 合成表面），因此即使无人值守也能验证显示效果。
"""
import ctypes
import ctypes.wintypes as wt
import sys
import subprocess
import time
import os

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

PW_RENDERFULLCONTENT = 0x00000002  # Win8.1+，抓取含 GPU 合成的窗口内容

BI_RGB = 0
DIB_RGB_COLORS = 0
SRCCOPY = 0x00CC0020


def find_window(keyword):
    """按标题关键字找可见顶层窗口（返回最接近的句柄）"""
    result = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb(hwnd, lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length <= 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        if keyword.lower() in buf.value.lower():
            result.append((hwnd, buf.value))
        return True

    user32.EnumWindows(cb, 0)
    return result


def grab(hwnd, out_path):
    rc = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    w, h = rc.right - rc.left, rc.bottom - rc.top
    if w <= 0 or h <= 0:
        raise RuntimeError("窗口尺寸异常 %dx%d" % (w, h))

    hdc_window = user32.GetWindowDC(hwnd)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_window)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                    ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                    ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                    ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                    ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                    ("biClrImportant", ctypes.c_uint32)]

    class BITMAPINFO(ctypes.Structure):
        _fields_ = [("bmiHeader", BITMAPINFOHEADER)]

    bmi = BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = w
    bmi.bmiHeader.biHeight = -h          # 自上而下
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = BI_RGB

    bits = ctypes.c_void_p()
    hbmp = gdi32.CreateDIBSection(hdc_mem, ctypes.byref(bmi), DIB_RGB_COLORS,
                                  ctypes.byref(bits), None, 0)
    gdi32.SelectObject(hdc_mem, hbmp)

    ok = user32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT)
    if not ok:
        ok = user32.PrintWindow(hwnd, hdc_mem, 0)  # 旧系统退化

    # BMP 写出后由 Pillow 无关方式转 PNG：这里直接写 32bpp BMP 再用 Qt/无关工具？
    # 不引第三方库，直接写 PNG 有难度，改为写 BMP（画图/资源管理器均可看）。
    bmp_path = os.path.splitext(out_path)[0] + ".bmp"
    row_bytes = w * 4
    data = (ctypes.c_ubyte * (row_bytes * h)).from_address(bits.value)

    # BMP 文件头
    # 注意：上面申请 DIB 时 biHeight 传的是 -h（自上而下），PrintWindow 按这个
    # 方向回填像素；文件头必须同样写 -h，否则看图工具会把整张图上下颠倒。
    import struct
    pixel_off = 14 + 40
    file_size = pixel_off + row_bytes * h
    fh = bytearray()
    fh += b"BM" + struct.pack("<IHHI", file_size, 0, 0, pixel_off)
    fh += struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, row_bytes * h, 0, 0, 0, 0)

    with open(bmp_path, "wb") as f:
        f.write(fh)
        f.write(bytes(data))

    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(hwnd, hdc_window)
    return bmp_path, ok


def main():
    exe = sys.argv[1]
    keyword = sys.argv[2] if len(sys.argv) > 2 else "MarkText"
    wait_s = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
    out = sys.argv[4] if len(sys.argv) > 4 else "grab.bmp"

    if exe and exe.strip():
        proc = subprocess.Popen([exe])
    time.sleep(wait_s)

    # keyword 也可以直接给窗口句柄（十进制或 0x 前缀）：
    # 中文窗口标题经 shell 传参会因代码页丢失，用句柄最稳。
    wins = []
    sKey = keyword.strip()
    if sKey.isdigit() or sKey.lower().startswith("0x"):
        hwnd = int(sKey, 0)
        if not user32.IsWindow(hwnd):
            print("NO_WINDOW for hwnd:", sKey)
            return 2
        length = user32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        wins.append((hwnd, buf.value))
    else:
        wins = find_window(keyword)

    if not wins:
        print("NO_WINDOW found for keyword:", keyword)
        return 2
    for hwnd, title in wins:
        rc = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rc))
        print("candidate:", hex(hwnd), title,
              rc.right - rc.left, "x", rc.bottom - rc.top)
    # 选面积最大的候选（主窗口通常远大于 tooltip/子弹层）
    def area(item):
        rc = wt.RECT()
        user32.GetWindowRect(item[0], ctypes.byref(rc))
        return (rc.right - rc.left) * (rc.bottom - rc.top)
    wins.sort(key=area, reverse=True)
    hwnd, title = wins[0]
    print("found window:", hex(hwnd), title)

    # 后台冷启动的窗口，DWM 可能还没有合成帧，PrintWindow 会抓到全黑。
    # 先最小化再还原（等价于"用户点一下任务栏"），逼 DWM 重建合成表面，再抓。
    SW_MINIMIZE = 6
    SW_RESTORE = 9
    user32.ShowWindow(hwnd, SW_MINIMIZE)
    time.sleep(1.0)
    user32.ShowWindow(hwnd, SW_RESTORE)
    user32.SetForegroundWindow(hwnd)
    time.sleep(2.5)

    bmp, ok = grab(hwnd, out)
    print("PrintWindow ok =", ok, "->", bmp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
