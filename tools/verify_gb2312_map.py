"""对照 Python 内置的严格 GB2312 编解码表，校验 main/gb2312_map.bin。

用法（在项目根目录执行）：

    python tools/verify_gb2312_map.py [main/gb2312_map.bin]

检查项：
    1. 文件长度是 4 的整数倍，条目数等于严格 GB2312 的字符数（7445）；
    2. 表按 Unicode 严格升序（Gb2312_FromUnicode() 二分查找的前提）；
    3. 表里每个条目的 GB2312 编码能被 Python 解码成同一个字符（没有错位）；
    4. Python 能编码的 GB2312 字符在表里都存在（没有漏项）。
"""
import os
import struct
import sys

# 严格 GB2312：区 1~87、位 1~94，即 0xA1A1 ~ 0xF7FE
ZONE_START, ZONE_END = 0xA1, 0xF7
POS_START, POS_END = 0xA1, 0xFE


def python_gb2312_table():
    """用 Python 的 gb2312 编解码器生成 [(unicode, gb2312), ...]。"""
    table = []
    for hi in range(ZONE_START, ZONE_END + 1):
        for lo in range(POS_START, POS_END + 1):
            raw = bytes((hi, lo))
            try:
                char = raw.decode('gb2312')
            except UnicodeDecodeError:
                continue
            table.append((ord(char), (hi << 8) | lo))
    return table


def load_bin(path):
    with open(path, 'rb') as fp:
        data = fp.read()
    if len(data) % 4 != 0:
        raise SystemExit('bin 长度不是 4 的整数倍（%d 字节）' % len(data))
    return [struct.unpack_from('<HH', data, off) for off in range(0, len(data), 4)]


def main():
    default_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                'main', 'gb2312_map.bin')
    path = sys.argv[1] if len(sys.argv) > 1 else default_path

    entries = load_bin(path)
    expected = python_gb2312_table()
    expected_map = dict(expected)

    print('文件          : %s' % path)
    print('表条目数      : %d' % len(entries))
    print('Python GB2312 : %d' % len(expected))

    ok = True

    if len(entries) != len(expected_map):
        print('[FAIL] 条目数与 Python 的 GB2312 字符数不一致')
        ok = False

    ascending = all(entries[i][0] < entries[i + 1][0] for i in range(len(entries) - 1))
    if not ascending:
        print('[FAIL] 表没有按 Unicode 严格升序排列')
        ok = False

    duplicated = len({u for u, _ in entries}) != len(entries)
    if duplicated:
        print('[FAIL] 表里有重复的 Unicode 码点')
        ok = False

    wrong = [(u, gb, expected_map.get(u)) for u, gb in entries if expected_map.get(u) != gb]
    if wrong:
        print('[FAIL] %d 个条目的 GB2312 编码与 Python 不一致，前 10 个：' % len(wrong))
        for unicode_value, got, want in wrong[:10]:
            print('       U+%04X 表里=%04X Python=%s' % (unicode_value, got,
                                                          '缺失' if want is None else '%04X' % want))
        ok = False

    present = {u for u, _ in entries}
    missing = sorted(set(expected_map) - present)
    if missing:
        print('[FAIL] 有 %d 个 GB2312 字符没有写进表，前 10 个：' % len(missing))
        for unicode_value in missing[:10]:
            print('       U+%04X -> %04X (%s)' % (unicode_value, expected_map[unicode_value],
                                                   chr(unicode_value)))
        ok = False

    print('结果          : %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
