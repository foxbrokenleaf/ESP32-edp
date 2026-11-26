/*
 * Host-side (PC) unit test for gb2312_map.c:
 *   - embedded Unicode -> GB2312 table (sorted / complete / correct)
 *   - Gb2312_FromUnicode()      binary search lookup
 *   - Gb2312_Utf8Next()         UTF-8 decoder (strict, BMP only)
 *   - Gb2312_FromUtf8()         whole-string conversion used by the e-paper GUI
 *
 * Build & run from the project root (MinGW gcc):
 *   gcc -std=gnu11 -Wall -Wextra -I main tools/test_gb2312_map.c main/gb2312_map.c \
 *       -o build/test_gb2312_map.exe
 *   .\build\test_gb2312_map.exe
 *
 * In the firmware main/gb2312_map.bin is embedded by CMakeLists.txt (EMBED_FILES)
 * and the linker creates the _binary_gb2312_map_bin_start/_end symbols; here the
 * same symbols are produced with the GAS .incbin directive so that the test runs
 * the very same data and code as the firmware.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gb2312_map.h"

__asm__(
    "    .section .rodata\n"
    "    .balign 4\n"
    "    .globl _binary_gb2312_map_bin_start\n"
    "_binary_gb2312_map_bin_start:\n"
    "    .incbin \"main/gb2312_map.bin\"\n"
    "    .globl _binary_gb2312_map_bin_end\n"
    "_binary_gb2312_map_bin_end:\n"
    "    .byte 0\n"
    "    .text\n");

extern const uint8_t gb2312_map_bin_start[] asm("_binary_gb2312_map_bin_start");
extern const uint8_t gb2312_map_bin_end[] asm("_binary_gb2312_map_bin_end");

#define STRICT_GB2312_CHARS  7445   /* 682 symbols + 6763 hanzi */

static int g_checks = 0;
static int g_failed = 0;

static void check(int cond, const char *what)
{
    g_checks++;
    if (!cond) {
        g_failed++;
        printf("  [FAIL] %s\n", what);
    }
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ------------------------------------------------------------------ table */

static void test_table(void)
{
    size_t size = (size_t)(gb2312_map_bin_end - gb2312_map_bin_start);
    size_t count = Gb2312_MapCount();
    size_t i;

    printf("[1] embedded table\n");
    check(size % 4 == 0, "table size is a multiple of 4");
    check(count == size / 4, "Gb2312_MapCount() equals size / 4");
    check(count == STRICT_GB2312_CHARS, "table holds the 7445 strict GB2312 chars");

    for (i = 1; i < count; i++) {
        if (read_u16(gb2312_map_bin_start + i * 4) <=
            read_u16(gb2312_map_bin_start + (i - 1) * 4)) {
            break;
        }
    }
    check(i == count, "table is sorted ascending by unicode (binary search works)");
}

/* ----------------------------------------------------------------- lookup */

typedef struct {
    const char *utf8;   /* single BMP char, UTF-8 */
    uint16_t    gb;     /* expected GB2312 code (verified with Python's codec) */
} sample_t;

static const sample_t s_samples[] = {
    { "清", 0xC7E5 }, { "单", 0xB5A5 }, { "带", 0xB4F8 }, { "钥", 0xD4BF },
    { "匙", 0xB3D7 }, { "完", 0xCDEA }, { "成", 0xB3C9 }, { "课", 0xBFCE },
    { "程", 0xB3CC }, { "作", 0xD7F7 }, { "业", 0xD2B5 }, { "背", 0xB1B3 },
    { "词", 0xB4CA }, { "你", 0xC4E3 }, { "好", 0xBAC3 }, { "中", 0xD6D0 },
    { "一", 0xD2BB }, { "全", 0xC8AB }, { "校", 0xD0A3 }, { "卡", 0xBFA8 },
    { "∑", 0xA1C6 }, { "※", 0xA1F9 }, { "　", 0xA1A1 },
};

static void test_lookup(void)
{
    size_t i;

    printf("[2] Gb2312_FromUnicode()\n");
    for (i = 0; i < sizeof(s_samples) / sizeof(s_samples[0]); i++) {
        const uint8_t *p = (const uint8_t *)s_samples[i].utf8;
        int32_t cp = Gb2312_Utf8Next(&p, p + strlen(s_samples[i].utf8));
        uint16_t gb = 0;
        char msg[96];

        snprintf(msg, sizeof(msg), "lookup of U+%04X -> %04X", (unsigned)cp, s_samples[i].gb);
        check(cp > 0 && Gb2312_FromUnicode((uint32_t)cp, &gb) && gb == s_samples[i].gb, msg);
        check((size_t)(p - (const uint8_t *)s_samples[i].utf8) == strlen(s_samples[i].utf8),
              "sample decoded as a single char");
    }

    /* characters outside strict GB2312 must be rejected */
    {
        static const uint32_t kRejected[] = {
            0x0041,     /* 'A'      : ASCII, handled separately by the renderer */
            0x20AC,     /* euro     : GBK only */
            0x263A,     /* smiley   : not in GB2312 */
            0x2713,     /* check mark: not in GB2312 */
            0x32A3,     /* circled char: not in GB2312 */
            0x9F98,     /* hanzi outside GB2312 */
            0x1F600,    /* emoji: outside BMP */
        };

        for (i = 0; i < sizeof(kRejected) / sizeof(kRejected[0]); i++) {
            uint16_t gb = 0;
            char msg[96];

            snprintf(msg, sizeof(msg), "U+%04X must not be in GB2312", (unsigned)kRejected[i]);
            check(!Gb2312_FromUnicode(kRejected[i], &gb), msg);
        }
    }

    /* defensive API behaviour */
    check(!Gb2312_FromUnicodeData(NULL, 16, 0x4F60, NULL), "NULL pointers rejected");
    {
        uint16_t gb = 0;
        check(!Gb2312_FromUnicodeData(gb2312_map_bin_start, 3, 0x6E05, &gb),
              "partial entry (size < 4) yields no match");
    }
}

/* ---------------------------------------------------------------- decoder */

static void test_utf8_decoder(void)
{
    static const struct {
        const char *text;   /* bytes */
        int32_t     want;   /* expected code point, -1 = invalid */
        size_t      bytes;  /* bytes consumed, 0 = invalid */
    } kCases[] = {
        { "A",            0x0041, 1 },
        { "清",           0x6E05, 3 },
        { "∑",            0x2211, 3 },
        { "\xC2\xA9",     0x00A9, 2 },   /* (c) sign, 2-byte sequence */
        { "\xF0\x9F\x98\x80", -1, 0 },   /* emoji: outside BMP */
        { "\xC0\xAF",         -1, 0 },   /* overlong encoding */
        { "\xED\xA0\x80",     -1, 0 },   /* UTF-16 surrogate */
        { "\xE6\xB8",         -1, 0 },   /* truncated sequence */
        { "\xFF",             -1, 0 },   /* invalid leading byte */
    };
    size_t i;

    printf("[3] Gb2312_Utf8Next()\n");
    for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        const uint8_t *begin = (const uint8_t *)kCases[i].text;
        const uint8_t *p = begin;
        int32_t cp = Gb2312_Utf8Next(&p, begin + strlen(kCases[i].text));
        char msg[96];

        snprintf(msg, sizeof(msg), "decode \"%s\" -> %d (want %d)", kCases[i].text,
                 (int)cp, (int)kCases[i].want);
        check(cp == kCases[i].want, msg);
        if (kCases[i].want >= 0) {
            check((size_t)(p - begin) == kCases[i].bytes, "cursor advanced by the char length");
        } else {
            check(p == begin, "cursor unchanged on invalid input");
        }
    }

    {
        const uint8_t *p = (const uint8_t *)"";
        check(Gb2312_Utf8Next(&p, p) == -1, "empty buffer returns -1");
        check(Gb2312_Utf8Next(NULL, NULL) == -1, "NULL cursor returns -1");
    }
}

/* ------------------------------------------------------------ conversion */

static void test_from_utf8(void)
{
    char out[64];
    size_t n;

    printf("[4] Gb2312_FromUtf8()\n");

    n = Gb2312_FromUtf8("清", out, sizeof(out));
    check(n == 2 && (uint8_t)out[0] == 0xC7 && (uint8_t)out[1] == 0xE5, "single hanzi -> C7 E5");

    n = Gb2312_FromUtf8("清单", out, sizeof(out));
    check(n == 4 && memcmp(out, "\xC7\xE5\xB5\xA5", 4) == 0, "\"qingdan\" -> C7 E5 B5 A5");

    n = Gb2312_FromUtf8("A1 z", out, sizeof(out));
    check(n == 4 && strcmp(out, "A1 z") == 0, "ASCII passes through");

    n = Gb2312_FromUtf8("\xE2\x82\xAC", out, sizeof(out));   /* euro sign */
    check(n == 1 && strcmp(out, "?") == 0, "unsupported char becomes '?'");

    /* invalid UTF-8 stops the conversion instead of emitting garbage */
    n = Gb2312_FromUtf8("\xFF\xFE", out, sizeof(out));
    check(n == 0 && out[0] == '\0', "invalid UTF-8 yields an empty string");

    /* the emoji is dropped, the GB2312 part before it is kept */
    n = Gb2312_FromUtf8("清\xF0\x9F\x98\x80", out, sizeof(out));
    check(n == 2 && (uint8_t)out[0] == 0xC7, "BMP-only: conversion stops at the emoji");

    /* buffer too small: never split a double-byte char */
    memset(out, 0x55, sizeof(out));
    n = Gb2312_FromUtf8("清单", out, 4);
    check(n == 2 && out[2] == '\0', "out_size=4 keeps exactly one GB2312 char");
    n = Gb2312_FromUtf8("清单", out, 3);
    check(n == 2 && out[2] == '\0', "out_size=3 also fits one GB2312 char + NUL");
    n = Gb2312_FromUtf8("清单", out, 2);
    check(n == 0 && out[0] == '\0', "out_size=2 keeps nothing");

    /* defensive API behaviour */
    check(Gb2312_FromUtf8(NULL, out, sizeof(out)) == 0, "NULL input returns 0");
    check(Gb2312_FromUtf8("清", NULL, sizeof(out)) == 0, "NULL output returns 0");
    check(Gb2312_FromUtf8("清", out, 0) == 0, "out_size=0 returns 0");
}

int main(void)
{
    printf("gb2312_map host test\n");
    test_table();
    test_lookup();
    test_utf8_decoder();
    test_from_utf8();

    printf("\n%d checks, %d failed -> %s\n", g_checks, g_failed,
           (g_failed == 0) ? "PASS" : "FAIL");
    return (g_failed == 0) ? 0 : 1;
}
