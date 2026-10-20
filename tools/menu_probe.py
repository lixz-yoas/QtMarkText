# -*- coding: utf-8 -*-
"""
menu_probe.py —— 菜单链路自动化探针（无人值守验证用）

流程：
  1. 把 QtMarkText 主窗口推到前台
  2. 点开指定顶级菜单（默认「编辑」）
  3. 悬停到目标菜单项并抓屏（确认高亮，证明坐标命中）
  4. 单击该项
  5. 抓主窗口（PrintWindow），确认命令在渲染层产生的可见效果

用法：python tools/menu_probe.py <hwnd> <顶级菜单x偏移> <项x> <项y> <输出前缀>
     x/y 均为相对窗口左上角的偏移（项坐标是弹出菜单里的位置）
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

PW_RENDERFULLCONTENT = 0x00000002


class BMPTool:
    @staticmethod
    def grab_screen(path):
        vx = user32.GetSystemMetrics(76)
        vy = user32.GetSystemMetrics(77)
        w = user32.GetSystemMetrics(78)
        h = user32.GetSystemMetrics(79)
        hdc = user32.GetDC(None)
        mdc = gdi32.CreateCompatibleDC(hdc)
        bmp, ptr = BMPTool._dib(mdc, w, h)
        gdi32.SelectObject(mdc, bmp)
        gdi32.BitBlt(mdc, 0, 0, w, h, hdc, vx, vy, 0x00CC0020)
        BMPTool._save(path, ptr, w, h)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mdc)
        user32.ReleaseDC(None, hdc)

    @staticmethod
    def grab_window(hwnd, path):
        rc = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rc))
        w, h = rc.right - rc.left, rc.bottom - rc.top
        hdc = user32.GetWindowDC(hwnd)
        mdc = gdi32.CreateCompatibleDC(hdc)
        bmp, ptr = BMPTool._dib(mdc, w, h)
        gdi32.SelectObject(mdc, bmp)
        user32.PrintWindow(hwnd, mdc, PW_RENDERFULLCONTENT)
        BMPTool._save(path, ptr, w, h)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mdc)
        user32.ReleaseDC(hwnd, hdc)

    @staticmethod
    def crop(src, dst, x0, y0, x1, y1):
        d = open(src, "rb").read()
        off = struct.unpack_from("<I", d, 10)[0]
        w, h = struct.unpack_from("<ii", d, 18)
        row = w * 4
        cw, ch = x1 - x0, y1 - y0
        crow = cw * 4
        out = bytearray()
        for yy in range(y0, y1):
            s = off + yy * row + x0 * 4
            out += d[s:s + crow]
        BMPTool._write(dst, cw, ch, out)

    @staticmethod
    def _dib(mdc, w, h):
        class BH(ctypes.Structure):
            _fields_ = [("s", ctypes.c_uint32), ("w", ctypes.c_int32),
                        ("h", ctypes.c_int32), ("p", ctypes.c_uint16),
                        ("b", ctypes.c_uint16), ("c", ctypes.c_uint32),
                        ("i", ctypes.c_uint32), ("x", ctypes.c_int32),
                        ("y", ctypes.c_int32), ("u", ctypes.c_uint32),
                        ("c2", ctypes.c_uint32)]
        class BI(ctypes.Structure):
            _fields_ = [("h", BH)]
        bi = BI()
        bi.h.s = 40
        bi.h.w = w
        bi.h.h = -h
        bi.h.p = 1
        bi.h.b = 32
        ptr = ctypes.c_void_p()
        bmp = gdi32.CreateDIBSection(mdc, ctypes.byref(bi), 0,
                                     ctypes.byref(ptr), None, 0)
        return bmp, ptr

    @staticmethod
    def _save(path, ptr, w, h):
        row = w * 4
        data = (ctypes.c_ubyte * (row * h)).from_address(ptr.value)
        BMPTool._write(path, w, h, bytes(data))

    @staticmethod
    def _write(path, w, h, pixels):
        row = w * 4
        off = 14 + 40
        size = off + row * h
        fh = bytearray(b"BM" + struct.pack("<IHHI", size, 0, 0, off)
                       + struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0,
                                     row * h, 0, 0, 0, 0))
        open(path, "wb").write(fh + pixels)


def click(x, y, d=0.3):
    user32.SetCursorPos(x, y)
    time.sleep(d)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.06)
    user32.mouse_event(0x0004, 0, 0, 0, 0)


def main():
    hwnd = int(sys.argv[1], 0)
    top_dx = int(sys.argv[2])
    item_dx = int(sys.argv[3])
    item_dy = int(sys.argv[4])
    prefix = sys.argv[5] if len(sys.argv) > 5 else "probe"

    rc = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    ox, oy = rc.left, rc.top
    print("window:", ox, oy, rc.right - rc.left, rc.bottom - rc.top)

    user32.SetForegroundWindow(hwnd)
    time.sleep(0.8)

    # 1) 打开顶级菜单
    click(ox + top_dx, oy + 12)
    time.sleep(1.0)

    # 2) 悬停目标项（不点击），抓屏确认高亮
    user32.SetCursorPos(ox + item_dx, oy + item_dy)
    time.sleep(0.8)
    BMPTool.grab_screen("%s_hover.bmp" % prefix)
    BMPTool.crop("%s_hover.bmp" % prefix, "%s_hover_crop.bmp" % prefix,
                 ox - 10, oy - 10, ox + 620, oy + 620)
    print("hover captured")

    # 3) 单击
    click(ox + item_dx, oy + item_dy)
    time.sleep(2.0)

    # 4) 抓主窗口
    BMPTool.grab_window(hwnd, "%s_after.bmp" % prefix)
    print("after captured")


if __name__ == "__main__":
    main()
