#!/usr/bin/env python3
"""Generate the Unicode -> GB2312 lookup table used by the e-paper GUI.

The GT30L32S4W font chip is indexed by GB2312 code, while the web page
posts UTF-8 text.  main/gb2312_map.c binary-searches the table produced
here to convert every UTF-8 code point into its GB2312 byte pair.

Output format (little endian, sorted by Unicode code point):
    uint16 unicode, uint16 gb2312   (repeated)

The table is embedded into the firmware through CMakeLists.txt:
    EMBED_FILES "gb2312_map.bin"
and exposed to C as _binary_gb2312_map_bin_start / _end.

Usage (from the project root):
    python tools/gen_gb2312_map.py

Only GB2312 is generated (7445 characters: 682 symbols + 6763 hanzi).
Characters outside GB2312 (GBK extensions, emoji, ...) are rejected by
the checklist API because the font chip cannot draw them.
"""

import pathlib
import struct
import sys


def main() -> int:
    if len(sys.argv) > 1:
        out_path = pathlib.Path(sys.argv[1])
    else:
        out_path = pathlib.Path(__file__).resolve().parent.parent / "main" / "gb2312_map.bin"

    pairs = []
    for zone in range(0xA1, 0xF8):
        for position in range(0xA1, 0xFF):
            try:
                text = bytes((zone, position)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(text) != 1:
                continue
            pairs.append(((ord(text) << 16) | (zone << 8) | position))

    pairs.sort()
    if len({item >> 16 for item in pairs}) != len(pairs):
        raise SystemExit("duplicate unicode code point in GB2312 table")
    if len(pairs) != 7445:
        print(f"warning: expected 7445 GB2312 characters, got {len(pairs)}", file=sys.stderr)

    data = b"".join(struct.pack("<HH", item >> 16, item & 0xFFFF) for item in pairs)
    out_path.write_bytes(data)
    print(f"GB2312 map: {len(pairs)} entries, {len(data)} bytes -> {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
