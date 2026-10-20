# -*- coding: utf-8 -*-
"""
bmp2png.py —— 32bpp BMP -> PNG（纯标准库，无需 Pillow）

为什么需要它：tools/grab_window.py 用 PrintWindow 抓下来的原始像素只能写 BMP，
而 BMP 无法用看图/预览工具直接展示结果。这里用 zlib 手写一个最小 PNG 编码器。

用法：python tools/bmp2png.py <in.bmp> [out.png]
"""
import struct
import sys
import zlib


def read_bmp32(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise RuntimeError("不是 BMP 文件")
    pixel_off = struct.unpack_from("<I", data, 10)[0]
    (hdr_size, width, height, planes, bpp, compression) = struct.unpack_from(
        "<IiiHHI", data, 14)
    if hdr_size < 40:
        raise RuntimeError("不支持的 BMP 头（size=%d）" % hdr_size)
    if bpp != 32:
        raise RuntimeError("只支持 32bpp BMP（当前 %d）" % bpp)

    bottom_up = height > 0
    height = abs(height)
    row_bytes = width * 4
    rows = []
    for y in range(height):
        src_y = (height - 1 - y) if bottom_up else y
        off = pixel_off + src_y * row_bytes
        rows.append(data[off:off + row_bytes])
    return width, height, rows


def write_png(path, width, height, rows):
    """rows: 每行 BGRA 字节（长度 = width*4）"""
    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter type 0 (None)
        # BMP 是 BGRA，PNG 要 RGBA
        rgba = bytearray(len(row))
        rgba[0::4] = row[2::4]
        rgba[1::4] = row[1::4]
        rgba[2::4] = row[0::4]
        rgba[3::4] = b"\xff" * width
        raw += rgba

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = bytearray(b"\x89PNG\r\n\x1a\n")
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + ".png"
    w, h, rows = read_bmp32(src)
    write_png(dst, w, h, rows)
    print("OK %dx%d -> %s" % (w, h, dst))


if __name__ == "__main__":
    main()
