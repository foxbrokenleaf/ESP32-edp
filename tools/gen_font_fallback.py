#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从字库芯片 ROM 镜像里抽取"本工程实际用到的"字模，生成内置字库回退表。

为什么需要它
------------
板载 GT30L32S4W 若不响应（HOLD# 未拉高 / 未贴片 / 走线断开 / 供电异常），
``GT30L32S4W_SelfTest()`` 必然失败。本脚本把工程真正会用到的字符从
``datasheet/GT30L32S4W.bin``（2 MB 完整 ROM 镜像）里抠出来编译进固件，
芯片不在线时自动回退到内置子集，显示功能不受影响；芯片修好后自动走芯片。

用法（在 epd 目录下执行）::

    python tools/gen_font_fallback.py            # 生成 main/font_fallback_data.c
    python tools/gen_font_fallback.py --list     # 只打印统计，不写文件
    python tools/gen_font_fallback.py --verify   # 额外打印几个字形的字符画

注意：GUI.c / 其它显示用源文件必须保持 GBK 编码（本脚本按 GBK 解析汉字）；
      源文件若是合法 UTF-8（例如只写英文注释的 main.c）则跳过，不参与取码。
"""

import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN_DIR = os.path.join(ROOT, "main")
DEFAULT_ROM = os.path.join(ROOT, "..", "datasheet", "GT30L32S4W.bin")
HEADER_PATH = os.path.join(MAIN_DIR, "GT30L32S4W.h")
OUT_PATH = os.path.join(MAIN_DIR, "font_fallback_data.c")

# 这些文件只"引用"字体枚举、或在注释里出现随意字节，不能用来推断用字
SKIP_FILES = {
    "GT30L32S4W.c", "GT30L32S4W.h",
    "font_fallback.c", "font_fallback.h", "font_fallback_data.c",
}

# 手动补充的字符（GBK 文本），比如以后要用的固定文案；留空即可
EXTRA_GBK_TEXT = ""

# 芯片 ROM 各字库布局（与 GT30L32S4W.c 的 s_font_table 一致）
# name -> (base_addr, record_size, width, height, bytes_per_row, header_size)
FONT_SPECS = {
    "ASCII_5X7":       (0x1DDF80, 8,    5,  8,  1, 0),
    "ASCII_7X8":       (0x1DE280, 8,    7,  8,  1, 0),
    "ASCII_6X12":      (0x1DBE00, 12,   6,  12, 1, 0),
    "ASCII_8X16":      (0x1DD780, 16,   8,  16, 1, 0),
    "ASCII_12X24":     (0x1DFF00, 48,   12, 24, 2, 0),
    "ASCII_16X32":     (0x1E5A50, 64,   16, 32, 2, 0),
    "ASCII_12_ARIAL":  (0x1DC400, 26,   12, 12, 2, 2),
    "ASCII_12_TIMES":  (0x1DCDC0, 26,   12, 12, 2, 2),
    "ASCII_16_ARIAL":  (0x1DE580, 34,   16, 16, 2, 2),
    "ASCII_16_TIMES":  (0x1DF240, 34,   16, 16, 2, 2),
    "ASCII_24_ARIAL":  (0x1E22D0, 74,   24, 24, 3, 2),
    "ASCII_24_TIMES":  (0x1E3E90, 74,   24, 24, 3, 2),
    "ASCII_32_ARIAL":  (0x1E99D0, 130,  32, 32, 4, 2),
    "ASCII_32_TIMES":  (0x1ECA90, 130,  32, 32, 4, 2),
    "GB2312_12X12":    (0x000000, 24,   12, 12, 2, 0),
    "GB2312_16X16":    (0x02C9D0, 32,   16, 16, 2, 0),
    "GB2312_24X24":    (0x068190, 72,   24, 24, 3, 0),
    "GB2312_32X32":    (0x0EDF00, 128,  32, 32, 4, 0),
    "GB_SPEC_8X16":    (0x1F2880, 16,   8,  16, 1, 0),
}

ASCII_FIRST = 0x20
ASCII_COUNT = 95                     # 0x20 ~ 0x7E
GB_SYMBOL_COUNT = 846                # 区码 0xA1~0xA9
GB_TOTAL_COUNT = 846 + 6763          # + 汉字区 0xB0~0xF7
GB_SPEC_FIRST = 0xACA1
GB_SPEC_LAST = 0xACDF


def is_gb2312_font(name):
    return name.startswith("GB")


def parse_font_enum(header_path):
    """从 GT30L32S4W.h 解析 gt30l32s4w_font_t 枚举名 -> 数值。"""
    with open(header_path, "r", encoding="utf-8", errors="replace") as fp:
        text = fp.read()
    body = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*gt30l32s4w_font_t", text, re.S)
    if body is None:
        raise SystemExit("无法在 %s 里找到 gt30l32s4w_font_t 枚举" % header_path)
    names = re.findall(r"GT30L32S4W_FONT_([A-Za-z0-9_]+)", body.group(1))
    return {n: i for i, n in enumerate(names)}


def source_files():
    files = sorted(glob.glob(os.path.join(MAIN_DIR, "*.c")) +
                   glob.glob(os.path.join(MAIN_DIR, "*.h")))
    return [p for p in files if os.path.basename(p) not in SKIP_FILES]


def display_sources(sources):
    """只保留真正调用 GT30L32S4W 显示函数的源文件（显示文本都在这里）。"""
    picked = []
    for path in sources:
        with open(path, "rb") as fp:
            raw = fp.read()
        if b"GT30L32S4W_Show" in raw:
            picked.append(path)
    return picked


def detect_fonts(sources):
    """扫描源码里出现的 GT30L32S4W_FONT_xxx，得到工程实际用到的字体。"""
    used = set()
    for path in sources:
        with open(path, "rb") as fp:
            raw = fp.read()
        for name in re.findall(rb"GT30L32S4W_FONT_([A-Z0-9_]+)", raw):
            name = name.decode("ascii")
            if name in FONT_SPECS:
                used.add(name)
    return [n for n in FONT_SPECS if n in used]      # 按 FONT_SPECS 顺序输出


def string_literals(raw):
    """逐字节扫描 C 源码，返回所有双引号字符串字面量解码后的 bytes。

    这里不用正则：必须按字节处理，才能正确处理 GBK 汉字与 ``\\xAC\\xC0``
    这类转义写法（编译器把它们变成原始字节，字库查表也按原始字节走）。
    """
    out = []
    i, n = 0, len(raw)
    while i < n:
        if raw[i] != 0x22:                       # 不是 '"'
            i += 1
            continue
        i += 1
        buf = bytearray()
        while i < n:
            c = raw[i]
            if c == 0x22:                        # 字面量结束
                i += 1
                break
            if c == 0x5C and i + 1 < n:          # 转义
                e = raw[i + 1]
                if e in (0x78, 0x58) and i + 3 < n:      # \xHH
                    try:
                        buf.append(int(raw[i + 2:i + 4].decode("ascii"), 16))
                        i += 4
                        continue
                    except ValueError:
                        pass
                if 0x30 <= e <= 0x37:            # 八进制 \NNN
                    j, val = i + 1, 0
                    while j < n and j < i + 4 and 0x30 <= raw[j] <= 0x37:
                        val = val * 8 + (raw[j] - 0x30)
                        j += 1
                    buf.append(val & 0xFF)
                    i = j
                    continue
                buf.append({0x6E: 10, 0x74: 9, 0x72: 13, 0x30: 0}.get(e, e))
                i += 2
                continue
            buf.append(c)
            i += 1
        out.append(bytes(buf))
    return out


def scan_gbk_codes(display_files):
    """从显示源文件的字符串字面量里取 GBK 双字节码。"""
    codes = set()
    for path in display_files:
        with open(path, "rb") as fp:
            literals = string_literals(fp.read())
        for lit in literals:
            i = 0
            while i < len(lit):
                lead = lit[i]
                if lead < 0x81 or i + 1 >= len(lit):
                    i += 1
                    continue
                trail = lit[i + 1]
                if 0x40 <= trail <= 0xFE and trail != 0x7F:
                    codes.add((lead << 8) | trail)
                    i += 2
                else:
                    i += 1
    for ch in EXTRA_GBK_TEXT:
        b = ch.encode("gbk")
        if len(b) == 2:
            codes.add((b[0] << 8) | b[1])
    return codes


def gb_index(name, code):
    """GBK 双字节码 -> 字库索引；不在该字库覆盖范围内返回 None。"""
    if name == "GB_SPEC_8X16":
        return code - GB_SPEC_FIRST if GB_SPEC_FIRST <= code <= GB_SPEC_LAST else None
    qu, wei = code >> 8, code & 0xFF
    if wei < 0xA1 or wei > 0xFE:
        return None
    if 0xA1 <= qu <= 0xA9:
        return (qu - 0xA1) * 94 + (wei - 0xA1)
    if 0xB0 <= qu <= 0xF7:
        return GB_SYMBOL_COUNT + (qu - 0xB0) * 94 + (wei - 0xA1)
    return None


def build(rom, enum, fonts, gb_codes, stats):
    """生成 (entries, blob)；entries 已按 (font, code) 升序，便于二分查找。"""
    entries = []
    blob = bytearray()
    for name in fonts:
        base, record, width, height, bpr, header = FONT_SPECS[name]
        font_value = enum[name]
        if is_gb2312_font(name):
            wanted = []
            for code in sorted(gb_codes):
                idx = gb_index(name, code)
                if idx is not None:
                    wanted.append((code, idx))
        else:
            wanted = [(ch, ch - ASCII_FIRST)
                      for ch in range(ASCII_FIRST, ASCII_FIRST + ASCII_COUNT)]
        for code, idx in wanted:
            addr = base + idx * record
            if addr + record > len(rom):
                raise SystemExit("地址越界: %s code=0x%04X addr=0x%06X" % (name, code, addr))
            entries.append((font_value, code, len(blob), record))
            blob += rom[addr:addr + record]
        stats.append((name, len(wanted), len(wanted) * record))
    entries.sort(key=lambda e: (e[0], e[1]))
    return entries, blob


def render_glyph(record, width, height, bpr, header):
    lines = []
    for row in range(height):
        line = record[header + row * bpr: header + (row + 1) * bpr]
        bits = "".join("#" if (line[c >> 3] >> (7 - (c & 7))) & 1 else "." for c in range(width))
        lines.append(bits)
    return "\n".join(lines)


def verify(rom):
    """打印几个字形，确认 ROM 地址 / 记录长度 / 位序都正确。"""
    samples = [
        ("ASCII_8X16", "A"), ("ASCII_16X32", "A"), ("ASCII_32_ARIAL", "A"),
        ("ASCII_32_ARIAL", "8"), ("ASCII_32_ARIAL", "/"), ("GB2312_32X32", "无"),
        ("GB2312_16X16", "第"), ("GB_SPEC_8X16", None),
    ]
    for name, ch in samples:
        base, record, width, height, bpr, header = FONT_SPECS[name]
        if ch is None:
            idx = 0xACC0 - GB_SPEC_FIRST
            label = "0xACC0"
        elif is_gb2312_font(name):
            raw = ch.encode("gbk")
            code = (raw[0] << 8) | raw[1]
            idx = gb_index(name, code)
            label = "0x%04X" % code
        else:
            idx = ord(ch) - ASCII_FIRST
            label = "'%s'" % ch
        addr = base + idx * record
        rec = rom[addr:addr + record]
        extra = ""
        if header:
            extra = " (width=%d)" % ((rec[0] << 8) | rec[1])
        print("--- %s %s @0x%06X%s" % (name, label, addr, extra))
        print(render_glyph(rec, width, height, bpr, header))


def write_out(entries, blob):
    lines = []
    lines.append("/*")
    lines.append(" * 本文件由 tools/gen_font_fallback.py 自动生成，请勿手工修改！")
    lines.append(" *")
    lines.append(" * 数据来源：datasheet/GT30L32S4W.bin（字库芯片 2MB ROM 镜像）")
    lines.append(" * 用途：字库芯片不在线时，GT30L32S4W.c 自动回退到这里的字模子集。")
    lines.append(" * 重新生成：在 epd 目录执行  python tools/gen_font_fallback.py")
    lines.append(" */")
    lines.append("")
    lines.append('#include "font_fallback.h"')
    lines.append("")
    lines.append("/* 所有字形的点阵数据首尾相连，索引项用 offset/length 定位 */")
    lines.append("const uint8_t g_font_fallback_data[] = {")
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        lines.append("    " + " ".join("0x%02X," % b for b in chunk))
    lines.append("};")
    lines.append("const unsigned g_font_fallback_data_size = sizeof(g_font_fallback_data);")
    lines.append("")
    lines.append("/* 按 (font, code) 升序排列；font 是 gt30l32s4w_font_t 枚举值，")
    lines.append(" * code 是 ASCII 字符值或 GBK 双字节码 */")
    lines.append("const font_fallback_entry_t g_font_fallback_index[] = {")
    for value, code, offset, length in entries:
        lines.append("    { %2d, 0x%04X, %6d, %3d }," % (value, code, offset, length))
    lines.append("};")
    lines.append("const unsigned g_font_fallback_index_count =")
    lines.append("    sizeof(g_font_fallback_index) / sizeof(g_font_fallback_index[0]);")
    lines.append("")
    with open(OUT_PATH, "w", encoding="utf-8", newline="\r\n") as fp:
        fp.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description="生成字库芯片内置回退表")
    ap.add_argument("--rom", default=DEFAULT_ROM, help="GT30L32S4W.bin 路径")
    ap.add_argument("--list", action="store_true", help="只打印统计，不写文件")
    ap.add_argument("--verify", action="store_true", help="打印几个字形做目视校验")
    ap.add_argument("--fonts", default="", help="强制指定字体名，逗号分隔（默认自动探测）")
    args = ap.parse_args()

    if not os.path.isfile(args.rom):
        raise SystemExit("找不到 ROM 镜像: %s" % args.rom)
    with open(args.rom, "rb") as fp:
        rom = fp.read()

    enum = parse_font_enum(HEADER_PATH)
    sources = source_files()
    display_files = display_sources(sources)
    if not display_files:
        raise SystemExit("没有找到调用 GT30L32S4W_Show* 的源文件，无法推断用字")
    gb_codes = scan_gbk_codes(display_files)

    if args.fonts:
        fonts = [n.strip() for n in args.fonts.split(",") if n.strip()]
        for n in fonts:
            if n not in FONT_SPECS:
                raise SystemExit("未知字体名: %s" % n)
    else:
        fonts = detect_fonts(sources)
    if not fonts:
        raise SystemExit("没有探测到任何 GT30L32S4W_FONT_xxx 用法")

    print("ROM     : %s (%d bytes)" % (os.path.normpath(args.rom), len(rom)))
    print("字体    : %s" % ", ".join(fonts))
    print("取字文件: %s" % ", ".join(os.path.basename(p) for p in display_files))
    print("GBK取码 : %d 个双字节码（GB2312 汉字/符号 + 0xAC 特殊字符区）" % len(gb_codes))

    if args.verify:
        verify(rom)

    stats = []
    entries, blob = build(rom, enum, fonts, gb_codes, stats)
    for name, count, size in stats:
        print("  %-16s %4d 字形 %6d 字节" % (name, count, size))
    print("合计    : %d 字形 / %d 字节数据" % (len(entries), len(blob)))

    if args.list:
        return
    write_out(entries, blob)
    print("已写出  : %s" % os.path.normpath(OUT_PATH))


if __name__ == "__main__":
    sys.exit(main())
