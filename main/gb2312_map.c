/*
 * @Description: Unicode -> GB2312 映射表实现（二分查找）。
 *
 * 映射数据是 main/gb2312_map.bin，由 tools/gen_gb2312_map.py 生成：
 *   - 每一项 4 字节，小端：uint16 unicode + uint16 gb2312
 *   - 按 unicode 升序排列，未定义的 GB2312 位置不写入
 *
 * CMakeLists.txt 使用 EMBED_FILES 把该文件嵌进固件，
 * 链接器生成 _binary_gb2312_map_bin_start / _end 两个符号。
 */
#include "gb2312_map.h"

#include <string.h>

/* EMBED_FILES "gb2312_map.bin" 生成的数据段符号 */
extern const uint8_t gb2312_map_bin_start[] asm("_binary_gb2312_map_bin_start");
extern const uint8_t gb2312_map_bin_end[] asm("_binary_gb2312_map_bin_end");

#define GB2312_MAP_ENTRY_SIZE   4

static uint16_t gb2312_map_read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool Gb2312_FromUnicodeData(const uint8_t *map, size_t map_size,
                            uint32_t unicode, uint16_t *gb_code)
{
    size_t count;
    size_t left;
    size_t right;

    if (map == NULL || gb_code == NULL || unicode > 0xFFFFU) {
        return false;
    }

    count = map_size / GB2312_MAP_ENTRY_SIZE;
    left  = 0;
    right = count;

    while (left < right) {
        const uint8_t *entry;
        uint16_t entry_unicode;
        size_t mid = left + (right - left) / 2;

        entry = map + mid * GB2312_MAP_ENTRY_SIZE;
        entry_unicode = gb2312_map_read_u16(entry);

        if (entry_unicode == (uint16_t)unicode) {
            *gb_code = gb2312_map_read_u16(entry + 2);
            return true;
        }
        if (entry_unicode < (uint16_t)unicode) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    return false;
}

bool Gb2312_FromUnicode(uint32_t unicode, uint16_t *gb_code)
{
    size_t map_size = (size_t)(gb2312_map_bin_end - gb2312_map_bin_start);

    return Gb2312_FromUnicodeData(gb2312_map_bin_start, map_size, unicode, gb_code);
}

size_t Gb2312_MapCount(void)
{
    size_t map_size = (size_t)(gb2312_map_bin_end - gb2312_map_bin_start);

    return map_size / GB2312_MAP_ENTRY_SIZE;
}

int32_t Gb2312_Utf8Next(const uint8_t **cursor, const uint8_t *end)
{
    const uint8_t *p;
    uint32_t cp;
    size_t extra;
    uint8_t first;

    if (cursor == NULL || *cursor == NULL) {
        return -1;
    }

    p = *cursor;
    if (p >= end) {
        return -1;
    }

    first = *p++;
    if (first < 0x80) {
        *cursor = p;
        return (int32_t)first;
    }

    if ((first & 0xE0) == 0xC0) {
        cp = first & 0x1Fu;
        extra = 1;
    } else if ((first & 0xF0) == 0xE0) {
        cp = first & 0x0Fu;
        extra = 2;
    } else if ((first & 0xF8) == 0xF0) {
        cp = first & 0x07u;
        extra = 3;
    } else {
        return -1;
    }

    if ((size_t)(end - p) < extra) {
        return -1;
    }
    for (size_t i = 0; i < extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return -1;
        }
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3Fu);
    }

    /* 拒绝过长编码、代理区以及 BMP 以外的字符（GB2312 全在 BMP 内） */
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
        (extra == 3 && cp < 0x10000) || cp > 0xFFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        return -1;
    }

    *cursor = p + extra;
    return (int32_t)cp;
}

size_t Gb2312_FromUtf8(const char *utf8, char *out, size_t out_size)
{
    const uint8_t *p;
    const uint8_t *end;
    size_t written = 0;

    if (utf8 == NULL || out == NULL || out_size == 0) {
        return 0;
    }

    p = (const uint8_t *)utf8;
    end = p + strlen(utf8);

    while (p < end) {
        int32_t cp = Gb2312_Utf8Next(&p, end);

        if (cp < 0) {
            break;
        }

        if (cp < 0x80) {
            if (written + 1 >= out_size) {
                break;
            }
            out[written++] = (char)cp;
        } else {
            uint16_t gb_code;

            if (!Gb2312_FromUnicode((uint32_t)cp, &gb_code)) {
                if (written + 1 >= out_size) {
                    break;
                }
                out[written++] = '?';
                continue;
            }
            if (written + 3 > out_size) {
                break;
            }
            out[written++] = (char)(gb_code >> 8);
            out[written++] = (char)(gb_code & 0xFFu);
        }
    }

    out[written] = '\0';
    return written;
}
